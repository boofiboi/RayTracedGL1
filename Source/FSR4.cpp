#include "FSR4.h"

#ifdef RG_USE_AMD_FSR4

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/dx12/ffx_api_dx12.h>

#include "RenderResolutionHelper.h"
#include "RgException.h"

#include <cstring>
#include <cmath>

namespace
{
static ffxFunctions ffxFuncs{};
static constexpr RTGL1::FramebufferImageIndex OUTPUT_IMAGE_INDEX =
    RTGL1::FramebufferImageIndex::FB_IMAGE_INDEX_UPSCALED_PONG;
}

bool RTGL1::FSR4::IsHardwareSupported( VkPhysicalDevice physDevice )
{
    if( physDevice == VK_NULL_HANDLE )
    {
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties( physDevice, &props );

    if( props.vendorID != 0x1002 )
    {
        return false;
    }

    if( props.deviceID >= 0x7440 )
    {
        return true;
    }

    const char* name = props.deviceName;
    if( name != nullptr )
    {
        if( strstr( name, "RX 7" ) || strstr( name, "RX 8" ) || strstr( name, "RX 9" ) ||
            strstr( name, "Radeon 7" ) || strstr( name, "Radeon 8" ) || strstr( name, "Radeon 9" ) ||
            strstr( name, "RDNA3" ) || strstr( name, "RDNA4" ) || strstr( name, "RDNA 3" ) || strstr( name, "RDNA 4" ) ||
            strstr( name, "Navi 3" ) || strstr( name, "Navi 4" ) || strstr( name, "Navi3" ) || strstr( name, "Navi4" ) ||
            strstr( name, "gfx11" ) || strstr( name, "gfx12" ) )
        {
            return true;
        }
    }

    return false;
}

bool RTGL1::FSR4::IsFsr4Available() const
{
    return isSupported;
}

RTGL1::FSR4::FSR4( VkInstance _instance, VkDevice _device, std::shared_ptr< PhysicalDevice > _physDevice, bool _enableFrameGeneration )
    : instance( _instance )
    , device( _device )
    , physDevice( std::move( _physDevice ) )
    , enableFrameGeneration( _enableFrameGeneration )
    , isSupported( IsHardwareSupported( physDevice ? physDevice->Get() : VK_NULL_HANDLE ) )
{
    if( isSupported )
    {
        dx12 = std::make_unique< Dx12Interop >();
        if( !dx12->Init( instance, physDevice, device ) )
        {
            isSupported = false;
        }
        else
        {
            ffxModule = LoadLibraryA( "amd_fidelityfx_loader_dx12.dll" );
            if( !ffxModule )
            {
                ffxModule = LoadLibraryA( "rt_bin/amd_fidelityfx_loader_dx12.dll" );
            }

            if( ffxModule )
            {
                ffxLoadFunctions( &ffxFuncs, ffxModule );
                if( !ffxFuncs.CreateContext || !ffxFuncs.Dispatch || !ffxFuncs.DestroyContext )
                {
                    isSupported = false;
                }
            }
            else
            {
                isSupported = false;
            }
        }
    }
}

RTGL1::FSR4::~FSR4()
{
    DestroyResources();

    if( dx12 )
    {
        dx12->Destroy();
        dx12.reset();
    }

    if( ffxModule )
    {
        FreeLibrary( ffxModule );
        ffxModule = nullptr;
    }
}

void RTGL1::FSR4::DestroyResources()
{
    if( isContextCreated && context && ffxFuncs.DestroyContext )
    {
        ffxContext ctx = ( ffxContext )context;
        ffxFuncs.DestroyContext( &ctx, nullptr );
        context = nullptr;
        isContextCreated = false;
    }
}

void RTGL1::FSR4::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
    if( !isSupported || !dx12 || !ffxFuncs.CreateContext )
    {
        return;
    }

    DestroyResources();

    if( !dx12->RecreateSharedTextures( resolutionState.renderWidth,
                                       resolutionState.renderHeight,
                                       resolutionState.upscaledWidth,
                                       resolutionState.upscaledHeight ) )
    {
        isContextCreated = false;
        return;
    }

    ffxCreateBackendDX12Desc backendDesc = {
        .header = {
            .type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,
            .pNext = nullptr,
        },
        .device = dx12->GetDevice(),
    };

    ffxCreateContextDescUpscaleVersion versionDesc = {
        .header = {
            .type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION,
            .pNext = &backendDesc.header,
        },
        .version = FFX_UPSCALER_VERSION,
    };

    ffxCreateContextDescUpscale createDesc = {
        .header = {
            .type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
            .pNext = &versionDesc.header,
        },
        .flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE,
        .maxRenderSize = { resolutionState.renderWidth, resolutionState.renderHeight },
        .maxUpscaleSize = { resolutionState.upscaledWidth, resolutionState.upscaledHeight },
        .fpMessage = nullptr,
    };

    ffxContext ctx = nullptr;
    ffxReturnCode_t r = ffxFuncs.CreateContext( &ctx, &createDesc.header, nullptr );
    if( r == FFX_API_RETURN_OK && ctx != nullptr )
    {
        context = ( void* )ctx;
        isContextCreated = true;
    }
    else
    {
        isContextCreated = false;
    }
}

