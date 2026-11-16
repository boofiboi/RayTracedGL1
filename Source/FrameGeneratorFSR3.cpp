// Copyright (c) 2022 Sultim Tsyrendashiev
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "FrameGeneratorFSR3.h"

#ifdef RG_USE_AMD_FSR3

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
    #include <ffx_api.h>
    #include <ffx_api_loader.h>
    #include <ffx_framegeneration.h>
    #include <vk/ffx_api_vk.h>
    #include <vk/ffx_api_framegeneration_vk.h>

    #include "FfxLoaderVK.h"
#else
    #include <ffx_api/ffx_api.h>
    #include <ffx_api/ffx_framegeneration.h>
    #include <ffx_api/vk/ffx_api_vk.h>
#endif

#include "DLSS.h"
#include "MemoryAllocator.h"
#include "Queues.h"
#include "RenderResolutionHelper.h"
#include "RgException.h"

namespace
{
void BarrierDepthAndMotion( VkCommandBuffer cmd,
                            VkImage         depthImage,
                            VkImage         motionImage,
                            bool            toShaderRead )
{
    VkImageMemoryBarrier2 barriers[ 2 ] = {};
    VkImage               images[ 2 ]   = { depthImage, motionImage };

    for( uint32_t i = 0; i < 2; i++ )
    {
        barriers[ i ] = {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask  = toShaderRead
                                 ? ( VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT )
                                 : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = toShaderRead
                                 ? ( VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT )
                                 : VK_ACCESS_2_SHADER_READ_BIT,
            .dstStageMask  = toShaderRead
                                 ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                 : ( VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT ),
            .dstAccessMask = toShaderRead
                                 ? VK_ACCESS_2_SHADER_READ_BIT
                                 : ( VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT ),
            .oldLayout     = toShaderRead ? VK_IMAGE_LAYOUT_GENERAL
                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout     = toShaderRead ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                          : VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = images[ i ],
            .subresourceRange    = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };
    }

    VkDependencyInfo dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers    = barriers,
    };
    RTGL1::svkCmdPipelineBarrier2KHR( cmd, &dep );
}

using CreateContextFn = ffxReturnCode_t ( * )( ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks* );

ffxReturnCode_t SafeCreateFfxContext( CreateContextFn             createFn,
                                      ffxContext*                 context,
                                      ffxCreateContextDescHeader* header )
{
    if( !createFn )
    {
        return FFX_API_RETURN_ERROR;
    }
#if defined( _MSC_VER )
    __try
    {
        return createFn( context, header, nullptr );
    }
    __except( EXCEPTION_EXECUTE_HANDLER )
    {
        return FFX_API_RETURN_ERROR;
    }
#else
    return createFn( context, header, nullptr );
#endif
}
}

struct RTGL1::FrameGeneratorFSR3::Impl
{
    static constexpr uint32_t HUDLESS_IMAGE_COUNT = 4;

    VkDevice                           device{ VK_NULL_HANDLE };
    VkPhysicalDevice                   physDevice{ VK_NULL_HANDLE };
    std::shared_ptr< MemoryAllocator > allocator;
    std::shared_ptr< Queues >          queues;
    VkFormat                           backBufferFormat{ VK_FORMAT_UNDEFINED };

#ifdef _WIN32
    HMODULE                            loaderModule{ nullptr };
    ffxFunctions                       funcs{};
    bool                               loaderAvailable{ false };
#endif

    ffxContext                         fgContext{ nullptr };
    ffxContext                         swapchainContext{ nullptr };
    VkSwapchainKHR                     currentSwapchain{ VK_NULL_HANDLE };

    PFN_vkCreateSwapchainFFXAPI        pfnCreateSwapchainFFX{ nullptr };
    PFN_vkDestroySwapchainFFXAPI       pfnDestroySwapchainFFX{ nullptr };
    PFN_vkGetSwapchainImagesKHR        pfnGetSwapchainImages{ nullptr };
    PFN_vkAcquireNextImageKHR          pfnAcquireNextImage{ nullptr };
    PFN_vkQueuePresentKHR              pfnQueuePresent{ nullptr };

    bool                               fgContextFailed{ false };
    bool                               swapchainContextFailed{ false };
    bool                               swapchainHandleAlive{ false };

    std::atomic< bool >                pendingReset{ false };
    std::mutex                         fgMutex;

    VkImage                            hudlessImages[ HUDLESS_IMAGE_COUNT ]{};
    VkImageView                        hudlessViews[ HUDLESS_IMAGE_COUNT ]{};
    VkDeviceMemory                     hudlessMemories[ HUDLESS_IMAGE_COUNT ]{};
    uint32_t                           currentHudlessIndex{ 0 };
    uint32_t                           lastCapturedHudlessIndex{ 0 };

    std::shared_ptr< class DLSS >      dlss;
    bool                               isDlssGActive{ false };
    bool                               prevFrameGenEnabled{ false };

    VkImage                            savedDepthImage{ VK_NULL_HANDLE };
    VkImageView                        savedDepthView{ VK_NULL_HANDLE };
    VkImage                            savedMotionImage{ VK_NULL_HANDLE };
    VkImageView                        savedMotionView{ VK_NULL_HANDLE };
    float                              savedViewMatrix[ 16 ]{};
    float                              savedCameraNear{ 0.0f };
    float                              savedCameraFar{ 0.0f };
    float                              savedFovVerticalRad{ 0.0f };
    uint32_t                           currentWidth{ 0 };
    uint32_t                           currentHeight{ 0 };
    uint32_t                           savedRenderWidth{ 0 };
    uint32_t                           savedRenderHeight{ 0 };
    uint32_t                           currentFrameIndex{ 0 };

