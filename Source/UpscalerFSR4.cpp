#include "UpscalerFSR4.h"

#if defined( RG_USE_AMD_FSR3 ) && defined( _WIN32 )

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>

#include "FfxLoaderVK.h"
#include "RenderResolutionHelper.h"

namespace
{
constexpr RTGL1::FramebufferImageIndex OUTPUT_IMAGE_INDEX = RTGL1::FB_IMAGE_INDEX_UPSCALED_PONG;

void FfxMessageCallback( uint32_t type, const wchar_t* message )
{
    if( !message )
    {
        return;
    }

    int len = WideCharToMultiByte( CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr );
    std::string utf8;
    if( len > 1 )
    {
        utf8.resize( static_cast< size_t >( len - 1 ) );
        WideCharToMultiByte( CP_UTF8, 0, message, -1, utf8.data(), len, nullptr, nullptr );
    }

    if( type == FFX_API_MESSAGE_TYPE_ERROR )
    {
        RTGL1::debug::Error( "FSR4: {}", utf8 );
    }
    else
    {
        RTGL1::debug::Warning( "FSR4: {}", utf8 );
    }
}

FfxApiResource ToFfxResource( RTGL1::FramebufferImageIndex  fbImage,
                              uint32_t                      frameIndex,
                              const RTGL1::Framebuffers&    framebuffers,
                              const RTGL1::ResolutionState& resolutionState,
                              uint32_t                      state )
{
    auto [ image, view, format, sz ] =
        framebuffers.GetImageHandles( fbImage, frameIndex, resolutionState );

    if( image == VK_NULL_HANDLE )
    {
        return FfxApiResource{};
    }

    VkImageCreateInfo createInfo = {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = format,
        .extent      = { sz.width, sz.height, 1 },
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .tiling      = VK_IMAGE_TILING_OPTIMAL,
        .usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
    };

    FfxApiResourceDescription resDesc = ffxApiGetImageResourceDescriptionVK( image, createInfo, 0 );
    return ffxApiGetResourceVK( static_cast< void* >( image ), resDesc, state );
}

template< size_t N >
void InsertBarriers( VkCommandBuffer      cmd,
                     uint32_t             frameIndex,
                     RTGL1::Framebuffers& framebuffers,
                     const RTGL1::FramebufferImageIndex ( &inputsAndOutput )[ N ],
                     bool                 isBackwards )
{
    assert( std::find( std::begin( inputsAndOutput ),
                       std::end( inputsAndOutput ),
                       OUTPUT_IMAGE_INDEX ) != std::end( inputsAndOutput ) );

    VkImageMemoryBarrier2 barriers[ N ];

    for( size_t i = 0; i < N; i++ )
    {
        barriers[ i ] = {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = inputsAndOutput[ i ] == OUTPUT_IMAGE_INDEX
                                 ? VK_ACCESS_2_SHADER_WRITE_BIT
                                 : VK_ACCESS_2_SHADER_READ_BIT,
            .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout     = inputsAndOutput[ i ] == OUTPUT_IMAGE_INDEX
                                 ? VK_IMAGE_LAYOUT_GENERAL
                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = framebuffers.GetImage( inputsAndOutput[ i ], frameIndex ),
            .subresourceRange    = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };

        if( isBackwards )
        {
            auto& b = barriers[ i ];
            std::swap( b.srcStageMask, b.dstStageMask );
            std::swap( b.srcAccessMask, b.dstAccessMask );
            std::swap( b.oldLayout, b.newLayout );
        }
    }

    VkDependencyInfoKHR dependencyInfo = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
        .imageMemoryBarrierCount = uint32_t( std::size( barriers ) ),
        .pImageMemoryBarriers    = barriers,
    };

    RTGL1::svkCmdPipelineBarrier2KHR( cmd, &dependencyInfo );
}
}

struct RTGL1::UpscalerFSR4::FfxState
{
    HMODULE      module{ nullptr };
    ffxFunctions funcs{};
    ffxContext   context{ nullptr };
};