RTGL1::FramebufferImageIndex RTGL1::FSR4::Apply(
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
    if( !isSupported || !dx12 || !ffxFuncs.Dispatch )
    {
        return OUTPUT_IMAGE_INDEX;
    }

    if( !isContextCreated || context == nullptr )
    {
        OnFramebuffersSizeChange( renderResolution.GetResolutionState() );
        if( !isContextCreated || context == nullptr )
        {
            return OUTPUT_IMAGE_INDEX;
        }
    }

    const auto& sc = dx12->GetColorTexture();
    const auto& sd = dx12->GetDepthTexture();
    const auto& sm = dx12->GetMotionTexture();
    const auto& so = dx12->GetOutputTexture();

    if( sc.vkImage == VK_NULL_HANDLE || sd.vkImage == VK_NULL_HANDLE ||
        sm.vkImage == VK_NULL_HANDLE || so.vkImage == VK_NULL_HANDLE )
    {
        return OUTPUT_IMAGE_INDEX;
    }

    uint32_t rW = renderResolution.GetResolutionState().renderWidth;
    uint32_t rH = renderResolution.GetResolutionState().renderHeight;
    uint32_t uW = renderResolution.GetResolutionState().upscaledWidth;
    uint32_t uH = renderResolution.GetResolutionState().upscaledHeight;

    const VkImageSubresourceRange subresRange = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    const VkImageSubresourceLayers subresLayers = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel       = 0,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    VkImage srcColor  = framebuffers->GetImage( FramebufferImageIndex::FB_IMAGE_INDEX_FINAL, frameIndex );
    VkImage srcDepth  = framebuffers->GetImage( FramebufferImageIndex::FB_IMAGE_INDEX_DEPTH_NDC, frameIndex );
    VkImage srcMotion = framebuffers->GetImage( FramebufferImageIndex::FB_IMAGE_INDEX_MOTION_DLSS, frameIndex );
    VkImage dstPong   = framebuffers->GetImage( OUTPUT_IMAGE_INDEX, frameIndex );

    VkImageMemoryBarrier2 preCopyBarriers[] = {
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = srcColor,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = srcDepth,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = srcMotion,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask       = VK_ACCESS_2_NONE,
            .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = sc.vkImage,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask       = VK_ACCESS_2_NONE,
            .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = sd.vkImage,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask       = VK_ACCESS_2_NONE,
            .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = sm.vkImage,
            .subresourceRange    = subresRange,
        },
    };

    VkDependencyInfo preDep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = std::size( preCopyBarriers ),
        .pImageMemoryBarriers    = preCopyBarriers,
    };
    svkCmdPipelineBarrier2KHR( cmd, &preDep );

    VkImageCopy copyRenderRegion = {
        .srcSubresource = subresLayers,
        .srcOffset      = { 0, 0, 0 },
        .dstSubresource = subresLayers,
        .dstOffset      = { 0, 0, 0 },
        .extent         = { rW, rH, 1 },
    };
    vkCmdCopyImage( cmd, srcColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRenderRegion );
    vkCmdCopyImage( cmd, srcDepth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sd.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRenderRegion );
    vkCmdCopyImage( cmd, srcMotion, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sm.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRenderRegion );

    VkImageMemoryBarrier2 postCopyBarriers[] = {
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = srcColor,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = srcDepth,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = srcMotion,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = sc.vkImage,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = sd.vkImage,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = sm.vkImage,
            .subresourceRange    = subresRange,
        },
    };

    VkDependencyInfo postDep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = std::size( postCopyBarriers ),
        .pImageMemoryBarriers    = postCopyBarriers,
    };
    svkCmdPipelineBarrier2KHR( cmd, &postDep );

    dx12->BeginCommands();
    ID3D12GraphicsCommandList* cl = dx12->GetCommandList();

    ffxDispatchDescUpscale dispatchDesc = {
        .header                  = { .type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE },
        .commandList             = cl,
        .color                   = ffxApiGetResourceDX12( sc.d3d12Resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ ),
        .depth                   = ffxApiGetResourceDX12( sd.d3d12Resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ ),
        .motionVectors           = ffxApiGetResourceDX12( sm.d3d12Resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ ),
        .exposure                = {},
        .reactive                = {},
        .transparencyAndComposition = {},
        .output                  = ffxApiGetResourceDX12( so.d3d12Resource.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS ),
        .jitterOffset            = { -jitterOffset.data[ 0 ], -jitterOffset.data[ 1 ] },
        .motionVectorScale       = { float( rW ), float( rH ) },
        .renderSize              = { rW, rH },
        .upscaleSize             = { uW, uH },
        .enableSharpening        = renderResolution.IsCASInsideFSR3(),
        .sharpness               = renderResolution.GetSharpeningIntensity(),
        .frameTimeDelta          = float( timeDelta * 1000.0 ),
        .preExposure             = 1.0f,
        .reset                   = resetAccumulation,
        .cameraNear              = nearPlane,
        .cameraFar               = farPlane,
        .cameraFovAngleVertical  = fovVerticalRad,
        .viewSpaceToMetersFactor = 1.0f,
        .flags                   = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB,
    };

    ffxContext ctx = ( ffxContext )context;
    ffxFuncs.Dispatch( &ctx, &dispatchDesc.header );

    dx12->EndAndExecuteCommands();
    dx12->WaitForGpu();

    VkImageMemoryBarrier2 preOutputBarriers[] = {
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask       = VK_ACCESS_2_NONE,
            .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = so.vkImage,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask       = VK_ACCESS_2_NONE,
            .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = dstPong,
            .subresourceRange    = subresRange,
        },
    };

    VkDependencyInfo preOutputDep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = std::size( preOutputBarriers ),
        .pImageMemoryBarriers    = preOutputBarriers,
    };
    svkCmdPipelineBarrier2KHR( cmd, &preOutputDep );

    VkImageCopy copyUpscaleRegion = {
        .srcSubresource = subresLayers,
        .srcOffset      = { 0, 0, 0 },
        .dstSubresource = subresLayers,
        .dstOffset      = { 0, 0, 0 },
        .extent         = { uW, uH, 1 },
    };
    vkCmdCopyImage( cmd, so.vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstPong, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyUpscaleRegion );

    VkImageMemoryBarrier2 postOutputBarriers[] = {
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = so.vkImage,
            .subresourceRange    = subresRange,
        },
        {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = dstPong,
            .subresourceRange    = subresRange,
        },
    };

    VkDependencyInfo postOutputDep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = std::size( postOutputBarriers ),
        .pImageMemoryBarriers    = postOutputBarriers,
    };
    svkCmdPipelineBarrier2KHR( cmd, &postOutputDep );

    return OUTPUT_IMAGE_INDEX;
}

