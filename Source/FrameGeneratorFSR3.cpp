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

#include <utility>

#include <ffx_api/ffx_framegeneration.h>

#include "DLSS.h"
#include "MemoryAllocator.h"
#include "Queues.h"
#include "RenderResolutionHelper.h"
#include "RgException.h"

namespace
{
ffxReturnCode_t SafeCreateFfxContext( ffxContext* context, ffxCreateContextDescHeader* header )
{
#if defined( _MSC_VER )
    __try
    {
        return ffxCreateContext( context, header, nullptr );
    }
    __except( EXCEPTION_EXECUTE_HANDLER )
    {
        return FFX_API_RETURN_ERROR;
    }
#else
    return ffxCreateContext( context, header, nullptr );
#endif
}
}

RTGL1::FrameGeneratorFSR3::FrameGeneratorFSR3( VkDevice                           _device,
                                               VkPhysicalDevice                   _physDevice,
                                               std::shared_ptr< MemoryAllocator > _allocator,
                                               std::shared_ptr< Queues >          _queues,
                                               VkFormat                           _backBufferFormat,
                                               float                              _worldScale )
    : device( _device )
    , physDevice( _physDevice )
    , allocator( std::move( _allocator ) )
    , queues( std::move( _queues ) )
    , backBufferFormat( _backBufferFormat )
    , worldScale( _worldScale <= 0.0f ? 1.0f : _worldScale )
{
}

void RTGL1::FrameGeneratorFSR3::SetDlss( std::shared_ptr< DLSS > _dlss )
{
    dlss = std::move( _dlss );
}

RTGL1::FrameGeneratorFSR3::~FrameGeneratorFSR3()
{
    WaitForPresents();

    DestroyFgContext();

    if( swapchainContext )
    {
        ffxDestroyContext( &swapchainContext, nullptr );
        swapchainContext = nullptr;
    }

    DestroyHudlessImages();
}

bool RTGL1::FrameGeneratorFSR3::IsAvailable() const
{
    return !fgContextFailed && !swapchainContextFailed;
}

RTGL1::IFrameGeneratorSwapchain* RTGL1::FrameGeneratorFSR3::GetSwapchainHooks()
{
    return this;
}

bool RTGL1::FrameGeneratorFSR3::IsActive() const
{
    return swapchainContext != nullptr;
}

void RTGL1::FrameGeneratorFSR3::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
    if( resolutionState.upscaledWidth == 0 || resolutionState.upscaledHeight == 0 )
    {
        return;
    }

    WaitForPresents();
    vkDeviceWaitIdle( device );

    DestroyFgContext();
    CreateHudlessImages( resolutionState.upscaledWidth, resolutionState.upscaledHeight );
    CreateFgContext( resolutionState.upscaledWidth, resolutionState.upscaledHeight );
}

void RTGL1::FrameGeneratorFSR3::CreateFgContext( uint32_t displayWidth, uint32_t displayHeight )
{
    ffxCreateBackendVKDesc backendDesc = {};
    backendDesc.header.type            = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backendDesc.vkDevice               = device;
    backendDesc.vkPhysicalDevice       = physDevice;
    backendDesc.vkDeviceProcAddr       = vkGetDeviceProcAddr;

    ffxCreateContextDescFrameGenerationHudless hudlessDesc = {};
    hudlessDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudlessDesc.header.pNext              = &backendDesc.header;
    hudlessDesc.hudlessBackBufferFormat   = ffxApiGetSurfaceFormatVK( backBufferFormat );

    ffxCreateContextDescFrameGeneration createFg = {};
    createFg.header.type     = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    createFg.header.pNext    = &hudlessDesc.header;
    createFg.flags           = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    createFg.displaySize     = { displayWidth, displayHeight };
    createFg.maxRenderSize   = { displayWidth, displayHeight };
    createFg.backBufferFormat = ffxApiGetSurfaceFormatVK( backBufferFormat );

    ffxReturnCode_t ret = SafeCreateFfxContext( &fgContext, &createFg.header );
    if( ret != FFX_API_RETURN_OK )
    {
        debug::Warning( "FSR3 FG: ffxCreateContext (frame generation) failed, code {}",
                        static_cast< int >( ret ) );

        if( fgContext )
        {
            ffxDestroyContext( &fgContext, nullptr );
            fgContext = nullptr;
        }

        fgContextFailed = true;
        return;
    }

    debug::Info( "FSR3 FG: frame generation context created" );
}

