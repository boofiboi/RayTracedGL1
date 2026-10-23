#include "FrameGeneratorDLSS.h"

#ifdef RG_USE_NVIDIA_DLSS

#include <cmath>
#include <utility>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params_dlssg.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers.h>

#include "MemoryAllocator.h"
#include "Queues.h"
#include "RenderResolutionHelper.h"
#include "Framebuffers.h"
#include "CmdLabel.h"
#include "DebugPrint.h"
#include "RgException.h"

static NVSDK_NGX_Resource_VK ToNGXResource(
    const std::shared_ptr< RTGL1::Framebuffers >& framebuffers,
    uint32_t                                      frameIndex,
    RTGL1::FramebufferImageIndex                  imageIndex,
    NVSDK_NGX_Dimensions                          size,
    bool                                          withWriteAccess = false )
{
    auto [ image, view, format ] = framebuffers->GetImageHandles( imageIndex, frameIndex );

    VkImageSubresourceRange subresourceRange = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    return NVSDK_NGX_Create_ImageView_Resource_VK(
        view, image, subresourceRange, format, size.Width, size.Height, withWriteAccess );
}

static NVSDK_NGX_Resource_VK ToNGXResourceFromView(
    VkImage               image,
    VkImageView           view,
    VkFormat              format,
    NVSDK_NGX_Dimensions  size,
    bool                  withWriteAccess = false )
{
    VkImageSubresourceRange subresourceRange = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    return NVSDK_NGX_Create_ImageView_Resource_VK(
        view, image, subresourceRange, format, size.Width, size.Height, withWriteAccess );
}

RTGL1::FrameGeneratorDLSS::FrameGeneratorDLSS(
    VkDevice                           _device,
    VkPhysicalDevice                   _physDevice,
    std::shared_ptr< MemoryAllocator > _allocator,
    std::shared_ptr< Queues >          _queues,
    std::shared_ptr< NgxManager >      _ngxManager,
    VkFormat                           _backBufferFormat )
    : device( _device )
    , physDevice( _physDevice )
    , allocator( std::move( _allocator ) )
    , queues( std::move( _queues ) )
    , ngxManager( std::move( _ngxManager ) )
    , backBufferFormat( _backBufferFormat )
    , pDlssgFeature( nullptr )
    , featureFailed( false )
    , hudlessImages{}
    , hudlessViews{}
    , hudlessMemories{}
    , interpImages{}
    , interpViews{}
    , interpMemories{}
    , featureWidth( 0 )
    , featureHeight( 0 )
    , displayWidth( 0 )
    , displayHeight( 0 )
    , isFrameGenEnabled( false )
    , pendingReset( true )
{
}

RTGL1::FrameGeneratorDLSS::~FrameGeneratorDLSS()
{
    WaitForPresents();
    DestroyDlssgFeature();
    DestroyHudlessImages();
    DestroyInterpImages();
}

bool RTGL1::FrameGeneratorDLSS::IsAvailable() const
{
    return ngxManager && ngxManager->IsInitialized() && ngxManager->IsFrameGenerationAvailable() && !featureFailed;
}

RTGL1::IFrameGeneratorSwapchain* RTGL1::FrameGeneratorDLSS::GetSwapchainHooks()
{
    return this;
}

bool RTGL1::FrameGeneratorDLSS::IsActive() const
{
    return IsAvailable() && isFrameGenEnabled;
}

void RTGL1::FrameGeneratorDLSS::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
    if( resolutionState.upscaledWidth == 0 || resolutionState.upscaledHeight == 0 )
    {
        return;
    }

    WaitForPresents();
    vkDeviceWaitIdle( device );

    DestroyDlssgFeature();
    CreateHudlessImages( resolutionState.upscaledWidth, resolutionState.upscaledHeight );
    CreateInterpImages( resolutionState.upscaledWidth, resolutionState.upscaledHeight );

    displayWidth  = resolutionState.upscaledWidth;
    displayHeight = resolutionState.upscaledHeight;
}