RTGL1::UpscalerFSR4::UpscalerFSR4( VkDevice _device, VkPhysicalDevice _physDevice )
    : device( _device )
    , physDevice( _physDevice )
    , ffxState( std::make_unique< FfxState >() )
{
    if( !LoadFfxApiFunctions( L"amd_fidelityfx_upscaler_vk.dll", ffxState->module, ffxState->funcs ) )
    {
        debug::Warning( "FSR4: failed to load amd_fidelityfx_loader_vk.dll / amd_fidelityfx_upscaler_vk.dll, disabling FSR upscaling" );
        return;
    }

    VkDeviceContext vkDevCtx = {
        .vkDevice         = device,
        .vkPhysicalDevice = physDevice,
        .vkDeviceProcAddr = vkGetDeviceProcAddr,
    };

    uint64_t                 versionCount = 0;
    ffxQueryDescGetVersions  versionQuery = {};
    versionQuery.header.type              = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versionQuery.createDescType           = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versionQuery.device                   = &vkDevCtx;
    versionQuery.outputCount              = &versionCount;

    ffxReturnCode_t ret = ffxState->funcs.Query( nullptr, &versionQuery.header );
    if( ret != FFX_API_RETURN_OK || versionCount == 0 )
    {
        debug::Warning( "FSR4: no upscaler providers available (code {}, count {})",
                        static_cast< int >( ret ),
                        versionCount );
        return;
    }

    std::vector< uint64_t >    versionIds( versionCount );
    std::vector< const char* > versionNames( versionCount );
    versionQuery.versionIds   = versionIds.data();
    versionQuery.versionNames = versionNames.data();

    ret = ffxState->funcs.Query( nullptr, &versionQuery.header );
    if( ret == FFX_API_RETURN_OK )
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
        debug::Info( "FSR4: available upscaler providers: {}", list );
    }

    loaderAvailable = true;
}

RTGL1::UpscalerFSR4::~UpscalerFSR4()
{
    if( ffxState )
    {
        if( ffxState->context )
        {
            vkDeviceWaitIdle( device );
            DestroyContext();
        }
        if( ffxState->module )
        {
            FreeLibrary( ffxState->module );
            ffxState->module = nullptr;
        }
    }
}

void RTGL1::UpscalerFSR4::DestroyContext()
{
    if( isContextCreated && ffxState && ffxState->context && ffxState->funcs.DestroyContext )
    {
        ffxState->funcs.DestroyContext( &ffxState->context, nullptr );
        ffxState->context = nullptr;
        isContextCreated  = false;
    }
}

void RTGL1::UpscalerFSR4::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
    if( resolutionState.upscaledWidth == 0 || resolutionState.upscaledHeight == 0 )
    {
        if( isContextCreated )
        {
            vkDeviceWaitIdle( device );
            DestroyContext();
        }
        return;
    }

    if( !loaderAvailable )
    {
        return;
    }

    vkDeviceWaitIdle( device );
    DestroyContext();

    ffxCreateBackendVKDesc backendDesc = {};
    backendDesc.header.type            = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backendDesc.deviceContext          = {
        .vkDevice         = device,
        .vkPhysicalDevice = physDevice,
        .vkDeviceProcAddr = vkGetDeviceProcAddr,
    };

    ffxCreateContextDescUpscaleVersion versionDesc = {};
    versionDesc.header.type                        = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    versionDesc.header.pNext                       = &backendDesc.header;
    versionDesc.version                            = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale createDesc = {};
    createDesc.header.type                 = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    createDesc.header.pNext                = &versionDesc.header;
    createDesc.flags                       = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
#ifndef NDEBUG
    createDesc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