void RTGL1::FrameGeneratorFSR3::DestroyFgContext()
{
    if( fgContext )
    {
        // disable generation and detach resources before destroying (per SDK docs)
        ffxConfigureDescFrameGeneration cfg = {};
        cfg.header.type            = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain              = nullptr;
        cfg.frameGenerationEnabled = false;

        ffxConfigure( &fgContext, &cfg.header );

        ffxDestroyContext( &fgContext, nullptr );
        fgContext = nullptr;
    }
}

void RTGL1::FrameGeneratorFSR3::CreateHudlessImages( uint32_t width, uint32_t height )
{
    DestroyHudlessImages();

    for( uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++ )
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
            .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_STORAGE_BIT,
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
}

void RTGL1::FrameGeneratorFSR3::DestroyHudlessImages()
{
    for( uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++ )
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

VkResult RTGL1::FrameGeneratorFSR3::CreateSwapchain( const VkSwapchainCreateInfoKHR& createInfo,
                                                     VkSwapchainKHR* outSwapchain )
{
    if( swapchainContextFailed )
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // context already exists: just recreate the swapchain through the replacement function
    if( swapchainContext != nullptr )
    {
        if( pfnCreateSwapchainFFX == nullptr )
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkSwapchainCreateInfoKHR ci = createInfo;
        ci.oldSwapchain            = VK_NULL_HANDLE;

        VkResult r = pfnCreateSwapchainFFX(
            device, &ci, nullptr, outSwapchain, static_cast< void* >( swapchainContext ) );
        swapchainHandleAlive = ( r == VK_SUCCESS );
        return r;
    }

    ffxCreateContextDescFrameGenerationSwapChainModeVK modeDesc = {};
    modeDesc.header.type          = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_MODE_VK;
    modeDesc.composeOnPresentQueue = false;

    ffxCreateContextDescFrameGenerationSwapChainVK createDesc = {};
    createDesc.header.type     = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    createDesc.header.pNext    = &modeDesc.header;
    createDesc.physicalDevice  = physDevice;
    createDesc.device          = device;
    createDesc.swapchain       = outSwapchain;
    createDesc.createInfo      = createInfo;
    createDesc.createInfo.oldSwapchain = VK_NULL_HANDLE;
    createDesc.allocator       = nullptr;
    createDesc.gameQueue       = { queues->GetGraphics(), queues->GetIndexGraphics(), nullptr };
    createDesc.asyncComputeQueue = { queues->GetCompute(), queues->GetIndexCompute(), nullptr };
    createDesc.presentQueue    = { queues->GetPresentQueue(), queues->GetIndexPresent(), nullptr };
    createDesc.imageAcquireQueue = { queues->GetImageAcquireQueue(),
                                     queues->GetIndexImageAcquire(),
                                     nullptr };

    *outSwapchain = VK_NULL_HANDLE;

    ffxReturnCode_t ret = SafeCreateFfxContext( &swapchainContext, &createDesc.header );
    if( ret != FFX_API_RETURN_OK || *outSwapchain == VK_NULL_HANDLE )
    {
        debug::Warning( "FSR3 FG: ffxCreateContext (swapchain) failed, code {}",
                        static_cast< int >( ret ) );

        if( swapchainContext )
        {
            ffxDestroyContext( &swapchainContext, nullptr );
            swapchainContext = nullptr;
        }

        swapchainContextFailed = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    ffxQueryDescSwapchainReplacementFunctionsVK replacementFunctions = {};
    replacementFunctions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;

    ffxReturnCode_t queryRet = ffxQuery( &swapchainContext, &replacementFunctions.header );

    pfnCreateSwapchainFFX  = replacementFunctions.pOutCreateSwapchainFFXAPI;
    pfnDestroySwapchainFFX = replacementFunctions.pOutDestroySwapchainFFXAPI;
    pfnGetSwapchainImages  = replacementFunctions.pOutGetSwapchainImagesKHR;
    pfnAcquireNextImage    = replacementFunctions.pOutAcquireNextImageKHR;
    pfnQueuePresent        = replacementFunctions.pOutQueuePresentKHR;

    if( queryRet != FFX_API_RETURN_OK || pfnCreateSwapchainFFX == nullptr ||
        pfnDestroySwapchainFFX == nullptr || pfnGetSwapchainImages == nullptr ||
        pfnAcquireNextImage == nullptr || pfnQueuePresent == nullptr )
    {
        debug::Warning( "FSR3 FG: failed to query swapchain replacement functions, code {}",
                        static_cast< int >( queryRet ) );

        ffxDestroyContext( &swapchainContext, nullptr );
        swapchainContext = nullptr;

        swapchainContextFailed = true;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    swapchainHandleAlive = true;
    debug::Info( "FSR3 FG: swapchain context created" );
    return VK_SUCCESS;
}

void RTGL1::FrameGeneratorFSR3::DestroySwapchain( VkSwapchainKHR swapchain )
{
    if( swapchainContext != nullptr && pfnDestroySwapchainFFX != nullptr &&
        swapchain != VK_NULL_HANDLE )
    {
        WaitForPresents();
        pfnDestroySwapchainFFX(
            device, swapchain, nullptr, static_cast< void* >( swapchainContext ) );
        swapchainHandleAlive = false;
    }
}

VkResult RTGL1::FrameGeneratorFSR3::AcquireNextImage( VkDevice       device_,
                                                      VkSwapchainKHR swapchain,
                                                      uint64_t       timeout,
                                                      VkSemaphore    semaphore,
                                                      uint32_t*      outImageIndex )
{
    return pfnAcquireNextImage( device_, swapchain, timeout, semaphore, VK_NULL_HANDLE, outImageIndex );
}

VkResult RTGL1::FrameGeneratorFSR3::QueuePresent( VkQueue                 queue,
                                                  const VkPresentInfoKHR& presentInfo )
{
    return pfnQueuePresent( queue, &presentInfo );
}

VkResult RTGL1::FrameGeneratorFSR3::GetSwapchainImages( VkDevice       device_,
                                                        VkSwapchainKHR swapchain,
                                                        uint32_t*      pCount,
                                                        VkImage*       pImages )
{
    return pfnGetSwapchainImages( device_, swapchain, pCount, pImages );
}

void RTGL1::FrameGeneratorFSR3::WaitForPresents()
{
    if( swapchainContext == nullptr || !swapchainHandleAlive )
    {
        return;
    }

    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK waitDesc = {};
    waitDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    ffxDispatch( &swapchainContext, &waitDesc.header );
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
        pendingReset.store( true );
    }

    savedCameraNear = nearPlane;
    savedCameraFar = farPlane;
    savedFovVerticalRad = fovVerticalRad;
    if( pView != nullptr )
    {
        memcpy( savedViewMatrix, pView, sizeof( savedViewMatrix ) );
    }

    using FI = FramebufferImageIndex;
    FI rs[] = { FI::FB_IMAGE_INDEX_DEPTH_NDC, FI::FB_IMAGE_INDEX_MOTION_DLSS };
    framebuffers->BarrierMultiple( cmd, frameIndex, rs, Framebuffers::BarrierType::Storage );

    auto [ depthImage, depthView, depthFormat, depthSz ] = framebuffers->GetImageHandles(
        FI::FB_IMAGE_INDEX_DEPTH_NDC, frameIndex, renderResolution.GetResolutionState() );
    auto [ motionImage, motionView, motionFormat, motionSz ] = framebuffers->GetImageHandles(
        FI::FB_IMAGE_INDEX_MOTION_DLSS, frameIndex, renderResolution.GetResolutionState() );

    savedDepthImage = depthImage;
    savedDepthView = depthView;
    savedMotionImage = motionImage;
    savedMotionView = motionView;
    savedRenderWidth = renderResolution.Width();
    savedRenderHeight = renderResolution.Height();

    if( isDlssGActive && dlss )
    {
        dlss->ValidateDlssgFeature( cmd,
                                    renderResolution.UpscaledWidth(),
                                    renderResolution.UpscaledHeight(),
                                    backBufferFormat );
        return;
    }

    if( !fgContext )
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
        (void*)depthImage,
        ffxApiGetImageResourceDescriptionVK( depthImage, depthInfo, 0 ),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
    FfxApiResource motionRes = ffxApiGetResourceVK(
        (void*)motionImage,
        ffxApiGetImageResourceDescriptionVK( motionImage, motionInfo, 0 ),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );

    const ResolutionState& rsState = renderResolution.GetResolutionState();

    ffxDispatchDescFrameGenerationPrepare prepareDesc = {};
    prepareDesc.header.type       = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepareDesc.frameID           = frameId;
    prepareDesc.flags             = 0;
    prepareDesc.commandList       = (void*)cmd;
    prepareDesc.renderSize        = { rsState.renderWidth, rsState.renderHeight };
    prepareDesc.jitterOffset      = { -jitterOffset.data[ 0 ], -jitterOffset.data[ 1 ] };
    prepareDesc.motionVectorScale = { float( rsState.renderWidth ), float( rsState.renderHeight ) };
    prepareDesc.frameTimeDelta    = float( timeDelta * 1000.0 );
    prepareDesc.cameraNear        = nearPlane;
    prepareDesc.cameraFar         = farPlane;
    prepareDesc.cameraFovAngleVertical = fovVerticalRad;
    prepareDesc.viewSpaceToMetersFactor = worldScale;
    prepareDesc.depth             = depthRes;
    prepareDesc.motionVectors     = motionRes;

    ffxDispatchDescFrameGenerationPrepareCameraInfo cameraInfo = {};
    if( pView != nullptr )
    {
        cameraInfo.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        cameraInfo.cameraRight[ 0 ]   = pView[ 0 ];
        cameraInfo.cameraRight[ 1 ]   = pView[ 4 ];
        cameraInfo.cameraRight[ 2 ]   = pView[ 8 ];
        cameraInfo.cameraUp[ 0 ]      = pView[ 1 ];
        cameraInfo.cameraUp[ 1 ]      = pView[ 5 ];
        cameraInfo.cameraUp[ 2 ]      = pView[ 9 ];
        cameraInfo.cameraForward[ 0 ] = -pView[ 2 ];
        cameraInfo.cameraForward[ 1 ] = -pView[ 6 ];
        cameraInfo.cameraForward[ 2 ] = -pView[ 10 ];

        const float tx = pView[ 12 ];
        const float ty = pView[ 13 ];
        const float tz = pView[ 14 ];
        cameraInfo.cameraPosition[ 0 ] = -( pView[ 0 ] * tx + pView[ 1 ] * ty + pView[ 2 ] * tz );
        cameraInfo.cameraPosition[ 1 ] = -( pView[ 4 ] * tx + pView[ 5 ] * ty + pView[ 6 ] * tz );
        cameraInfo.cameraPosition[ 2 ] = -( pView[ 8 ] * tx + pView[ 9 ] * ty + pView[ 10 ] * tz );

        prepareDesc.header.pNext = &cameraInfo.header;
    }

    ffxReturnCode_t ret = ffxDispatch( &fgContext, &prepareDesc.header );
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
    if( hudlessImages[ frameIndex ] == VK_NULL_HANDLE )
    {
        return;
    }

    VkImageMemoryBarrier2 barriers[ 2 ] = {};

    barriers[ 0 ] = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image         = hudlessImages[ frameIndex ],
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
        .srcOffsets     = { { 0, 0, 0 }, { (int32_t)width, (int32_t)height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets     = { { 0, 0, 0 }, { (int32_t)width, (int32_t)height, 1 } },
    };
    vkCmdBlitImage( cmd,
                    srcImage,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    hudlessImages[ frameIndex ],
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
    if( isDlssGActive != newDlssG || ( frameGenEnabled && !prevFrameGenEnabled ) )
    {
        pendingReset.store( true );
    }
    prevFrameGenEnabled = frameGenEnabled;
    isDlssGActive       = newDlssG;
    currentWidth      = width;
    currentHeight     = height;
    currentFrameIndex = frameIndex;

    if( !fgContext || !swapchainContext )
    {
        return;
    }

    FfxApiResource hudlessRes = {};
    if( frameGenEnabled && hudlessImages[ frameIndex ] != VK_NULL_HANDLE )
    {
        VkImageCreateInfo hudlessInfo = {
            .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType   = VK_IMAGE_TYPE_2D,
            .format      = backBufferFormat,
            .extent      = { width, height, 1 },
            .mipLevels   = 1,
            .arrayLayers = 1,
            .samples     = VK_SAMPLE_COUNT_1_BIT,
            .tiling      = VK_IMAGE_TILING_OPTIMAL,
            .usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT,
        };
        hudlessRes = ffxApiGetResourceVK(
            (void*)hudlessImages[ frameIndex ],
            ffxApiGetImageResourceDescriptionVK( hudlessImages[ frameIndex ], hudlessInfo, 0 ),
            FFX_API_RESOURCE_STATE_COMPUTE_READ );
    }

    ffxConfigureDescFrameGeneration configDesc  = {};
    configDesc.header.type         = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configDesc.swapChain           = (void*)swapchain;
    configDesc.presentCallback     = nullptr;
    configDesc.frameGenerationCallback =
        []( ffxDispatchDescFrameGeneration* params, void* pUserCtx ) -> ffxReturnCode_t {
        auto* self = static_cast< FrameGeneratorFSR3* >( pUserCtx );

        if( self->pendingReset.exchange( false ) )
        {
            params->reset = true;
        }

        if( self->isDlssGActive && self->dlss )
        {
            VkCommandBuffer cb = static_cast< VkCommandBuffer >( params->commandList );
            VkImage outInterpolatedImage = static_cast< VkImage >( params->outputs[ 0 ].resource );
            VkImage realFrameImage = static_cast< VkImage >( params->presentColor.resource );

            VkImage hudlessImg = self->hudlessImages[ self->currentFrameIndex ];
            VkImageView hudlessVw = self->hudlessViews[ self->currentFrameIndex ];

            bool ok = self->dlss->EvaluateDlssg(
                cb,
                realFrameImage,
                outInterpolatedImage,
                self->savedDepthImage,
                self->savedDepthView,
                self->savedMotionImage,
                self->savedMotionView,
                hudlessImg,
                hudlessVw,
                self->currentWidth,
                self->currentHeight,
                self->savedRenderWidth,
                self->savedRenderHeight,
                self->backBufferFormat,
                self->savedViewMatrix,
                self->savedCameraNear,
                self->savedCameraFar,
                self->savedFovVerticalRad,
                params->reset
            );

            return ok ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR;
        }

        return ffxDispatch( &self->fgContext, &params->header );
    };
    configDesc.frameGenerationCallbackUserContext = this;
    configDesc.frameGenerationEnabled             = frameGenEnabled;
    configDesc.allowAsyncWorkloads                = true;
    configDesc.HUDLessColor                       = hudlessRes;
    configDesc.flags                              = 0;
    configDesc.onlyPresentGenerated               = false;
    configDesc.generationRect                     = { 0, 0, (int32_t)width, (int32_t)height };
    configDesc.frameID                            = frameId;

    ffxReturnCode_t ret = ffxConfigure( &fgContext, &configDesc.header );
    if( ret != FFX_API_RETURN_OK )
    {
        debug::Warning( "FSR3 FG: ffxConfigure failed, code {}", static_cast< int >( ret ) );
    }
}

#else

RTGL1::FrameGeneratorFSR3::FrameGeneratorFSR3( VkDevice                           _device,
                                               VkPhysicalDevice                   _physDevice,
                                               std::shared_ptr< MemoryAllocator > _allocator,
                                               std::shared_ptr< Queues >          _queues,
                                               VkFormat                           _backBufferFormat,
                                               float                              _worldScale )
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
