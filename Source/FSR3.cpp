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

#include "FSR3.h"

#ifdef RG_USE_AMD_FSR3

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include "RenderResolutionHelper.h"
#include "RgException.h"

namespace
{
void CheckError( FfxErrorCode r )
{
    if( r != FFX_OK )
    {
        RTGL1::debug::Error( "FSR3: Fail, FfxErrorCode={}", r );
        throw RTGL1::RgException( RG_RESULT_GRAPHICS_API_ERROR, "Can't initialize FSR3" );
    }
}
}

RTGL1::FSR3::FSR3( VkDevice _device, VkPhysicalDevice _physDevice )
    : device( _device )
    , physDevice( _physDevice )
    , context( std::make_unique< FfxFsr3UpscalerContext >() )
{
    memset( context.get(), 0, sizeof( FfxFsr3UpscalerContext ) );
}

RTGL1::FSR3::~FSR3()
{
    if( isContextCreated && context )
    {
        ffxFsr3UpscalerContextDestroy( context.get() );
    }
}

void RTGL1::FSR3::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
    if( isContextCreated && context )
    {
        ffxFsr3UpscalerContextDestroy( context.get() );
        memset( context.get(), 0, sizeof( FfxFsr3UpscalerContext ) );
        isContextCreated = false;
    }

    FfxFsr3UpscalerContextDescription contextDesc = {
        .flags         = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
                         FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE,
        .maxRenderSize = { resolutionState.renderWidth, resolutionState.renderHeight },
        .maxUpscaleSize = { resolutionState.upscaledWidth, resolutionState.upscaledHeight },
        .fpMessage     = nullptr,
        .backendInterface = {},
    };

    VkDeviceContext vkDeviceContext = {
        .vkDevice         = device,
        .vkPhysicalDevice = physDevice,
        .vkDeviceProcAddr = vkGetDeviceProcAddr,
    };
    FfxDevice ffxDevice = ffxGetDeviceVK( &vkDeviceContext );

    const size_t scratchBufferSize = ffxGetScratchMemorySizeVK( physDevice, FFX_FSR3UPSCALER_CONTEXT_COUNT );
    scratchBuffer.resize( scratchBufferSize );

    FfxErrorCode r = ffxGetInterfaceVK(
        &contextDesc.backendInterface,
        ffxDevice,
        scratchBuffer.data(),
        scratchBufferSize,
        FFX_FSR3UPSCALER_CONTEXT_COUNT );
    CheckError( r );

    r = ffxFsr3UpscalerContextCreate( context.get(), &contextDesc );
    CheckError( r );

    isContextCreated = true;
}

