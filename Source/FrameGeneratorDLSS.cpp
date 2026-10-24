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
#include "Utils.h"

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
    , backbufferImages{}
    , backbufferViews{}
    , backbufferMemories{}
    , interpImages{}
    , interpViews{}
    , interpMemories{}
    , featureWidth( 0 )
    , featureHeight( 0 )
    , displayWidth( 0 )
    , displayHeight( 0 )
    , isFrameGenEnabled( false )
    , pendingReset( true )
    , blitCommandPool( VK_NULL_HANDLE )
    , blitCmd{}
    , interpAcquireSemaphores{}
    , interpRenderFinishedSemaphores{}
    , realRenderFinishedSemaphores{}
    , blitFences{}
    , currentSwapchainHandle( VK_NULL_HANDLE )
    , currentFrameIndex( 0 )
    , swapchainHandleAlive( false )
    , pacerThread{}
    , pacerStop( false )
    , taskBusy( false )
    , pendingTask{}
    , lastRealPresentQpc( 0 )
    , avgFrameDeltaQpc( 0 )
    , qpcFrequency( 0 )
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency( &freq );
    qpcFrequency = freq.QuadPart;

    pacerThread = std::thread( &FrameGeneratorDLSS::PacerThreadFunc, this );

    VkCommandPoolCreateInfo poolInfo = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queues->GetIndexGraphics(),
    };
    VkResult r = vkCreateCommandPool( device, &poolInfo, nullptr, &blitCommandPool );
    VK_CHECKERROR( r );

    VkCommandBufferAllocateInfo allocInfo = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = blitCommandPool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MAX_FRAMES_IN_FLIGHT,
    };
    r = vkAllocateCommandBuffers( device, &allocInfo, blitCmd );
    VK_CHECKERROR( r );

    VkSemaphoreCreateInfo semInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    for( uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++ )
    {
        r = vkCreateSemaphore( device, &semInfo, nullptr, &interpAcquireSemaphores[ i ] );
        VK_CHECKERROR( r );
        r = vkCreateSemaphore( device, &semInfo, nullptr, &interpRenderFinishedSemaphores[ i ] );
        VK_CHECKERROR( r );
        r = vkCreateSemaphore( device, &semInfo, nullptr, &realRenderFinishedSemaphores[ i ] );
        VK_CHECKERROR( r );
        r = vkCreateFence( device, &fenceInfo, nullptr, &blitFences[ i ] );
        VK_CHECKERROR( r );
    }
}