bool RTGL1::FrameGeneratorDLSS::ValidateDlssgFeature( VkCommandBuffer cmd, uint32_t width, uint32_t height )
{
    if( !ngxManager || !ngxManager->IsInitialized() || !ngxManager->IsFrameGenerationAvailable() )
    {
        return false;
    }

    if( pDlssgFeature != nullptr && featureWidth == width && featureHeight == height )
    {
        return true;
    }

    DestroyDlssgFeature();

    featureWidth  = width;
    featureHeight = height;

    NVSDK_NGX_DLSSG_Create_Params createParams = {
        .Width                    = width,
        .Height                   = height,
        .NativeBackbufferFormat   = static_cast< unsigned int >( backBufferFormat ),
        .RenderWidth              = width,
        .RenderHeight             = height,
        .DynamicResolutionScaling = false,
    };

    uint32_t creationNodeMask   = 1;
    uint32_t visibilityNodeMask = 1;

    NVSDK_NGX_Result r = NGX_VK_CREATE_DLSSG(
        cmd, creationNodeMask, visibilityNodeMask, &pDlssgFeature, ngxManager->GetParameters(), &createParams );

    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS FG: NGX_VK_CREATE_DLSSG failed: 0x{:08X}", static_cast< uint32_t >( r ) );
        pDlssgFeature = nullptr;
        featureFailed = true;
        return false;
    }

    return true;
}

void RTGL1::FrameGeneratorDLSS::DestroyDlssgFeature()
{
    if( pDlssgFeature != nullptr )
    {
        vkDeviceWaitIdle( device );
        NVSDK_NGX_VULKAN_ReleaseFeature( pDlssgFeature );
        pDlssgFeature = nullptr;
    }

    featureWidth  = 0;
    featureHeight = 0;
}

void RTGL1::FrameGeneratorDLSS::CreateHudlessImages( uint32_t width, uint32_t height )
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
            .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VkResult r = vkCreateImage( device, &imageInfo, nullptr, &hudlessImages[ i ] );
        VK_CHECKERROR( r );

        VkMemoryRequirements memReqs = {};
        vkGetImageMemoryRequirements( device, hudlessImages[ i ], &memReqs );

        hudlessMemories[ i ] = allocator->AllocDedicated( memReqs,
                                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                          MemoryAllocator::AllocType::DEFAULT,
                                                          "DLSS FG HUD-less Image" );

        r = vkBindImageMemory( device, hudlessImages[ i ], hudlessMemories[ i ], 0 );
        VK_CHECKERROR( r );

        VkImageViewCreateInfo viewInfo = {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = hudlessImages[ i ],
            .viewType         = VK_IMAGE_VIEW_TYPE_2D,
            .format           = backBufferFormat,
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

void RTGL1::FrameGeneratorDLSS::DestroyHudlessImages()
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

void RTGL1::FrameGeneratorDLSS::CreateInterpImages( uint32_t width, uint32_t height )
{
    DestroyInterpImages();

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
            .usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VkResult r = vkCreateImage( device, &imageInfo, nullptr, &interpImages[ i ] );
        VK_CHECKERROR( r );

        VkMemoryRequirements memReqs = {};
        vkGetImageMemoryRequirements( device, interpImages[ i ], &memReqs );

        interpMemories[ i ] = allocator->AllocDedicated( memReqs,
                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                         MemoryAllocator::AllocType::DEFAULT,
                                                         "DLSS FG Interp Image" );

        r = vkBindImageMemory( device, interpImages[ i ], interpMemories[ i ], 0 );
        VK_CHECKERROR( r );

        VkImageViewCreateInfo viewInfo = {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = interpImages[ i ],
            .viewType         = VK_IMAGE_VIEW_TYPE_2D,
            .format           = backBufferFormat,
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };

        r = vkCreateImageView( device, &viewInfo, nullptr, &interpViews[ i ] );
        VK_CHECKERROR( r );
    }
}

void RTGL1::FrameGeneratorDLSS::DestroyInterpImages()
{
    for( uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++ )
    {
        if( interpViews[ i ] != VK_NULL_HANDLE )
        {
            vkDestroyImageView( device, interpViews[ i ], nullptr );
            interpViews[ i ] = VK_NULL_HANDLE;
        }
        if( interpImages[ i ] != VK_NULL_HANDLE )
        {
            vkDestroyImage( device, interpImages[ i ], nullptr );
            interpImages[ i ] = VK_NULL_HANDLE;
        }
        if( interpMemories[ i ] != VK_NULL_HANDLE )
        {
            MemoryAllocator::FreeDedicated( device, interpMemories[ i ] );
            interpMemories[ i ] = VK_NULL_HANDLE;
        }
    }
}