    ffxReturnCode_t CreateContextCall( ffxContext* ctx, ffxCreateContextDescHeader* hdr ) const
    {
#ifdef _WIN32
        return SafeCreateFfxContext( funcs.CreateContext, ctx, hdr );
#else
        return SafeCreateFfxContext( &ffxCreateContext, ctx, hdr );
#endif
    }

    ffxReturnCode_t DestroyContextCall( ffxContext* ctx ) const
    {
#ifdef _WIN32
        return funcs.DestroyContext ? funcs.DestroyContext( ctx, nullptr ) : FFX_API_RETURN_OK;
#else
        return ffxDestroyContext( ctx, nullptr );
#endif
    }

    ffxReturnCode_t ConfigureCall( ffxContext* ctx, const ffxConfigureDescHeader* hdr ) const
    {
#ifdef _WIN32
        return funcs.Configure ? funcs.Configure( ctx, hdr ) : FFX_API_RETURN_ERROR;
#else
        return ffxConfigure( ctx, hdr );
#endif
    }

    ffxReturnCode_t QueryCall( ffxContext* ctx, ffxQueryDescHeader* hdr ) const
    {
#ifdef _WIN32
        return funcs.Query ? funcs.Query( ctx, hdr ) : FFX_API_RETURN_ERROR;
#else
        return ffxQuery( ctx, hdr );
#endif
    }

    ffxReturnCode_t DispatchCall( ffxContext* ctx, ffxDispatchDescHeader* hdr ) const
    {
#ifdef _WIN32
        return funcs.Dispatch ? funcs.Dispatch( ctx, hdr ) : FFX_API_RETURN_ERROR;
#else
        return ffxDispatch( ctx, hdr );
#endif
    }

    void DisableFgLocked( VkSwapchainKHR swapchain )
    {
        if( !fgContext )
        {
            return;
        }

        ffxConfigureDescFrameGeneration cfg = {};
        cfg.header.type                     = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain                       = reinterpret_cast< void* >( swapchain );
        cfg.presentCallback                 = nullptr;
        cfg.frameGenerationCallback         = nullptr;
        cfg.HUDLessColor                    = FfxApiResource{};
        cfg.frameGenerationEnabled          = false;

        ConfigureCall( &fgContext, &cfg.header );
    }

    void WaitForPresentsNoLock()
    {
        if( swapchainContext == nullptr || !swapchainHandleAlive )
        {
            return;
        }

        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK waitDesc = {};
        waitDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
        DispatchCall( &swapchainContext, &waitDesc.header );
    }

    void DestroyFgContextLocked()
    {
        if( fgContext )
        {
            DisableFgLocked( swapchainHandleAlive ? currentSwapchain : VK_NULL_HANDLE );
            DestroyContextCall( &fgContext );
            fgContext = nullptr;
        }
    }

    void DestroyHudlessImages()
    {
        for( uint32_t i = 0; i < HUDLESS_IMAGE_COUNT; i++ )
        {
            if( hudlessViews[ i ] != VK_NULL_HANDLE )
            {
                vkDestroyImageView( device, hudlessViews[ i ], nullptr );
                hudlessViews[ i ] = VK_NULL_HANDLE;
            }
            if( hudlessImages[ i ] != VK_NULL_HANDLE )
            {
                vkDestroyImage( device, hudlessImages[ i ], nullptr );
                hudlessImages[ i ] = VK_NULL_HANDLE;
            }
            if( hudlessMemories[ i ] != VK_NULL_HANDLE )
            {
                MemoryAllocator::FreeDedicated( device, hudlessMemories[ i ] );
                hudlessMemories[ i ] = VK_NULL_HANDLE;
            }
        }
    }