namespace
{
int32_t GetFsr4JitterPhaseCount( int32_t renderWidth, int32_t displayWidth )
{
    const float basePhaseCount = 8.0f;
    const float jitterPhaseCount = basePhaseCount * powf( float( displayWidth ) / float( renderWidth ), 2.0f );
    return int32_t( ceilf( jitterPhaseCount ) );
}

float Halton( int32_t index, int32_t base )
{
    float f = 1.0f;
    float r = 0.0f;
    while( index > 0 )
    {
        f = f / float( base );
        r = r + f * float( index % base );
        index = index / base;
    }
    return r;
}
}

RgFloat2D RTGL1::FSR4::GetJitter( const ResolutionState& resolutionState, uint32_t frameId )
{
    const int32_t jitterPhaseCount = GetFsr4JitterPhaseCount(
        int32_t( resolutionState.renderWidth ), int32_t( resolutionState.upscaledWidth ) );

    const int32_t index = int32_t( frameId % uint32_t( jitterPhaseCount ) ) + 1;
    const float x = Halton( index, 2 ) - 0.5f;
    const float y = Halton( index, 3 ) - 0.5f;

    return RgFloat2D{ x, y };
}

#else

using namespace RTGL1;

FSR4::FSR4( VkInstance instance, VkDevice device, std::shared_ptr< PhysicalDevice > physDevice, bool enableFrameGeneration )
    : instance( instance ), device( device ), physDevice( std::move( physDevice ) ), enableFrameGeneration( enableFrameGeneration ), isSupported( false )
{
}

FSR4::~FSR4() = default;

void FSR4::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
}

FramebufferImageIndex FSR4::Apply( VkCommandBuffer                        cmd,
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
    return FramebufferImageIndex::FB_IMAGE_INDEX_UPSCALED_PONG;
}

RgFloat2D FSR4::GetJitter( const ResolutionState& resolutionState, uint32_t frameId )
{
    return RgFloat2D{ 0, 0 };
}

bool FSR4::IsHardwareSupported( VkPhysicalDevice physDevice )
{
    return false;
}

bool FSR4::IsFsr4Available() const
{
    return false;
}

#endif