void RTGL1::FrameGeneratorDLSS::CaptureHudless(
    VkCommandBuffer cmd,
    uint32_t        frameIndex,
    VkImage         srcImage,
    uint32_t        width,
    uint32_t        height )
{
    if( !IsAvailable() || hudlessImages[ frameIndex ] == VK_NULL_HANDLE )
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
        .srcOffsets     = { { 0, 0, 0 }, { static_cast< int32_t >( width ), static_cast< int32_t >( height ), 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets     = { { 0, 0, 0 }, { static_cast< int32_t >( width ), static_cast< int32_t >( height ), 1 } },
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

void RTGL1::FrameGeneratorDLSS::ConfigureFrame(
    VkSwapchainKHR swapchain,
    uint32_t       frameIndex,
    uint32_t       width,
    uint32_t       height,
    uint32_t       frameId,
    bool           frameGenEnabled )
{
    displayWidth      = width;
    displayHeight     = height;
    isFrameGenEnabled = frameGenEnabled;
}

void RTGL1::FrameGeneratorDLSS::PrepareFrame(
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
    if( !IsAvailable() )
    {
        return;
    }

    if( resetAccumulation )
    {
        pendingReset = true;
    }

    if( !ValidateDlssgFeature( cmd, renderResolution.UpscaledWidth(), renderResolution.UpscaledHeight() ) )
    {
        return;
    }

    if( hudlessViews[ frameIndex ] == VK_NULL_HANDLE || interpViews[ frameIndex ] == VK_NULL_HANDLE )
    {
        return;
    }

    using FI = FramebufferImageIndex;
    FI fs[]  = { FI::FB_IMAGE_INDEX_DEPTH_NDC, FI::FB_IMAGE_INDEX_MOTION_DLSS };
    framebuffers->BarrierMultiple( cmd, frameIndex, fs, Framebuffers::BarrierType::Storage );

    VkImageMemoryBarrier2 interpBarrier = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .image         = interpImages[ frameIndex ],
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };

    VkDependencyInfo interpDep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &interpBarrier,
    };
    svkCmdPipelineBarrier2KHR( cmd, &interpDep );

    NVSDK_NGX_Dimensions sourceSize = {
        renderResolution.Width(),
        renderResolution.Height(),
    };
    NVSDK_NGX_Dimensions targetSize = {
        renderResolution.UpscaledWidth(),
        renderResolution.UpscaledHeight(),
    };

    NVSDK_NGX_Resource_VK backbufferResource =
        ToNGXResource( framebuffers, frameIndex, FI::FB_IMAGE_INDEX_FINAL, targetSize );
    NVSDK_NGX_Resource_VK depthResource =
        ToNGXResource( framebuffers, frameIndex, FI::FB_IMAGE_INDEX_DEPTH_NDC, sourceSize );
    NVSDK_NGX_Resource_VK motionResource =
        ToNGXResource( framebuffers, frameIndex, FI::FB_IMAGE_INDEX_MOTION_DLSS, sourceSize );
    NVSDK_NGX_Resource_VK hudlessResource =
        ToNGXResourceFromView( hudlessImages[ frameIndex ], hudlessViews[ frameIndex ], backBufferFormat, targetSize );
    NVSDK_NGX_Resource_VK interpResource =
        ToNGXResourceFromView( interpImages[ frameIndex ], interpViews[ frameIndex ], backBufferFormat, targetSize, true );

    NVSDK_NGX_VK_DLSSG_Eval_Params evalParams = {
        .pBackbuffer               = &backbufferResource,
        .pDepth                    = &depthResource,
        .pMVecs                    = &motionResource,
        .pHudless                  = &hudlessResource,
        .pUI                       = nullptr,
        .pUIAlpha                  = nullptr,
        .pBidirectionalDistortionField = nullptr,
        .pOutputInterpFrame        = &interpResource,
        .pOutputRealFrame          = nullptr,
        .pOutputDisableInterpolation = nullptr,
    };

    NVSDK_NGX_DLSSG_Opt_Eval_Params optParams = {};
    optParams.multiFrameCount = 1;
    optParams.multiFrameIndex = 1;

    for( int r = 0; r < 4; r++ )
    {
        for( int c = 0; c < 4; c++ )
        {
            optParams.cameraViewToClip[ r ][ c ] = ( r == c ? 1.0f : 0.0f );
            optParams.clipToCameraView[ r ][ c ] = ( r == c ? 1.0f : 0.0f );
            optParams.clipToLensClip[ r ][ c ]   = ( r == c ? 1.0f : 0.0f );
            optParams.clipToPrevClip[ r ][ c ]   = ( r == c ? 1.0f : 0.0f );
            optParams.prevClipToClip[ r ][ c ]   = ( r == c ? 1.0f : 0.0f );
        }
    }

    const float aspectRatio = targetSize.Height > 0 ? static_cast< float >( targetSize.Width ) / static_cast< float >( targetSize.Height ) : 1.0f;
    const float f = 1.0f / std::tan( fovVerticalRad * 0.5f );

    optParams.cameraViewToClip[ 0 ][ 0 ] = f / aspectRatio;
    optParams.cameraViewToClip[ 1 ][ 1 ] = f;
    optParams.cameraViewToClip[ 2 ][ 2 ] = farPlane / ( nearPlane - farPlane );
    optParams.cameraViewToClip[ 2 ][ 3 ] = -1.0f;
    optParams.cameraViewToClip[ 3 ][ 2 ] = ( nearPlane * farPlane ) / ( nearPlane - farPlane );
    optParams.cameraViewToClip[ 3 ][ 3 ] = 0.0f;

    optParams.clipToCameraView[ 0 ][ 0 ] = aspectRatio / f;
    optParams.clipToCameraView[ 1 ][ 1 ] = 1.0f / f;
    optParams.clipToCameraView[ 2 ][ 2 ] = 0.0f;
    optParams.clipToCameraView[ 2 ][ 3 ] = ( nearPlane - farPlane ) / ( nearPlane * farPlane );
    optParams.clipToCameraView[ 3 ][ 2 ] = -1.0f;
    optParams.clipToCameraView[ 3 ][ 3 ] = 1.0f / nearPlane;

    if( pView != nullptr )
    {
        optParams.cameraRight[ 0 ]   = pView[ 0 ];
        optParams.cameraRight[ 1 ]   = pView[ 4 ];
        optParams.cameraRight[ 2 ]   = pView[ 8 ];
        optParams.cameraUp[ 0 ]      = pView[ 1 ];
        optParams.cameraUp[ 1 ]      = pView[ 5 ];
        optParams.cameraUp[ 2 ]      = pView[ 9 ];
        optParams.cameraFwd[ 0 ]     = -pView[ 2 ];
        optParams.cameraFwd[ 1 ]     = -pView[ 6 ];
        optParams.cameraFwd[ 2 ]     = -pView[ 10 ];

        const float tx = pView[ 12 ];
        const float ty = pView[ 13 ];
        const float tz = pView[ 14 ];
        optParams.cameraPos[ 0 ] = -( pView[ 0 ] * tx + pView[ 1 ] * ty + pView[ 2 ] * tz );
        optParams.cameraPos[ 1 ] = -( pView[ 4 ] * tx + pView[ 5 ] * ty + pView[ 6 ] * tz );
        optParams.cameraPos[ 2 ] = -( pView[ 8 ] * tx + pView[ 9 ] * ty + pView[ 10 ] * tz );
    }

    optParams.cameraNear        = nearPlane;
    optParams.cameraFar         = farPlane;
    optParams.cameraFOV         = fovVerticalRad;
    optParams.cameraAspectRatio = aspectRatio;

    optParams.jitterOffset[ 0 ] = -jitterOffset.data[ 0 ];
    optParams.jitterOffset[ 1 ] = -jitterOffset.data[ 1 ];

    optParams.mvecScale[ 0 ] = static_cast< float >( sourceSize.Width );
    optParams.mvecScale[ 1 ] = static_cast< float >( sourceSize.Height );

    optParams.reset        = resetAccumulation || pendingReset;
    pendingReset           = false;
    optParams.cameraMotionIncluded = true;
    optParams.colorBuffersHDR      = false;
    optParams.depthInverted        = false;

    optParams.mvecsSubrectSize        = sourceSize;
    optParams.depthSubrectSize        = sourceSize;
    optParams.hudLessSubrectSize      = targetSize;
    optParams.backbufferSubrectSize   = targetSize;
    optParams.outputInterpSubrectSize = targetSize;

    CmdLabel label( cmd, "DLSS FG" );
    NVSDK_NGX_Result r = NGX_VK_EVALUATE_DLSSG(
        cmd, pDlssgFeature, ngxManager->GetParameters(), &evalParams, &optParams );

    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS FG: NGX_VK_EVALUATE_DLSSG failed: 0x{:08X}", static_cast< uint32_t >( r ) );
    }
}