    void CreateHudlessImages( uint32_t width, uint32_t height )
    {
        DestroyHudlessImages();

        const uint32_t graphicsFamily = queues->GetIndexGraphics();
        const uint32_t computeFamily  = queues->GetIndexCompute();
        const uint32_t queueFamilies[ 2 ] = { graphicsFamily, computeFamily };
        const bool     useConcurrent      = graphicsFamily != computeFamily;

        for( uint32_t i = 0; i < HUDLESS_IMAGE_COUNT; i++ )
        {
            VkImageCreateInfo imageInfo = {
                .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType     = VK_IMAGE_TYPE_2D,
                .format        = backBufferFormat,
                .extent        = { width, height, 1 },
                .mipLevels     = 1,
                .arrayLayers   = 1,
                .samples       = VK_SAMPLE_COUNT_1_BIT,
                .tiling        = VK_IMAGE_TILING_OPTIMAL,
                .usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode   = useConcurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = useConcurrent ? 2u : 0u,
                .pQueueFamilyIndices   = useConcurrent ? queueFamilies : nullptr,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };

            VkResult r = vkCreateImage( device, &imageInfo, nullptr, &hudlessImages[ i ] );
            VK_CHECKERROR( r );

            VkMemoryRequirements memReqs = {};
            vkGetImageMemoryRequirements( device, hudlessImages[ i ], &memReqs );

            hudlessMemories[ i ] = allocator->AllocDedicated( memReqs,
                                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                              MemoryAllocator::AllocType::DEFAULT,
                                                              "FSR3 FG HUD-less Image" );

            r = vkBindImageMemory( device, hudlessImages[ i ], hudlessMemories[ i ], 0 );
            VK_CHECKERROR( r );

            VkImageViewCreateInfo viewInfo = {
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image    = hudlessImages[ i ],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format   = backBufferFormat,
                .subresourceRange = {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel   = 0,
                    .levelCount     = 1,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
            };
            r = vkCreateImageView( device, &viewInfo, nullptr, &hudlessViews[ i ] );
            VK_CHECKERROR( r );
        }

        currentHudlessIndex      = 0;
        lastCapturedHudlessIndex = 0;
    }

    void CreateFgContextLocked( uint32_t displayWidth, uint32_t displayHeight )
    {
#ifdef _WIN32
        if( !loaderAvailable )
        {
            fgContextFailed = true;
            return;
        }

        ffxCreateBackendVKDesc backendDesc = {};
        backendDesc.header.type            = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backendDesc.deviceContext          = {
            .vkDevice         = device,
            .vkPhysicalDevice = physDevice,
            .vkDeviceProcAddr = vkGetDeviceProcAddr,
        };

        ffxCreateContextDescFrameGenerationVersion versionDesc = {};
        versionDesc.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        versionDesc.header.pNext = &backendDesc.header;
        versionDesc.version      = FFX_FRAMEGENERATION_VERSION;

        ffxCreateContextDescFrameGenerationHudless hudlessDesc = {};
        hudlessDesc.header.type             = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hudlessDesc.header.pNext            = &versionDesc.header;
        hudlessDesc.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK( backBufferFormat );
#else
        ffxCreateBackendVKDesc backendDesc = {};
        backendDesc.header.type            = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backendDesc.vkDevice               = device;
        backendDesc.vkPhysicalDevice       = physDevice;
        backendDesc.vkDeviceProcAddr       = vkGetDeviceProcAddr;

        ffxCreateContextDescFrameGenerationHudless hudlessDesc = {};
        hudlessDesc.header.type             = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hudlessDesc.header.pNext            = &backendDesc.header;
        hudlessDesc.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK( backBufferFormat );
#endif

        ffxCreateContextDescFrameGeneration createFg = {};
        createFg.header.type      = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        createFg.header.pNext     = &hudlessDesc.header;
        createFg.flags            = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
#ifndef NDEBUG
        createFg.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
#endif
        createFg.displaySize      = { displayWidth, displayHeight };
        createFg.maxRenderSize    = { displayWidth, displayHeight };
        createFg.backBufferFormat = ffxApiGetSurfaceFormatVK( backBufferFormat );

        ffxReturnCode_t ret = CreateContextCall( &fgContext, &createFg.header );
        if( ret != FFX_API_RETURN_OK )
        {
            debug::Warning( "FSR3 FG: ffxCreateContext (frame generation) failed, code {}",
                            static_cast< int >( ret ) );

            if( fgContext )
            {
                DestroyContextCall( &fgContext );
                fgContext = nullptr;
            }

            fgContextFailed = true;
            return;
        }

#ifdef _WIN32
        ffxQueryGetProviderVersion providerVer = {};
        providerVer.header.type                = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if( QueryCall( &fgContext, &providerVer.header ) == FFX_API_RETURN_OK )
        {
            debug::Info( "FSR3 FG: active frame generation provider '{}' (0x{:016X})",
                         providerVer.versionName ? providerVer.versionName : "unknown",
                         providerVer.versionId );
        }
#else
        debug::Info( "FSR3 FG: frame generation context created" );
#endif
    }
};

RTGL1::FrameGeneratorFSR3::FrameGeneratorFSR3( VkDevice                           _device,
                                               VkPhysicalDevice                   _physDevice,
                                               std::shared_ptr< MemoryAllocator > _allocator,
                                               std::shared_ptr< Queues >          _queues,
                                               VkFormat                           _backBufferFormat )
    : impl( std::make_unique< Impl >() )
{
    impl->device           = _device;
    impl->physDevice       = _physDevice;
    impl->allocator        = std::move( _allocator );
    impl->queues           = std::move( _queues );
    impl->backBufferFormat = _backBufferFormat;

#ifdef _WIN32
    if( !LoadFfxApiFunctions( L"amd_fidelityfx_framegeneration_vk.dll",
                              impl->loaderModule,
                              impl->funcs ) )
    {
        debug::Warning( "FSR3 FG: failed to load amd_fidelityfx_loader_vk.dll / amd_fidelityfx_framegeneration_vk.dll, disabling FSR frame generation" );
        impl->fgContextFailed        = true;
        impl->swapchainContextFailed = true;
        return;
    }

    VkDeviceContext vkDevCtx = {
        .vkDevice         = impl->device,
        .vkPhysicalDevice = impl->physDevice,
        .vkDeviceProcAddr = vkGetDeviceProcAddr,
    };

    uint64_t                versionCount = 0;
    ffxQueryDescGetVersions versionQuery = {};
    versionQuery.header.type             = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versionQuery.createDescType          = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    versionQuery.device                  = &vkDevCtx;
    versionQuery.outputCount             = &versionCount;

    ffxReturnCode_t ret = impl->QueryCall( nullptr, &versionQuery.header );
    if( ret != FFX_API_RETURN_OK || versionCount == 0 )
    {
        debug::Warning( "FSR3 FG: no frame generation providers available (code {}, count {})",
                        static_cast< int >( ret ),
                        versionCount );
        impl->fgContextFailed        = true;
        impl->swapchainContextFailed = true;
        return;
    }

    std::vector< uint64_t >    versionIds( versionCount );
    std::vector< const char* > versionNames( versionCount );
    versionQuery.versionIds   = versionIds.data();
    versionQuery.versionNames = versionNames.data();

    if( impl->QueryCall( nullptr, &versionQuery.header ) == FFX_API_RETURN_OK )
    {
        std::string list;
        for( uint64_t i = 0; i < versionCount; i++ )
        {
            if( i > 0 )
            {
                list += ", ";
            }
            list += versionNames[ i ] ? versionNames[ i ] : "unknown";
        }
        debug::Info( "FSR3 FG: available frame generation providers: {}", list );
    }

    impl->loaderAvailable = true;
#endif
}

void RTGL1::FrameGeneratorFSR3::SetDlss( std::shared_ptr< DLSS > _dlss )
{
    impl->dlss = std::move( _dlss );
}

RTGL1::FrameGeneratorFSR3::~FrameGeneratorFSR3()
{
    if( !impl )
    {
        return;
    }

    {
        std::lock_guard< std::mutex > lock( impl->fgMutex );
        impl->WaitForPresentsNoLock();
        impl->DestroyFgContextLocked();

        if( impl->swapchainContext )
        {
            impl->DestroyContextCall( &impl->swapchainContext );
            impl->swapchainContext = nullptr;
        }
    }

    impl->DestroyHudlessImages();

#ifdef _WIN32
    if( impl->loaderModule )
    {
        FreeLibrary( impl->loaderModule );
        impl->loaderModule = nullptr;
    }
#endif
}

bool RTGL1::FrameGeneratorFSR3::IsAvailable() const
{
#ifdef _WIN32
    if( !impl->loaderAvailable )
    {
        return false;
    }
#endif
    return !impl->fgContextFailed && !impl->swapchainContextFailed;
}

RTGL1::IFrameGeneratorSwapchain* RTGL1::FrameGeneratorFSR3::GetSwapchainHooks()
{
    return this;
}

bool RTGL1::FrameGeneratorFSR3::IsActive() const
{
    return impl->swapchainContext != nullptr && impl->swapchainHandleAlive;
}

void RTGL1::FrameGeneratorFSR3::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
    if( resolutionState.upscaledWidth == 0 || resolutionState.upscaledHeight == 0 )
    {
        return;
    }