#endif
    createDesc.maxRenderSize  = { resolutionState.upscaledWidth, resolutionState.upscaledHeight };
    createDesc.maxUpscaleSize = { resolutionState.upscaledWidth, resolutionState.upscaledHeight };
    createDesc.fpMessage      = FfxMessageCallback;

    ffxReturnCode_t ret =
        ffxState->funcs.CreateContext( &ffxState->context, &createDesc.header, nullptr );
    if( ret != FFX_API_RETURN_OK || !ffxState->context )
    {
        debug::Warning( "FSR4: ffxCreateContext failed, code {}", static_cast< int >( ret ) );
        if( ffxState->context )
        {
            ffxState->funcs.DestroyContext( &ffxState->context, nullptr );
            ffxState->context = nullptr;
        }
        contextFailed = true;
        return;
    }

    isContextCreated = true;
    contextFailed    = false;

    ffxQueryGetProviderVersion providerVer = {};
    providerVer.header.type                = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if( ffxState->funcs.Query( &ffxState->context, &providerVer.header ) == FFX_API_RETURN_OK )
    {
        debug::Info( "FSR4: active upscaler provider '{}' (0x{:016X})",
                     providerVer.versionName ? providerVer.versionName : "unknown",
                     providerVer.versionId );
    }
}

RTGL1::FramebufferImageIndex RTGL1::UpscalerFSR4::Apply(
    VkCommandBuffer                        cmd,
    uint32_t                               frameIndex,
    const std::shared_ptr< Framebuffers >& framebuffers,
    const RenderResolutionHelper&          renderResolution,
    RgFloat2D                              jitterOffset,
    double                                 timeDelta,
    float                                  nearPlane,
    float                                  farPlane,
    float                                  fovVerticalRad,
    bool                                   resetAccumulation )
{
    assert( nearPlane > 0.0f && nearPlane < farPlane );

    if( !loaderAvailable || contextFailed )
    {
        return FramebufferImageIndex::FB_IMAGE_INDEX_FINAL;
    }

    if( !isContextCreated )
    {
        OnFramebuffersSizeChange( renderResolution.GetResolutionState() );
    }

    if( !isContextCreated || contextFailed )
    {
        return FramebufferImageIndex::FB_IMAGE_INDEX_FINAL;
    }

    using FI = FramebufferImageIndex;

    FI rs[] = {
        FI::FB_IMAGE_INDEX_FINAL,
        FI::FB_IMAGE_INDEX_DEPTH_NDC,
        FI::FB_IMAGE_INDEX_MOTION_DLSS,
        FI::FB_IMAGE_INDEX_REACTIVITY,
        OUTPUT_IMAGE_INDEX,
    };
    InsertBarriers( cmd, frameIndex, *framebuffers, rs, false );

    const ResolutionState& rsState = renderResolution.GetResolutionState();

    ffxDispatchDescUpscale info   = {};
    info.header.type              = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    info.commandList              = static_cast< void* >( cmd );
    info.color                    = ToFfxResource( FI::FB_IMAGE_INDEX_FINAL, frameIndex, *framebuffers, rsState, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
    info.depth                    = ToFfxResource( FI::FB_IMAGE_INDEX_DEPTH_NDC, frameIndex, *framebuffers, rsState, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
    info.motionVectors            = ToFfxResource( FI::FB_IMAGE_INDEX_MOTION_DLSS, frameIndex, *framebuffers, rsState, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
    info.exposure                 = {};
    info.reactive                 = ToFfxResource( FI::FB_IMAGE_INDEX_REACTIVITY, frameIndex, *framebuffers, rsState, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
    info.transparencyAndComposition = {};
    info.output                   = ToFfxResource( OUTPUT_IMAGE_INDEX, frameIndex, *framebuffers, rsState, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS );
    info.jitterOffset             = { -jitterOffset.data[ 0 ], -jitterOffset.data[ 1 ] };
    info.motionVectorScale        = { float( rsState.renderWidth ), float( rsState.renderHeight ) };
    info.renderSize               = { rsState.renderWidth, rsState.renderHeight };
    info.upscaleSize              = { rsState.upscaledWidth, rsState.upscaledHeight };
    info.enableSharpening         = renderResolution.IsCASInsideFSR();
    info.sharpness                = renderResolution.GetSharpeningIntensity();
    info.frameTimeDelta           = float( timeDelta * 1000.0 );
    info.preExposure              = 1.0f;
    info.reset                    = resetAccumulation;
    info.cameraNear               = nearPlane;
    info.cameraFar                = farPlane;
    info.cameraFovAngleVertical   = fovVerticalRad;
    info.viewSpaceToMetersFactor  = 1.0f;
    info.flags                    = 0;

    ffxReturnCode_t ret = ffxState->funcs.Dispatch( &ffxState->context, &info.header );

    InsertBarriers( cmd, frameIndex, *framebuffers, rs, true );

    if( ret != FFX_API_RETURN_OK )
    {
        debug::Warning( "FSR4: dispatch failed (code {}), disabling FSR upscaling",
                        static_cast< int >( ret ) );
        contextFailed = true;
        return FramebufferImageIndex::FB_IMAGE_INDEX_FINAL;
    }

    return OUTPUT_IMAGE_INDEX;
}

RgFloat2D RTGL1::UpscalerFSR4::GetJitter( const ResolutionState& resolutionState,
                                          uint32_t               frameId ) const
{
    if( !IsAvailable() || resolutionState.renderWidth == 0 || resolutionState.upscaledWidth == 0 )
    {
        return RgFloat2D{ 0.0f, 0.0f };
    }

    ffxCreateBackendVKDesc backendDesc = {};
    backendDesc.header.type            = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backendDesc.deviceContext          = {
        .vkDevice         = device,
        .vkPhysicalDevice = physDevice,
        .vkDeviceProcAddr = vkGetDeviceProcAddr,
    };

    ffxContext  ctxCopy = ffxState->context;
    ffxContext* pCtx    = ctxCopy ? &ctxCopy : nullptr;

    int32_t                                  phaseCount = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount   phaseDesc  = {};
    phaseDesc.header.type    = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    phaseDesc.header.pNext   = pCtx ? nullptr : &backendDesc.header;
    phaseDesc.renderWidth    = resolutionState.renderWidth;
    phaseDesc.displayWidth   = resolutionState.upscaledWidth;
    phaseDesc.pOutPhaseCount = &phaseCount;

    ffxReturnCode_t ret = ffxState->funcs.Query( pCtx, &phaseDesc.header );
    assert( ret == FFX_API_RETURN_OK );
    phaseCount = std::max( 1, phaseCount );

    RgFloat2D                          jitter     = {};
    ffxQueryDescUpscaleGetJitterOffset offsetDesc = {};
    offsetDesc.header.type                        = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    offsetDesc.header.pNext                       = pCtx ? nullptr : &backendDesc.header;
    offsetDesc.index                              = int32_t( frameId );
    offsetDesc.phaseCount                         = phaseCount;
    offsetDesc.pOutX                              = &jitter.data[ 0 ];
    offsetDesc.pOutY                              = &jitter.data[ 1 ];

    ret = ffxState->funcs.Query( pCtx, &offsetDesc.header );
    assert( ret == FFX_API_RETURN_OK );

    return jitter;
}

bool RTGL1::UpscalerFSR4::IsAvailable() const
{
    return loaderAvailable && !contextFailed;
}

#else

struct RTGL1::UpscalerFSR4::FfxState
{
};

RTGL1::UpscalerFSR4::UpscalerFSR4( VkDevice _device, VkPhysicalDevice _physDevice )
    : device( _device )
    , physDevice( _physDevice )
{
}

RTGL1::UpscalerFSR4::~UpscalerFSR4() {}

void RTGL1::UpscalerFSR4::DestroyContext() {}

void RTGL1::UpscalerFSR4::OnFramebuffersSizeChange( const ResolutionState& resolutionState ) {}

RTGL1::FramebufferImageIndex RTGL1::UpscalerFSR4::Apply(
    VkCommandBuffer                        cmd,
    uint32_t                               frameIndex,
    const std::shared_ptr< Framebuffers >& framebuffers,
    const RenderResolutionHelper&          renderResolution,
    RgFloat2D                              jitterOffset,
    double                                 timeDelta,
    float                                  nearPlane,
    float                                  farPlane,
    float                                  fovVerticalRad,
    bool                                   resetAccumulation )
{
    return FramebufferImageIndex::FB_IMAGE_INDEX_FINAL;
}

RgFloat2D RTGL1::UpscalerFSR4::GetJitter( const ResolutionState& resolutionState,
                                          uint32_t               frameId ) const
{
    return RgFloat2D{ 0.0f, 0.0f };
}

bool RTGL1::UpscalerFSR4::IsAvailable() const
{
    return false;
}

#endif