VkResult RTGL1::FrameGeneratorDLSS::CreateSwapchain(
    const VkSwapchainCreateInfoKHR& createInfo,
    VkSwapchainKHR*                 outSwapchain )
{
    return vkCreateSwapchainKHR( device, &createInfo, nullptr, outSwapchain );
}

void RTGL1::FrameGeneratorDLSS::DestroySwapchain( VkSwapchainKHR swapchain )
{
    if( swapchain != VK_NULL_HANDLE )
    {
        vkDestroySwapchainKHR( device, swapchain, nullptr );
    }
}

VkResult RTGL1::FrameGeneratorDLSS::AcquireNextImage(
    VkDevice       device_,
    VkSwapchainKHR swapchain,
    uint64_t       timeout,
    VkSemaphore    semaphore,
    uint32_t*      outImageIndex )
{
    return vkAcquireNextImageKHR( device_, swapchain, timeout, semaphore, VK_NULL_HANDLE, outImageIndex );
}

VkResult RTGL1::FrameGeneratorDLSS::QueuePresent(
    VkQueue                 queue,
    const VkPresentInfoKHR& presentInfo )
{
    return vkQueuePresentKHR( queue, &presentInfo );
}

VkResult RTGL1::FrameGeneratorDLSS::GetSwapchainImages(
    VkDevice       device_,
    VkSwapchainKHR swapchain,
    uint32_t*      pCount,
    VkImage*       pImages )
{
    return vkGetSwapchainImagesKHR( device_, swapchain, pCount, pImages );
}