namespace
{
constexpr RTGL1::FramebufferImageIndex OUTPUT_IMAGE_INDEX = RTGL1::FB_IMAGE_INDEX_UPSCALED_PONG;

FfxResource ToFSR3Resource( RTGL1::FramebufferImageIndex  fbImage,
                            uint32_t                      frameIndex,
                            const RTGL1::Framebuffers&    framebuffers,
                            const RTGL1::ResolutionState& resolutionState,
                            FfxResourceStates             state,
                            const wchar_t*                name = nullptr )
{
    auto [ image, view, format, sz ] =
        framebuffers.GetImageHandles( fbImage, frameIndex, resolutionState );

    if( image == VK_NULL_HANDLE )
    {
        return FfxResource{};
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
        .usage       = VkImageUsageFlags( ( fbImage == OUTPUT_IMAGE_INDEX )
                           ? ( VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT )
                           : ( VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT ) ),
    };

    FfxResourceDescription resDesc = ffxGetImageResourceDescriptionVK( image, createInfo );
    return ffxGetResourceVK( image, resDesc, const_cast< wchar_t* >( name ), state );
}

template< size_t N >
void InsertBarriers( VkCommandBuffer      cmd,
                     uint32_t             frameIndex,
                     RTGL1::Framebuffers& framebuffers,
                     const RTGL1::FramebufferImageIndex ( &inputsAndOutput )[ N ],
                     bool isBackwards )
{
    assert( std::find( std::begin( inputsAndOutput ),
                       std::end( inputsAndOutput ),
                       OUTPUT_IMAGE_INDEX ) != std::end( inputsAndOutput ) );

    VkImageMemoryBarrier2 barriers[ N ];

    for( size_t i = 0; i < N; i++ )
    {
        barriers[ i ] = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask       = inputsAndOutput[ i ] == OUTPUT_IMAGE_INDEX ? VK_ACCESS_2_SHADER_WRITE_BIT : VK_ACCESS_2_SHADER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout           = inputsAndOutput[ i ] == OUTPUT_IMAGE_INDEX ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
            std::swap( b.srcQueueFamilyIndex, b.dstQueueFamilyIndex );
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

RTGL1::FramebufferImageIndex RTGL1::FSR3::Apply(
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

    if( !isContextCreated )
    {
        OnFramebuffersSizeChange( renderResolution.GetResolutionState() );
    }

    using FI = FramebufferImageIndex;

    FI rs[] = {
        FI::FB_IMAGE_INDEX_FINAL,      FI::FB_IMAGE_INDEX_DEPTH_NDC, FI::FB_IMAGE_INDEX_MOTION_DLSS,
        FI::FB_IMAGE_INDEX_REACTIVITY, OUTPUT_IMAGE_INDEX,
    };
    InsertBarriers( cmd, frameIndex, *framebuffers, rs, false );

    // clang-format off
    FfxFsr3UpscalerDispatchDescription info = {
        .commandList                = ffxGetCommandListVK( cmd ),
        .color                      = ToFSR3Resource( FI::FB_IMAGE_INDEX_FINAL, frameIndex, *framebuffers, renderResolution.GetResolutionState(), FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_InputColor" ),
        .depth                      = ToFSR3Resource( FI::FB_IMAGE_INDEX_DEPTH_NDC, frameIndex, *framebuffers, renderResolution.GetResolutionState(), FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_InputDepth" ),
        .motionVectors              = ToFSR3Resource( FI::FB_IMAGE_INDEX_MOTION_DLSS, frameIndex, *framebuffers, renderResolution.GetResolutionState(), FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_InputMotionVectors" ),
        .exposure                   = {},
        .reactive                   = ToFSR3Resource( FI::FB_IMAGE_INDEX_REACTIVITY, frameIndex, *framebuffers, renderResolution.GetResolutionState(), FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_InputReactive" ),
        .transparencyAndComposition = {},
        .dilatedDepth               = {},
        .dilatedMotionVectors       = {},
        .reconstructedPrevNearestDepth = {},
        .output                     = ToFSR3Resource( OUTPUT_IMAGE_INDEX, frameIndex, *framebuffers, renderResolution.GetResolutionState(), FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"FSR3_OutputColor" ),
        .jitterOffset               = { -jitterOffset.data[ 0 ], -jitterOffset.data[ 1 ] },
        .motionVectorScale          = { float( renderResolution.GetResolutionState().renderWidth ), float( renderResolution.GetResolutionState().renderHeight ) },
        .renderSize                 = { renderResolution.GetResolutionState().renderWidth, renderResolution.GetResolutionState().renderHeight },
        .upscaleSize                = { renderResolution.GetResolutionState().upscaledWidth, renderResolution.GetResolutionState().upscaledHeight },
        .enableSharpening           = renderResolution.IsCASInsideFSR3(),
        .sharpness                  = renderResolution.GetSharpeningIntensity(),
        .frameTimeDelta             = float( timeDelta * 1000.0 ),
        .preExposure                = 1.0f,
        .reset                      = resetAccumulation,
        .cameraNear                 = nearPlane,
        .cameraFar                  = farPlane,
        .cameraFovAngleVertical     = fovVerticalRad,
        .viewSpaceToMetersFactor    = 1.0f,
        .flags                      = 0,
    };
    // clang-format on

    FfxErrorCode r = ffxFsr3UpscalerContextDispatch( context.get(), &info );
    CheckError( r );

    InsertBarriers( cmd, frameIndex, *framebuffers, rs, true );

    return OUTPUT_IMAGE_INDEX;
}

RgFloat2D RTGL1::FSR3::GetJitter( const ResolutionState& resolutionState, uint32_t frameId )
{
    const int32_t jitterPhaseCount = ffxFsr3UpscalerGetJitterPhaseCount(
        int32_t( resolutionState.renderWidth ), int32_t( resolutionState.upscaledWidth ) );

    RgFloat2D    jitter = {};
    FfxErrorCode r =
        ffxFsr3UpscalerGetJitterOffset( &jitter.data[ 0 ], &jitter.data[ 1 ], frameId, jitterPhaseCount );
    assert( r == FFX_OK );

    return jitter;
}

bool RTGL1::FSR3::IsFsr3Available() {
   return true;
}

#else

RTGL1::FSR3::FSR3(VkDevice _device, VkPhysicalDevice _physDevice)
        : device(_device)
        , physDevice(_physDevice) {}

RTGL1::FSR3::~FSR3() {}

void RTGL1::FSR3::OnFramebuffersSizeChange(const ResolutionState &resolutionState) {}

RTGL1::FramebufferImageIndex RTGL1::FSR3::Apply(
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

RgFloat2D RTGL1::FSR3::GetJitter(const RTGL1::ResolutionState &resolutionState, uint32_t frameId)
{
   return RgFloat2D{0, 0};
}

bool RTGL1::FSR3::IsFsr3Available()
{
   return false;
}

#endif