    std::lock_guard< std::mutex > lock( impl->fgMutex );
    impl->WaitForPresentsNoLock();
    vkDeviceWaitIdle( impl->device );

    impl->DestroyFgContextLocked();
    impl->CreateHudlessImages( resolutionState.upscaledWidth, resolutionState.upscaledHeight );
    impl->CreateFgContextLocked( resolutionState.upscaledWidth, resolutionState.upscaledHeight );
}

VkResult RTGL1::FrameGeneratorFSR3::CreateSwapchain( const VkSwapchainCreateInfoKHR& createInfo,
                                                     VkSwapchainKHR* outSwapchain )
{
#ifdef _WIN32
    if( !impl->loaderAvailable )
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
#endif
    if( impl->swapchainContextFailed )
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    std::lock_guard< std::mutex > lock( impl->fgMutex );

    if( impl->swapchainContext != nullptr )
    {
        if( impl->pfnCreateSwapchainFFX == nullptr )
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkSwapchainCreateInfoKHR ci = createInfo;
        ci.oldSwapchain             = VK_NULL_HANDLE;

        VkResult r = impl->pfnCreateSwapchainFFX(
            impl->device, &ci, nullptr, outSwapchain, static_cast< void* >( impl->swapchainContext ) );
        impl->swapchainHandleAlive = ( r == VK_SUCCESS );
        impl->currentSwapchain     = ( r == VK_SUCCESS ) ? *outSwapchain : VK_NULL_HANDLE;
        return r;
    }

    ffxCreateContextDescFrameGenerationSwapChainModeVK modeDesc = {};
    modeDesc.header.type           = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_MODE_VK;
    modeDesc.composeOnPresentQueue = true;

#ifdef _WIN32
    ffxCreateContextDescFrameGenerationSwapChainVersionVK scVersionDesc = {};
    scVersionDesc.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    scVersionDesc.header.pNext = &modeDesc.header;
    scVersionDesc.version      = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
#endif

    ffxCreateContextDescFrameGenerationSwapChainVK createDesc = {};
    createDesc.header.type             = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
#ifdef _WIN32
    createDesc.header.pNext            = &scVersionDesc.header;
#else
    createDesc.header.pNext            = &modeDesc.header;
#endif
    createDesc.physicalDevice          = impl->physDevice;
    createDesc.device                  = impl->device;
    createDesc.swapchain               = outSwapchain;
    createDesc.createInfo              = createInfo;
    createDesc.createInfo.oldSwapchain = VK_NULL_HANDLE;
    createDesc.allocator               = nullptr;
    createDesc.gameQueue       = { impl->queues->GetGraphics(), impl->queues->GetIndexGraphics(), nullptr };
    createDesc.asyncComputeQueue = { impl->queues->GetCompute(), impl->queues->GetIndexCompute(), nullptr };
    createDesc.presentQueue    = { impl->queues->GetPresentQueue(), impl->queues->GetIndexPresent(), nullptr };
    createDesc.imageAcquireQueue = { impl->queues->GetImageAcquireQueue(),
                                     impl->queues->GetIndexImageAcquire(),
                                     nullptr };

    *outSwapchain = VK_NULL_HANDLE;

    ffxReturnCode_t ret = impl->CreateContextCall( &impl->swapchainContext, &createDesc.header );
    if( ret != FFX_API_RETURN_OK || *outSwapchain == VK_NULL_HANDLE )
    {
        debug::Warning( "FSR3 FG: ffxCreateContext (swapchain) failed, code {}",
                        static_cast< int >( ret ) );

        if( impl->swapchainContext )
        {
            impl->DestroyContextCall( &impl->swapchainContext );
            impl->swapchainContext = nullptr;
        }

        impl->swapchainContextFailed = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    ffxQueryDescSwapchainReplacementFunctionsVK replacementFunctions = {};
    replacementFunctions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;

    ffxReturnCode_t queryRet =
        impl->QueryCall( &impl->swapchainContext, &replacementFunctions.header );

    impl->pfnCreateSwapchainFFX  = replacementFunctions.pOutCreateSwapchainFFXAPI;
    impl->pfnDestroySwapchainFFX = replacementFunctions.pOutDestroySwapchainFFXAPI;
    impl->pfnGetSwapchainImages  = replacementFunctions.pOutGetSwapchainImagesKHR;
    impl->pfnAcquireNextImage    = replacementFunctions.pOutAcquireNextImageKHR;
    impl->pfnQueuePresent        = replacementFunctions.pOutQueuePresentKHR;

    if( queryRet != FFX_API_RETURN_OK || impl->pfnCreateSwapchainFFX == nullptr ||
        impl->pfnDestroySwapchainFFX == nullptr || impl->pfnGetSwapchainImages == nullptr ||
        impl->pfnAcquireNextImage == nullptr || impl->pfnQueuePresent == nullptr )
    {
        debug::Warning( "FSR3 FG: failed to query swapchain replacement functions, code {}",
                        static_cast< int >( queryRet ) );

        impl->DestroyContextCall( &impl->swapchainContext );
        impl->swapchainContext = nullptr;

        impl->swapchainContextFailed = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    impl->swapchainHandleAlive = true;
    impl->currentSwapchain     = *outSwapchain;
    debug::Info( "FSR3 FG: swapchain context created" );
    return VK_SUCCESS;
}

void RTGL1::FrameGeneratorFSR3::DestroySwapchain( VkSwapchainKHR swapchain )
{
    std::lock_guard< std::mutex > lock( impl->fgMutex );
    if( impl->swapchainContext != nullptr && impl->pfnDestroySwapchainFFX != nullptr &&
        swapchain != VK_NULL_HANDLE )
    {
        impl->DisableFgLocked( swapchain );
        impl->WaitForPresentsNoLock();
#ifdef _WIN32
        impl->DestroyContextCall( &impl->swapchainContext );
        impl->swapchainContext = nullptr;
        impl->pfnDestroySwapchainFFX( impl->device, swapchain, nullptr, nullptr );
#else
        impl->pfnDestroySwapchainFFX(
            impl->device, swapchain, nullptr, static_cast< void* >( impl->swapchainContext ) );
        impl->DestroyContextCall( &impl->swapchainContext );
        impl->swapchainContext = nullptr;
#endif
        impl->pfnCreateSwapchainFFX  = nullptr;
        impl->pfnDestroySwapchainFFX = nullptr;
        impl->pfnGetSwapchainImages  = nullptr;
        impl->pfnAcquireNextImage    = nullptr;
        impl->pfnQueuePresent        = nullptr;
        impl->swapchainHandleAlive   = false;
        impl->currentSwapchain       = VK_NULL_HANDLE;
    }
}

VkResult RTGL1::FrameGeneratorFSR3::AcquireNextImage( VkDevice       device_,
                                                      VkSwapchainKHR swapchain,
                                                      uint64_t       timeout,
                                                      VkSemaphore    semaphore,
                                                      uint32_t*      outImageIndex )
{
    return impl->pfnAcquireNextImage(
        device_, swapchain, timeout, semaphore, VK_NULL_HANDLE, outImageIndex );
}

VkResult RTGL1::FrameGeneratorFSR3::QueuePresent( VkQueue                 queue,
                                                  const VkPresentInfoKHR& presentInfo )
{
    std::lock_guard< std::mutex > lock( impl->fgMutex );
    return impl->pfnQueuePresent( queue, &presentInfo );
}

VkResult RTGL1::FrameGeneratorFSR3::GetSwapchainImages( VkDevice       device_,
                                                        VkSwapchainKHR swapchain,
                                                        uint32_t*      pCount,
                                                        VkImage*       pImages )
{
    return impl->pfnGetSwapchainImages( device_, swapchain, pCount, pImages );
}

void RTGL1::FrameGeneratorFSR3::WaitForPresents()
{
    std::lock_guard< std::mutex > lock( impl->fgMutex );
    impl->WaitForPresentsNoLock();
}

void RTGL1::FrameGeneratorFSR3::PrepareFrame(
    VkCommandBuffer                        cmd,
    uint32_t                               frameIndex,
    const std::shared_ptr< Framebuffers >& framebuffers,
    const RenderResolutionHelper&          renderResolution,
    RgFloat2D                              jitterOffset,
    double                                 timeDelta,
    float                                  nearPlane,
    float                                  farPlane,
    float                                  fovVerticalRad,
    bool                                   resetAccumulation,
    const float*                           pView,
    uint32_t                               frameId )
{
    if( resetAccumulation )
    {
        impl->pendingReset.store( true );
    }

    impl->savedCameraNear     = nearPlane;
    impl->savedCameraFar      = farPlane;
    impl->savedFovVerticalRad = fovVerticalRad;
    if( pView != nullptr )
    {
        memcpy( impl->savedViewMatrix, pView, sizeof( impl->savedViewMatrix ) );
    }

    using FI = FramebufferImageIndex;
    FI rs[]  = { FI::FB_IMAGE_INDEX_DEPTH_NDC, FI::FB_IMAGE_INDEX_MOTION_DLSS };
    framebuffers->BarrierMultiple( cmd, frameIndex, rs, Framebuffers::BarrierType::Storage );

    auto [ depthImage, depthView, depthFormat, depthSz ] = framebuffers->GetImageHandles(
        FI::FB_IMAGE_INDEX_DEPTH_NDC, frameIndex, renderResolution.GetResolutionState() );
    auto [ motionImage, motionView, motionFormat, motionSz ] = framebuffers->GetImageHandles(
        FI::FB_IMAGE_INDEX_MOTION_DLSS, frameIndex, renderResolution.GetResolutionState() );

    impl->savedDepthImage   = depthImage;
    impl->savedDepthView    = depthView;
    impl->savedMotionImage  = motionImage;
    impl->savedMotionView   = motionView;
    impl->savedRenderWidth  = renderResolution.Width();
    impl->savedRenderHeight = renderResolution.Height();

    if( impl->isDlssGActive && impl->dlss )
    {
        impl->dlss->ValidateDlssgFeature( cmd,
                                          renderResolution.UpscaledWidth(),
                                          renderResolution.UpscaledHeight(),
                                          impl->backBufferFormat );
        return;
    }

    std::lock_guard< std::mutex > lock( impl->fgMutex );
    if( !impl->fgContext )
    {
        return;
    }

    VkImageCreateInfo depthInfo = {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = depthFormat,
        .extent      = { depthSz.width, depthSz.height, 1 },
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .tiling      = VK_IMAGE_TILING_OPTIMAL,
        .usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
    };
    VkImageCreateInfo motionInfo = {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = motionFormat,
        .extent      = { motionSz.width, motionSz.height, 1 },
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .tiling      = VK_IMAGE_TILING_OPTIMAL,
        .usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
    };

    FfxApiResource depthRes = ffxApiGetResourceVK(
        static_cast< void* >( depthImage ),
        ffxApiGetImageResourceDescriptionVK( depthImage, depthInfo, 0 ),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
    FfxApiResource motionRes = ffxApiGetResourceVK(
        static_cast< void* >( motionImage ),
        ffxApiGetImageResourceDescriptionVK( motionImage, motionInfo, 0 ),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );

    const ResolutionState& rsState = renderResolution.GetResolutionState();

#ifdef _WIN32
    ffxDispatchDescFrameGenerationPrepareV2 prepareDesc = {};
    prepareDesc.header.type             = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepareDesc.frameID                 = frameId;
    prepareDesc.flags                   = 0;
    prepareDesc.commandList             = static_cast< void* >( cmd );
    prepareDesc.renderSize              = { rsState.renderWidth, rsState.renderHeight };
    prepareDesc.jitterOffset            = { -jitterOffset.data[ 0 ], -jitterOffset.data[ 1 ] };
    prepareDesc.motionVectorScale       = { float( rsState.renderWidth ), float( rsState.renderHeight ) };
    prepareDesc.frameTimeDelta          = float( timeDelta * 1000.0 );
    prepareDesc.reset                   = resetAccumulation;
    prepareDesc.cameraNear              = nearPlane;
    prepareDesc.cameraFar               = farPlane;
    prepareDesc.cameraFovAngleVertical  = fovVerticalRad;
    prepareDesc.viewSpaceToMetersFactor = 1.0f;
    prepareDesc.depth                   = depthRes;
    prepareDesc.motionVectors           = motionRes;
    if( pView != nullptr )
    {
        prepareDesc.cameraRight[ 0 ]    = pView[ 0 ];
        prepareDesc.cameraRight[ 1 ]    = pView[ 1 ];
        prepareDesc.cameraRight[ 2 ]    = pView[ 2 ];
        prepareDesc.cameraUp[ 0 ]       = pView[ 4 ];
        prepareDesc.cameraUp[ 1 ]       = pView[ 5 ];
        prepareDesc.cameraUp[ 2 ]       = pView[ 6 ];
        prepareDesc.cameraForward[ 0 ]  = -pView[ 8 ];
        prepareDesc.cameraForward[ 1 ]  = -pView[ 9 ];
        prepareDesc.cameraForward[ 2 ]  = -pView[ 10 ];
        prepareDesc.cameraPosition[ 0 ] = pView[ 12 ];
        prepareDesc.cameraPosition[ 1 ] = pView[ 13 ];
        prepareDesc.cameraPosition[ 2 ] = pView[ 14 ];
    }
#else
    ffxDispatchDescFrameGenerationPrepare prepareDesc = {};
    prepareDesc.header.type             = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepareDesc.frameID                 = frameId;
    prepareDesc.flags                   = 0;
    prepareDesc.commandList             = static_cast< void* >( cmd );
    prepareDesc.renderSize              = { rsState.renderWidth, rsState.renderHeight };
    prepareDesc.jitterOffset            = { -jitterOffset.data[ 0 ], -jitterOffset.data[ 1 ] };
    prepareDesc.motionVectorScale       = { float( rsState.renderWidth ), float( rsState.renderHeight ) };
    prepareDesc.frameTimeDelta          = float( timeDelta * 1000.0 );
    prepareDesc.cameraNear              = nearPlane;
    prepareDesc.cameraFar               = farPlane;
    prepareDesc.cameraFovAngleVertical  = fovVerticalRad;
    prepareDesc.viewSpaceToMetersFactor = 1.0f;
    prepareDesc.depth                   = depthRes;
    prepareDesc.motionVectors           = motionRes;

    ffxDispatchDescFrameGenerationPrepareCameraInfo cameraInfo = {};
    if( pView != nullptr )
    {
        cameraInfo.header.type        = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        cameraInfo.cameraRight[ 0 ]   = pView[ 0 ];
        cameraInfo.cameraRight[ 1 ]   = pView[ 1 ];
        cameraInfo.cameraRight[ 2 ]   = pView[ 2 ];
        cameraInfo.cameraUp[ 0 ]      = pView[ 4 ];
        cameraInfo.cameraUp[ 1 ]      = pView[ 5 ];
        cameraInfo.cameraUp[ 2 ]      = pView[ 6 ];
        cameraInfo.cameraForward[ 0 ] = -pView[ 8 ];
        cameraInfo.cameraForward[ 1 ] = -pView[ 9 ];
        cameraInfo.cameraForward[ 2 ] = -pView[ 10 ];

        cameraInfo.cameraPosition[ 0 ] = pView[ 12 ];
        cameraInfo.cameraPosition[ 1 ] = pView[ 13 ];
        cameraInfo.cameraPosition[ 2 ] = pView[ 14 ];

        prepareDesc.header.pNext = &cameraInfo.header;
    }
#endif

    BarrierDepthAndMotion( cmd, depthImage, motionImage, true );

    ffxReturnCode_t ret = impl->DispatchCall( &impl->fgContext, &prepareDesc.header );

    BarrierDepthAndMotion( cmd, depthImage, motionImage, false );

    if( ret != FFX_API_RETURN_OK )
    {
        debug::Warning( "FSR3 FG: ffxDispatch (prepare) failed, code {}", static_cast< int >( ret ) );
    }
}

void RTGL1::FrameGeneratorFSR3::CaptureHudless( VkCommandBuffer cmd,
                                                uint32_t        frameIndex,
                                                VkImage         srcImage,
                                                uint32_t        width,
                                                uint32_t        height )
{
    const uint32_t imgIdx = impl->currentHudlessIndex;
    if( impl->hudlessImages[ imgIdx ] == VK_NULL_HANDLE )
    {
        return;
    }

    impl->lastCapturedHudlessIndex = imgIdx;
    impl->currentHudlessIndex      = ( imgIdx + 1 ) % Impl::HUDLESS_IMAGE_COUNT;

    VkImageMemoryBarrier2 barriers[ 2 ] = {};

    barriers[ 0 ] = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image         = impl->hudlessImages[ imgIdx ],
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };

    barriers[ 1 ] = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image         = srcImage,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };

    VkDependencyInfo dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers    = barriers,
    };
    svkCmdPipelineBarrier2KHR( cmd, &dep );

    VkImageBlit blitRegion = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets     = { { 0, 0, 0 }, { int32_t( width ), int32_t( height ), 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets     = { { 0, 0, 0 }, { int32_t( width ), int32_t( height ), 1 } },
    };
    vkCmdBlitImage( cmd,
                    srcImage,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    impl->hudlessImages[ imgIdx ],
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &blitRegion,
                    VK_FILTER_NEAREST );

    barriers[ 0 ].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barriers[ 0 ].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barriers[ 0 ].dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[ 0 ].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barriers[ 0 ].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[ 0 ].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    barriers[ 1 ].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barriers[ 1 ].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[ 1 ].dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[ 1 ].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    barriers[ 1 ].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[ 1 ].newLayout     = VK_IMAGE_LAYOUT_GENERAL;

    svkCmdPipelineBarrier2KHR( cmd, &dep );
}

void RTGL1::FrameGeneratorFSR3::ConfigureFrame( VkSwapchainKHR swapchain,
                                                uint32_t       frameIndex,
                                                uint32_t       width,
                                                uint32_t       height,
                                                uint32_t       frameId,
                                                bool           frameGenEnabled,
                                                bool           useDlssG )
{
    bool newDlssG = frameGenEnabled && useDlssG;
    if( impl->isDlssGActive != newDlssG || ( frameGenEnabled && !impl->prevFrameGenEnabled ) )
    {
        impl->pendingReset.store( true );
    }
    impl->prevFrameGenEnabled = frameGenEnabled;
    impl->isDlssGActive       = newDlssG;
    impl->currentWidth        = width;
    impl->currentHeight       = height;
    impl->currentFrameIndex   = frameIndex;
    impl->currentSwapchain    = swapchain;

    std::lock_guard< std::mutex > lock( impl->fgMutex );
    if( !impl->fgContext || !impl->swapchainContext )
    {
        return;
    }

    FfxApiResource hudlessRes = {};
    VkImage        hudlessImg = impl->hudlessImages[ impl->lastCapturedHudlessIndex ];
    if( frameGenEnabled && hudlessImg != VK_NULL_HANDLE )
    {
        VkImageCreateInfo hudlessInfo = {
            .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType   = VK_IMAGE_TYPE_2D,
            .format      = impl->backBufferFormat,
            .extent      = { width, height, 1 },
            .mipLevels   = 1,
            .arrayLayers = 1,
            .samples     = VK_SAMPLE_COUNT_1_BIT,
            .tiling      = VK_IMAGE_TILING_OPTIMAL,
            .usage       = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT,
        };
        hudlessRes = ffxApiGetResourceVK(
            static_cast< void* >( hudlessImg ),
            ffxApiGetImageResourceDescriptionVK( hudlessImg, hudlessInfo, 0 ),
            FFX_API_RESOURCE_STATE_COMPUTE_READ );
    }

    ffxConfigureDescFrameGeneration configDesc = {};
    configDesc.header.type                     = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configDesc.swapChain                       = reinterpret_cast< void* >( swapchain );
    configDesc.presentCallback                 = nullptr;
    configDesc.frameGenerationCallback =
        []( ffxDispatchDescFrameGeneration* params, void* pUserCtx ) -> ffxReturnCode_t {
        auto* self = static_cast< Impl* >( pUserCtx );

        if( self->pendingReset.exchange( false ) )
        {
            params->reset = true;
        }

        if( self->isDlssGActive && self->dlss )
        {
            VkCommandBuffer cb = static_cast< VkCommandBuffer >( params->commandList );
            VkImage outInterpolatedImage = static_cast< VkImage >( params->outputs[ 0 ].resource );
            VkImage realFrameImage       = static_cast< VkImage >( params->presentColor.resource );

            bool ok = self->dlss->EvaluateDlssg(
                cb,
                realFrameImage,
                outInterpolatedImage,
                self->savedDepthImage,
                self->savedDepthView,
                self->savedMotionImage,
                self->savedMotionView,
                VK_NULL_HANDLE,
                VK_NULL_HANDLE,
                self->currentWidth,
                self->currentHeight,
                self->savedRenderWidth,
                self->savedRenderHeight,
                self->backBufferFormat,
                self->savedViewMatrix,
                self->savedCameraNear,
                self->savedCameraFar,
                self->savedFovVerticalRad,
                params->reset );

            return ok ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR;
        }

        return self->DispatchCall( &self->fgContext, &params->header );
    };
    configDesc.frameGenerationCallbackUserContext = impl.get();
    configDesc.frameGenerationEnabled             = frameGenEnabled;
    configDesc.allowAsyncWorkloads                = true;
    configDesc.HUDLessColor                       = hudlessRes;
    configDesc.flags                              = 0;
    configDesc.onlyPresentGenerated               = false;
    configDesc.generationRect                     = { 0, 0, int32_t( width ), int32_t( height ) };
    configDesc.frameID                            = frameId;

    ffxReturnCode_t ret = impl->ConfigureCall( &impl->fgContext, &configDesc.header );
    if( ret != FFX_API_RETURN_OK )
    {
        debug::Warning( "FSR3 FG: ffxConfigure failed, code {}", static_cast< int >( ret ) );
    }
}

#else

struct RTGL1::FrameGeneratorFSR3::Impl
{
};

RTGL1::FrameGeneratorFSR3::FrameGeneratorFSR3( VkDevice                           _device,
                                               VkPhysicalDevice                   _physDevice,
                                               std::shared_ptr< MemoryAllocator > _allocator,
                                               std::shared_ptr< Queues >          _queues,
                                               VkFormat                           _backBufferFormat )
{
}

RTGL1::FrameGeneratorFSR3::~FrameGeneratorFSR3() {}

bool RTGL1::FrameGeneratorFSR3::IsAvailable() const
{
    return false;
}

RTGL1::IFrameGeneratorSwapchain* RTGL1::FrameGeneratorFSR3::GetSwapchainHooks()
{
    return nullptr;
}

bool RTGL1::FrameGeneratorFSR3::IsActive() const
{
    return false;
}

void RTGL1::FrameGeneratorFSR3::OnFramebuffersSizeChange( const ResolutionState& resolutionState ) {}

void RTGL1::FrameGeneratorFSR3::PrepareFrame( VkCommandBuffer                        cmd,
                                              uint32_t                               frameIndex,
                                              const std::shared_ptr< Framebuffers >& framebuffers,
                                              const RenderResolutionHelper&          renderResolution,
                                              RgFloat2D      jitterOffset,
                                              double           timeDelta,
                                              float            nearPlane,
                                              float            farPlane,
                                              float            fovVerticalRad,
                                              bool             resetAccumulation,
                                              const float*     pView,
                                              uint32_t         frameId )
{
}

void RTGL1::FrameGeneratorFSR3::CaptureHudless(
    VkCommandBuffer cmd, uint32_t frameIndex, VkImage srcImage, uint32_t width, uint32_t height )
{
}

void RTGL1::FrameGeneratorFSR3::SetDlss( std::shared_ptr< class DLSS > _dlss ) {}

void RTGL1::FrameGeneratorFSR3::ConfigureFrame( VkSwapchainKHR swapchain,
                                                uint32_t       frameIndex,
                                                uint32_t       width,
                                                uint32_t       height,
                                                uint32_t       frameId,
                                                bool           frameGenEnabled,
                                                bool           useDlssG )
{
}

VkResult RTGL1::FrameGeneratorFSR3::CreateSwapchain( const VkSwapchainCreateInfoKHR& createInfo,
                                                     VkSwapchainKHR* outSwapchain )
{
    return VK_ERROR_INITIALIZATION_FAILED;
}

void RTGL1::FrameGeneratorFSR3::DestroySwapchain( VkSwapchainKHR swapchain ) {}

VkResult RTGL1::FrameGeneratorFSR3::AcquireNextImage( VkDevice       device_,
                                                      VkSwapchainKHR swapchain,
                                                      uint64_t       timeout,
                                                      VkSemaphore    semaphore,
                                                      uint32_t*      outImageIndex )
{
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult RTGL1::FrameGeneratorFSR3::QueuePresent( VkQueue                 queue,
                                                  const VkPresentInfoKHR& presentInfo )
{
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult RTGL1::FrameGeneratorFSR3::GetSwapchainImages( VkDevice       device_,
                                                        VkSwapchainKHR swapchain,
                                                        uint32_t*      pCount,
                                                        VkImage*       pImages )
{
    return VK_ERROR_INITIALIZATION_FAILED;
}

void RTGL1::FrameGeneratorFSR3::WaitForPresents() {}

#endif