void RTGL1::FrameGeneratorDLSS::WaitForPresents()
{
    vkDeviceWaitIdle( device );
}

#else

RTGL1::FrameGeneratorDLSS::FrameGeneratorDLSS(
    VkDevice                           _device,
    VkPhysicalDevice                   _physDevice,
    std::shared_ptr< MemoryAllocator > _allocator,
    std::shared_ptr< Queues >          _queues,
    std::shared_ptr< NgxManager >      _ngxManager,
    VkFormat                           _backBufferFormat )
{
}

RTGL1::FrameGeneratorDLSS::~FrameGeneratorDLSS() = default;

bool RTGL1::FrameGeneratorDLSS::IsAvailable() const
{
    return false;
}

RTGL1::IFrameGeneratorSwapchain* RTGL1::FrameGeneratorDLSS::GetSwapchainHooks()
{
    return nullptr;
}

bool RTGL1::FrameGeneratorDLSS::IsActive() const
{
    return false;
}

void RTGL1::FrameGeneratorDLSS::OnFramebuffersSizeChange( const ResolutionState& resolutionState ) {}

void RTGL1::FrameGeneratorDLSS::PrepareFrame(
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
}

void RTGL1::FrameGeneratorDLSS::CaptureHudless(
    VkCommandBuffer cmd,
    uint32_t        frameIndex,
    VkImage         srcImage,
    uint32_t        width,
    uint32_t        height )
{
}

void RTGL1::FrameGeneratorDLSS::ConfigureFrame(
    VkSwapchainKHR swapchain,
    uint32_t       frameIndex,
    uint32_t       width,
    uint32_t       height,
    uint32_t       frameId,
    bool           frameGenEnabled )
{
}

VkResult RTGL1::FrameGeneratorDLSS::CreateSwapchain(
    const VkSwapchainCreateInfoKHR& createInfo,
    VkSwapchainKHR*                 outSwapchain )
{
    return VK_ERROR_INITIALIZATION_FAILED;
}

void RTGL1::FrameGeneratorDLSS::DestroySwapchain( VkSwapchainKHR swapchain ) {}

VkResult RTGL1::FrameGeneratorDLSS::AcquireNextImage(
    VkDevice       device,
    VkSwapchainKHR swapchain,
    uint64_t       timeout,
    VkSemaphore    semaphore,
    uint32_t*      outImageIndex )
{
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult RTGL1::FrameGeneratorDLSS::QueuePresent(
    VkQueue                 queue,
    const VkPresentInfoKHR& presentInfo )
{
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult RTGL1::FrameGeneratorDLSS::GetSwapchainImages(
    VkDevice       device,
    VkSwapchainKHR swapchain,
    uint32_t*      pCount,
    VkImage*       pImages )
{
    return VK_ERROR_INITIALIZATION_FAILED;
}

void RTGL1::FrameGeneratorDLSS::WaitForPresents() {}

#endif