RTGL1::FrameGeneratorDLSS::~FrameGeneratorDLSS()
{
    {
        std::unique_lock< std::mutex > lock( pacerMutex );
        pacerStop = true;
        pacerCv.notify_all();
    }
    if( pacerThread.joinable() )
    {
        pacerThread.join();
    }

    DestroyDlssgFeature();
    DestroyHudlessImages();
    DestroyBackbufferImages();
    DestroyInterpImages();

    for( uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++ )
    {
        if( interpAcquireSemaphores[ i ] != VK_NULL_HANDLE )
        {
            vkDestroySemaphore( device, interpAcquireSemaphores[ i ], nullptr );
            interpAcquireSemaphores[ i ] = VK_NULL_HANDLE;
        }
        if( interpRenderFinishedSemaphores[ i ] != VK_NULL_HANDLE )
        {
            vkDestroySemaphore( device, interpRenderFinishedSemaphores[ i ], nullptr );
            interpRenderFinishedSemaphores[ i ] = VK_NULL_HANDLE;
        }
        if( realRenderFinishedSemaphores[ i ] != VK_NULL_HANDLE )
        {
            vkDestroySemaphore( device, realRenderFinishedSemaphores[ i ], nullptr );
            realRenderFinishedSemaphores[ i ] = VK_NULL_HANDLE;
        }
        if( blitFences[ i ] != VK_NULL_HANDLE )
        {
            vkDestroyFence( device, blitFences[ i ], nullptr );
            blitFences[ i ] = VK_NULL_HANDLE;
        }
    }

    if( blitCommandPool != VK_NULL_HANDLE )
    {
        vkDestroyCommandPool( device, blitCommandPool, nullptr );
        blitCommandPool = VK_NULL_HANDLE;
    }
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
    return IsAvailable() && swapchainHandleAlive;
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
    CreateBackbufferImages( resolutionState.upscaledWidth, resolutionState.upscaledHeight );
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

void RTGL1::FrameGeneratorDLSS::CreateBackbufferImages( uint32_t width, uint32_t height )
{
    DestroyBackbufferImages();

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

        VkResult r = vkCreateImage( device, &imageInfo, nullptr, &backbufferImages[ i ] );
        VK_CHECKERROR( r );

        VkMemoryRequirements memReqs = {};
        vkGetImageMemoryRequirements( device, backbufferImages[ i ], &memReqs );

        backbufferMemories[ i ] = allocator->AllocDedicated( memReqs,
                                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                             MemoryAllocator::AllocType::DEFAULT,
                                                             "DLSS FG Backbuffer Image" );

        r = vkBindImageMemory( device, backbufferImages[ i ], backbufferMemories[ i ], 0 );
        VK_CHECKERROR( r );

        VkImageViewCreateInfo viewInfo = {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = backbufferImages[ i ],
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

        r = vkCreateImageView( device, &viewInfo, nullptr, &backbufferViews[ i ] );
        VK_CHECKERROR( r );
    }
}

void RTGL1::FrameGeneratorDLSS::DestroyBackbufferImages()
{
    for( uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++ )
    {
        if( backbufferViews[ i ] != VK_NULL_HANDLE )
        {
            vkDestroyImageView( device, backbufferViews[ i ], nullptr );
            backbufferViews[ i ] = VK_NULL_HANDLE;
        }
        if( backbufferImages[ i ] != VK_NULL_HANDLE )
        {
            vkDestroyImage( device, backbufferImages[ i ], nullptr );
            backbufferImages[ i ] = VK_NULL_HANDLE;
        }
        if( backbufferMemories[ i ] != VK_NULL_HANDLE )
        {
            MemoryAllocator::FreeDedicated( device, backbufferMemories[ i ] );
            backbufferMemories[ i ] = VK_NULL_HANDLE;
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

void RTGL1::FrameGeneratorDLSS::CaptureBackbuffer(
    VkCommandBuffer cmd,
    uint32_t        frameIndex,
    VkImage         srcImage,
    uint32_t        width,
    uint32_t        height )
{
    if( !IsAvailable() || backbufferImages[ frameIndex ] == VK_NULL_HANDLE || srcImage == VK_NULL_HANDLE )
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
        .image         = backbufferImages[ frameIndex ],
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
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
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
                    backbufferImages[ frameIndex ],
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &blitRegion,
                    VK_FILTER_NEAREST );

    barriers[ 0 ].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barriers[ 0 ].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barriers[ 0 ].dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[ 0 ].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barriers[ 0 ].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[ 0 ].newLayout     = VK_IMAGE_LAYOUT_GENERAL;

    barriers[ 1 ].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barriers[ 1 ].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[ 1 ].dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    barriers[ 1 ].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
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
    displayWidth           = width;
    displayHeight          = height;
    isFrameGenEnabled      = frameGenEnabled;
    currentFrameIndex      = frameIndex;
    currentSwapchainHandle = swapchain;
}

void RTGL1::FrameGeneratorDLSS::PrepareFrame(
    VkCommandBuffer                        cmd,
    uint32_t                               frameIndex,
    VkImage                                backbufferSrcImage,
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
        isFrameGenEnabled = false;
        return;
    }

    if( hudlessViews[ frameIndex ] == VK_NULL_HANDLE || interpViews[ frameIndex ] == VK_NULL_HANDLE ||
        backbufferViews[ frameIndex ] == VK_NULL_HANDLE )
    {
        isFrameGenEnabled = false;
        return;
    }

    CaptureBackbuffer(
        cmd, frameIndex, backbufferSrcImage, renderResolution.UpscaledWidth(), renderResolution.UpscaledHeight() );

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
        ToNGXResourceFromView( backbufferImages[ frameIndex ], backbufferViews[ frameIndex ], backBufferFormat, targetSize );
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
        isFrameGenEnabled = false;
    }
}

VkResult RTGL1::FrameGeneratorDLSS::CreateSwapchain(
    const VkSwapchainCreateInfoKHR& createInfo,
    VkSwapchainKHR*                 outSwapchain )
{
    VkResult r = vkCreateSwapchainKHR( device, &createInfo, nullptr, outSwapchain );
    swapchainHandleAlive = ( r == VK_SUCCESS );
    if( swapchainHandleAlive && outSwapchain != nullptr )
    {
        currentSwapchainHandle = *outSwapchain;
    }
    else
    {
        currentSwapchainHandle = VK_NULL_HANDLE;
    }
    return r;
}

void RTGL1::FrameGeneratorDLSS::DestroySwapchain( VkSwapchainKHR swapchain )
{
    WaitForPresents();
    swapchainImages.clear();
    currentSwapchainHandle = VK_NULL_HANDLE;
    swapchainHandleAlive   = false;
    lastRealPresentQpc     = 0;
    avgFrameDeltaQpc       = 0;
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
    if( !isFrameGenEnabled || !swapchainHandleAlive || swapchainImages.empty() ||
        interpImages[ currentFrameIndex ] == VK_NULL_HANDLE ||
        currentSwapchainHandle == VK_NULL_HANDLE )
    {
        isFrameGenEnabled = false;
        WaitForPresents();
        return vkQueuePresentKHR( queue, &presentInfo );
    }

    isFrameGenEnabled = false;

    WaitForPresents();

    vkWaitForFences( device, 1, &blitFences[ currentFrameIndex ], VK_TRUE, UINT64_MAX );
    vkResetFences( device, 1, &blitFences[ currentFrameIndex ] );

    uint32_t interpImageIndex = UINT32_MAX;
    VkResult acquireRes = vkAcquireNextImageKHR(
        device,
        currentSwapchainHandle,
        UINT64_MAX,
        interpAcquireSemaphores[ currentFrameIndex ],
        VK_NULL_HANDLE,
        &interpImageIndex );

    if( acquireRes != VK_SUCCESS && acquireRes != VK_SUBOPTIMAL_KHR )
    {
        return vkQueuePresentKHR( queue, &presentInfo );
    }

    if( interpImageIndex >= swapchainImages.size() )
    {
        return vkQueuePresentKHR( queue, &presentInfo );
    }

    VkCommandBuffer cmd = blitCmd[ currentFrameIndex ];
    vkResetCommandBuffer( cmd, 0 );

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer( cmd, &beginInfo );

    VkImage interpSrc = interpImages[ currentFrameIndex ];
    VkImage swapDst   = swapchainImages[ interpImageIndex ];

    Utils::BarrierImage( cmd,
                         interpSrc,
                         VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );

    Utils::BarrierImage( cmd,
                         swapDst,
                         0,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

    VkImageBlit blitRegion = {};
    blitRegion.srcSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blitRegion.srcOffsets[ 0 ] = { 0, 0, 0 };
    blitRegion.srcOffsets[ 1 ] = { static_cast< int32_t >( displayWidth ),
                                   static_cast< int32_t >( displayHeight ),
                                   1 };
    blitRegion.dstSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blitRegion.dstOffsets[ 0 ] = { 0, 0, 0 };
    blitRegion.dstOffsets[ 1 ] = { static_cast< int32_t >( displayWidth ),
                                   static_cast< int32_t >( displayHeight ),
                                   1 };

    vkCmdBlitImage( cmd,
                    interpSrc,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    swapDst,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &blitRegion,
                    VK_FILTER_NEAREST );

    Utils::BarrierImage( cmd,
                         interpSrc,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_GENERAL );

    Utils::BarrierImage( cmd,
                         swapDst,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         0,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR );

    vkEndCommandBuffer( cmd );

    VkSemaphore waitSemaphores[ 2 ] = {
        presentInfo.waitSemaphoreCount > 0 ? presentInfo.pWaitSemaphores[ 0 ] : VK_NULL_HANDLE,
        interpAcquireSemaphores[ currentFrameIndex ],
    };
    VkPipelineStageFlags waitStages[ 2 ] = {
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
    };
    uint32_t waitCount = ( waitSemaphores[ 0 ] != VK_NULL_HANDLE ) ? 2 : 1;
    if( waitCount == 1 )
    {
        waitSemaphores[ 0 ] = interpAcquireSemaphores[ currentFrameIndex ];
    }

    VkSemaphore signalSemaphores[ 2 ] = {
        interpRenderFinishedSemaphores[ currentFrameIndex ],
        realRenderFinishedSemaphores[ currentFrameIndex ],
    };

    VkSubmitInfo submitInfo = {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount   = waitCount,
        .pWaitSemaphores      = waitSemaphores,
        .pWaitDstStageMask    = waitStages,
        .commandBufferCount   = 1,
        .pCommandBuffers      = &cmd,
        .signalSemaphoreCount = 2,
        .pSignalSemaphores    = signalSemaphores,
    };

    VkResult submitRes = vkQueueSubmit( queue, 1, &submitInfo, blitFences[ currentFrameIndex ] );
    if( submitRes != VK_SUCCESS )
    {
        return vkQueuePresentKHR( queue, &presentInfo );
    }

    LARGE_INTEGER curQpc;
    QueryPerformanceCounter( &curQpc );
    if( lastRealPresentQpc > 0 && qpcFrequency > 0 )
    {
        int64_t deltaQpc = curQpc.QuadPart - lastRealPresentQpc;
        if( deltaQpc > 0 && deltaQpc < ( qpcFrequency / 5 ) )
        {
            if( avgFrameDeltaQpc == 0 )
            {
                avgFrameDeltaQpc = deltaQpc;
            }
            else
            {
                avgFrameDeltaQpc = ( avgFrameDeltaQpc * 7 + deltaQpc ) / 8;
            }
        }
    }
    lastRealPresentQpc = curQpc.QuadPart;

    int64_t halfInterval = avgFrameDeltaQpc / 2;
    int64_t minInterval  = ( qpcFrequency * 1 ) / 1000;
    int64_t maxInterval  = ( qpcFrequency * 40 ) / 1000;
    halfInterval = std::clamp( halfInterval, minInterval, maxInterval );

    int64_t targetRealQpc = curQpc.QuadPart + halfInterval;

    {
        std::unique_lock< std::mutex > lock( pacerMutex );
        pendingTask.queue               = queue;
        pendingTask.swapchain           = ( presentInfo.swapchainCount > 0 && presentInfo.pSwapchains != nullptr )
                                              ? presentInfo.pSwapchains[ 0 ]
                                              : currentSwapchainHandle;
        pendingTask.interpImageIndex    = interpImageIndex;
        pendingTask.realImageIndex      = ( presentInfo.swapchainCount > 0 && presentInfo.pImageIndices != nullptr )
                                              ? presentInfo.pImageIndices[ 0 ]
                                              : 0;
        pendingTask.interpWaitSemaphore = interpRenderFinishedSemaphores[ currentFrameIndex ];
        pendingTask.realWaitSemaphore   = realRenderFinishedSemaphores[ currentFrameIndex ];
        pendingTask.targetRealQpc       = targetRealQpc;
        pendingTask.valid               = true;
        taskBusy                        = true;
        pacerCv.notify_one();
    }

    return VK_SUCCESS;
}

VkResult RTGL1::FrameGeneratorDLSS::GetSwapchainImages(
    VkDevice       device_,
    VkSwapchainKHR swapchain,
    uint32_t*      pCount,
    VkImage*       pImages )
{
    VkResult r = vkGetSwapchainImagesKHR( device_, swapchain, pCount, pImages );
    if( r == VK_SUCCESS && pImages != nullptr && pCount != nullptr )
    {
        swapchainImages.assign( pImages, pImages + *pCount );
        currentSwapchainHandle = swapchain;
        swapchainHandleAlive   = true;
    }
    return r;
}

void RTGL1::FrameGeneratorDLSS::WaitForPresents()
{
    std::unique_lock< std::mutex > lock( pacerMutex );
    pacerDoneCv.wait( lock, [ this ] { return !taskBusy; } );

    if( queues )
    {
        vkQueueWaitIdle( queues->GetGraphics() );
    }
    else if( device != VK_NULL_HANDLE )
    {
        vkDeviceWaitIdle( device );
    }
}

void RTGL1::FrameGeneratorDLSS::PacerThreadFunc()
{
    while( true )
    {
        PresentTask task;
        {
            std::unique_lock< std::mutex > lock( pacerMutex );
            pacerCv.wait( lock, [ this ] { return pacerStop || pendingTask.valid; } );

            if( pacerStop && !pendingTask.valid )
            {
                break;
            }

            task              = pendingTask;
            pendingTask.valid = false;
        }

        if( task.queue != VK_NULL_HANDLE && task.swapchain != VK_NULL_HANDLE )
        {
            VkPresentInfoKHR interpPresent = {
                .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores    = &task.interpWaitSemaphore,
                .swapchainCount     = 1,
                .pSwapchains        = &task.swapchain,
                .pImageIndices      = &task.interpImageIndex,
                .pResults           = nullptr,
            };
            vkQueuePresentKHR( task.queue, &interpPresent );

            if( task.targetRealQpc > 0 && qpcFrequency > 0 )
            {
                int64_t sleepThreshold = ( qpcFrequency * 2 ) / 1000;
                while( true )
                {
                    LARGE_INTEGER cur;
                    QueryPerformanceCounter( &cur );
                    int64_t rem = task.targetRealQpc - cur.QuadPart;
                    if( rem <= 0 )
                    {
                        break;
                    }
                    if( rem > sleepThreshold )
                    {
                        Sleep( 1 );
                    }
                    else
                    {
                        YieldProcessor();
                    }
                }
            }

            VkPresentInfoKHR realPresent = {
                .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores    = &task.realWaitSemaphore,
                .swapchainCount     = 1,
                .pSwapchains        = &task.swapchain,
                .pImageIndices      = &task.realImageIndex,
                .pResults           = nullptr,
            };
            vkQueuePresentKHR( task.queue, &realPresent );
        }

        {
            std::unique_lock< std::mutex > lock( pacerMutex );
            taskBusy = false;
            pacerDoneCv.notify_all();
        }
    }
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
    VkImage                                backbufferSrcImage,
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
