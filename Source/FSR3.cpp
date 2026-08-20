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
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/vk/ffx_api_vk.h>

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

RTGL1::FSR3::FSR3( VkDevice                                _device,
                    VkPhysicalDevice                        _physDevice,
                    std::shared_ptr< MemoryAllocator >      _allocator,
                    bool                                    _enableFrameGeneration )
    : device( _device )
    , physDevice( _physDevice )
    , allocator( std::move( _allocator ) )
    , enableFrameGeneration( _enableFrameGeneration )
    , context( std::make_unique< FfxFsr3UpscalerContext >() )
{
    memset( context.get(), 0, sizeof( FfxFsr3UpscalerContext ) );
}

RTGL1::FSR3::~FSR3()
{
    DestroyResources();

    if( isContextCreated && context )
    {
        ffxFsr3UpscalerContextDestroy( context.get() );
    }

    if( fgContext )
    {
        ffxDestroyContext( (ffxContext*)&fgContext, nullptr );
        fgContext = nullptr;
    }
}

void RTGL1::FSR3::DestroyResources()
{
    if( backendInterface.fpDestroyResource )
    {
        if( dilatedDepthInternal.internalIndex != 0 )
        {
            backendInterface.fpDestroyResource( &backendInterface, dilatedDepthInternal, 0 );
            dilatedDepthInternal = {};
            dilatedDepthRes = {};
        }
        if( dilatedMotionVectorsInternal.internalIndex != 0 )
        {
            backendInterface.fpDestroyResource( &backendInterface, dilatedMotionVectorsInternal, 0 );
            dilatedMotionVectorsInternal = {};
            dilatedMotionVectorsRes = {};
        }
        if( reconstructedPrevNearestDepthInternal.internalIndex != 0 )
        {
            backendInterface.fpDestroyResource( &backendInterface, reconstructedPrevNearestDepthInternal, 0 );
            reconstructedPrevNearestDepthInternal = {};
            reconstructedPrevNearestDepthRes = {};
        }
    }

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

void RTGL1::FSR3::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
    DestroyResources();

    if( isContextCreated && context )
    {
        ffxFsr3UpscalerContextDestroy( context.get() );
        memset( context.get(), 0, sizeof( FfxFsr3UpscalerContext ) );
        isContextCreated = false;
    }

    if( fgContext )
    {
        ffxDestroyContext( (ffxContext*)&fgContext, nullptr );
        fgContext = nullptr;
    }

    hudlessExtent = { resolutionState.upscaledWidth, resolutionState.upscaledHeight };

    if( enableFrameGeneration && allocator )
    {
        for( uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++ )
        {
            VkImageCreateInfo imageInfo = {
                .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType     = VK_IMAGE_TYPE_2D,
                .format        = hudlessFormat,
                .extent        = { hudlessExtent.width, hudlessExtent.height, 1 },
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

            hudlessMemories[ i ] = allocator->AllocDedicated(
                memReqs,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                MemoryAllocator::AllocType::DEFAULT,
                "FSR3 HUDLess Image" );

            r = vkBindImageMemory( device, hudlessImages[ i ], hudlessMemories[ i ], 0 );
            VK_CHECKERROR( r );

            VkImageViewCreateInfo viewInfo = {
                .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image            = hudlessImages[ i ],
                .viewType         = VK_IMAGE_VIEW_TYPE_2D,
                .format           = hudlessFormat,
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

    FfxFsr3UpscalerContextDescription contextDesc = {
        .flags         = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
                         FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE,
        .maxRenderSize = { resolutionState.upscaledWidth, resolutionState.upscaledHeight },
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

    backendInterface = contextDesc.backendInterface;

    r = ffxFsr3UpscalerContextCreate( context.get(), &contextDesc );
    CheckError( r );

    FfxFsr3UpscalerSharedResourceDescriptions sharedDescs = {};
    r = ffxFsr3UpscalerGetSharedResourceDescriptions( context.get(), &sharedDescs );
    CheckError( r );

    backendInterface.fpCreateResource( &backendInterface, &sharedDescs.dilatedDepth, 0, &dilatedDepthInternal );
    backendInterface.fpCreateResource( &backendInterface, &sharedDescs.dilatedMotionVectors, 0, &dilatedMotionVectorsInternal );
    backendInterface.fpCreateResource( &backendInterface, &sharedDescs.reconstructedPrevNearestDepth, 0, &reconstructedPrevNearestDepthInternal );

    dilatedDepthRes = backendInterface.fpGetResource( &backendInterface, dilatedDepthInternal );
    dilatedMotionVectorsRes = backendInterface.fpGetResource( &backendInterface, dilatedMotionVectorsInternal );
    reconstructedPrevNearestDepthRes = backendInterface.fpGetResource( &backendInterface, reconstructedPrevNearestDepthInternal );

    isContextCreated = true;

    if( enableFrameGeneration )
    {
        ffxCreateBackendVKDesc backendDesc = {};
        backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backendDesc.vkDevice = device;
        backendDesc.vkPhysicalDevice = physDevice;
        backendDesc.vkDeviceProcAddr = vkGetDeviceProcAddr;

        ffxCreateContextDescFrameGenerationHudless hudlessDesc = {};
        hudlessDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hudlessDesc.header.pNext = &backendDesc.header;
        hudlessDesc.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK( hudlessFormat );

        ffxCreateContextDescFrameGeneration createFg = {};
        createFg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        createFg.header.pNext = &hudlessDesc.header;
        createFg.displaySize = { resolutionState.upscaledWidth, resolutionState.upscaledHeight };
        createFg.maxRenderSize = { resolutionState.upscaledWidth, resolutionState.upscaledHeight };
        createFg.flags = 0;
        createFg.backBufferFormat = ffxApiGetSurfaceFormatVK( hudlessFormat );

        remove( "fsr3_error.txt" );
        ffxReturnCode_t ret = ffxCreateContext( (ffxContext*)&fgContext, &createFg.header, nullptr );
        if( ret != FFX_API_RETURN_OK )
        {
            char errBuf[512] = {};
            FILE* f = fopen( "fsr3_error.txt", "r" );
            if( f )
            {
                fgets( errBuf, sizeof( errBuf ), f );
                fclose( f );
            }
            if( errBuf[0] )
            {
                debug::Warning( "{}", errBuf );
            }
            debug::Warning( "FSR3: fgContext creation failed, ffxCreateContext returned {}", static_cast<int>( ret ) );
            fgContext = nullptr;
        }
        else
        {
            debug::Info( "FSR3: fgContext created successfully" );
        }
    }
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
    // Note: FB_IMAGE_INDEX_REACTIVITY is already included in the barrier above and passed to FSR3

    // clang-format off
    FfxFsr3UpscalerDispatchDescription info = {
        .commandList                = ffxGetCommandListVK( cmd ),
        .color                      = ToFSR3Resource( FI::FB_IMAGE_INDEX_FINAL, frameIndex, *framebuffers, renderResolution.GetResolutionState(), FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_InputColor" ),
        .depth                      = ToFSR3Resource( FI::FB_IMAGE_INDEX_DEPTH_NDC, frameIndex, *framebuffers, renderResolution.GetResolutionState(), FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_InputDepth" ),
        .motionVectors              = ToFSR3Resource( FI::FB_IMAGE_INDEX_MOTION_DLSS, frameIndex, *framebuffers, renderResolution.GetResolutionState(), FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_InputMotionVectors" ),
        .exposure                   = {},
        .reactive                   = ToFSR3Resource( FI::FB_IMAGE_INDEX_REACTIVITY, frameIndex, *framebuffers, renderResolution.GetResolutionState(), FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_InputReactivity" ),
        .transparencyAndComposition = {},
        .dilatedDepth               = dilatedDepthRes,
        .dilatedMotionVectors       = dilatedMotionVectorsRes,
        .reconstructedPrevNearestDepth = reconstructedPrevNearestDepthRes,
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

void RTGL1::FSR3::PrepareFrameGeneration( VkCommandBuffer                        cmd,
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
                                          uint64_t                               frameId )
{
    if( !enableFrameGeneration || !fgContext )
    {
        return;
    }

    using FI = FramebufferImageIndex;
    FI rs[] = { FI::FB_IMAGE_INDEX_DEPTH_NDC, FI::FB_IMAGE_INDEX_MOTION_DLSS };
    framebuffers->BarrierMultiple( cmd, frameIndex, rs, Framebuffers::BarrierType::Storage );

    auto [ depthImage, depthView, depthFormat, depthSz ] =
        framebuffers->GetImageHandles( FI::FB_IMAGE_INDEX_DEPTH_NDC, frameIndex, renderResolution.GetResolutionState() );
    auto [ motionImage, motionView, motionFormat, motionSz ] =
        framebuffers->GetImageHandles( FI::FB_IMAGE_INDEX_MOTION_DLSS, frameIndex, renderResolution.GetResolutionState() );

    VkImageCreateInfo depthInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = depthFormat,
        .extent = { depthSz.width, depthSz.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
    };
    VkImageCreateInfo motionInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = motionFormat,
        .extent = { motionSz.width, motionSz.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
    };

    FfxApiResource depthRes = ffxApiGetResourceVK( (void*)depthImage, ffxApiGetImageResourceDescriptionVK( depthImage, depthInfo, 0 ), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
    FfxApiResource motionRes = ffxApiGetResourceVK( (void*)motionImage, ffxApiGetImageResourceDescriptionVK( motionImage, motionInfo, 0 ), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );

    ffxDispatchDescFrameGenerationPrepare prepareDesc = {};
    prepareDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepareDesc.frameID = frameId;
    prepareDesc.flags = 0;
    prepareDesc.commandList = (void*)cmd;
    prepareDesc.renderSize = { renderResolution.GetResolutionState().renderWidth, renderResolution.GetResolutionState().renderHeight };
    prepareDesc.jitterOffset = { -jitterOffset.data[ 0 ], -jitterOffset.data[ 1 ] };
    prepareDesc.motionVectorScale = { float( renderResolution.GetResolutionState().renderWidth ), float( renderResolution.GetResolutionState().renderHeight ) };
    prepareDesc.frameTimeDelta = float( timeDelta * 1000.0 );
    prepareDesc.cameraNear = nearPlane;
    prepareDesc.cameraFar = farPlane;
    prepareDesc.cameraFovAngleVertical = fovVerticalRad;
    prepareDesc.viewSpaceToMetersFactor = 1.0f;
    prepareDesc.depth = depthRes;
    prepareDesc.motionVectors = motionRes;

    ffxDispatchDescFrameGenerationPrepareCameraInfo cameraInfo = {};
    if( pView != nullptr )
    {
        cameraInfo.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        cameraInfo.cameraRight[0] = pView[0];
        cameraInfo.cameraRight[1] = pView[4];
        cameraInfo.cameraRight[2] = pView[8];
        cameraInfo.cameraUp[0] = pView[1];
        cameraInfo.cameraUp[1] = pView[5];
        cameraInfo.cameraUp[2] = pView[9];
        cameraInfo.cameraForward[0] = -pView[2];
        cameraInfo.cameraForward[1] = -pView[6];
        cameraInfo.cameraForward[2] = -pView[10];
        prepareDesc.header.pNext = &cameraInfo.header;
    }

    ffxDispatch( (ffxContext*)&fgContext, &prepareDesc.header );
}

void RTGL1::FSR3::CaptureHudless( VkCommandBuffer cmd,
                                  uint32_t        frameIndex,
                                  VkImage         srcAccumImage,
                                  uint32_t        width,
                                  uint32_t        height )
{
    if( !enableFrameGeneration || !fgContext || hudlessImages[ frameIndex ] == VK_NULL_HANDLE )
    {
        return;
    }

    VkImageMemoryBarrier2KHR barriers[ 2 ] = {};

    barriers[ 0 ] = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
        .srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT_KHR | VK_ACCESS_2_MEMORY_READ_BIT_KHR,
        .dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR,
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
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
        .srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT_KHR | VK_ACCESS_2_MEMORY_READ_BIT_KHR,
        .dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT_KHR,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT_KHR,
        .oldLayout     = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image         = srcAccumImage,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };

    VkDependencyInfoKHR dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
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
                    srcAccumImage,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    hudlessImages[ frameIndex ],
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &blitRegion,
                    VK_FILTER_NEAREST );

    barriers[ 0 ].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT_KHR;
    barriers[ 0 ].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR;
    barriers[ 0 ].dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
    barriers[ 0 ].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT_KHR;
    barriers[ 0 ].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[ 0 ].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    barriers[ 1 ].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT_KHR;
    barriers[ 1 ].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT_KHR;
    barriers[ 1 ].dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
    barriers[ 1 ].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT_KHR | VK_ACCESS_2_MEMORY_WRITE_BIT_KHR;
    barriers[ 1 ].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[ 1 ].newLayout     = VK_IMAGE_LAYOUT_GENERAL;

    svkCmdPipelineBarrier2KHR( cmd, &dep );
}

void RTGL1::FSR3::ConfigureFrameGeneration( VkSwapchainKHR swapchain,
                                            uint32_t       frameIndex,
                                            uint32_t       width,
                                            uint32_t       height,
                                            uint64_t       frameId,
                                            bool           frameGenEnabled )
{
    if( !enableFrameGeneration || !fgContext )
    {
        return;
    }

    FfxApiResource hudlessRes = {};
    if( frameGenEnabled && hudlessImages[ frameIndex ] != VK_NULL_HANDLE )
    {
        VkImageCreateInfo hudlessInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = hudlessFormat,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        };
        hudlessRes = ffxApiGetResourceVK( (void*)hudlessImages[ frameIndex ], ffxApiGetImageResourceDescriptionVK( hudlessImages[ frameIndex ], hudlessInfo, 0 ), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ );
    }

    ffxConfigureDescFrameGeneration configDesc = {};
    configDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configDesc.swapChain = (void*)swapchain;
    configDesc.presentCallback = nullptr;
    configDesc.presentCallbackUserContext = nullptr;
    configDesc.frameGenerationCallback = []( ffxDispatchDescFrameGeneration* params, void* pUserCtx ) -> ffxReturnCode_t {
        return ffxDispatch( (ffxContext*)pUserCtx, &params->header );
    };
    configDesc.frameGenerationCallbackUserContext = &fgContext;
    configDesc.frameGenerationEnabled = frameGenEnabled;
    configDesc.allowAsyncWorkloads = true;
    configDesc.HUDLessColor = hudlessRes;
    configDesc.flags = 0;
    configDesc.onlyPresentGenerated = false;
    configDesc.generationRect = { 0, 0, (int32_t)width, (int32_t)height };
    configDesc.frameID = frameId;

    ffxConfigure( (ffxContext*)&fgContext, &configDesc.header );
}

bool RTGL1::FSR3::IsFrameGenerationAvailable() const
{
    return true;
}

#else

RTGL1::FSR3::FSR3(VkDevice _device, VkPhysicalDevice _physDevice, bool _enableFrameGeneration)
        : device(_device)
        , physDevice(_physDevice)
        , enableFrameGeneration(_enableFrameGeneration) {}

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

void RTGL1::FSR3::PrepareFrameGeneration( VkCommandBuffer                        cmd,
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
                                          uint64_t                               frameId )
{
}

void RTGL1::FSR3::ConfigureFrameGeneration( VkSwapchainKHR swapchain,
                                            VkImage        hudlessImage,
                                            VkFormat       hudlessFormat,
                                            uint32_t       width,
                                            uint32_t       height,
                                            uint64_t       frameId,
                                            bool           frameGenEnabled )
{
}

RgFloat2D RTGL1::FSR3::GetJitter(const RTGL1::ResolutionState &resolutionState, uint32_t frameId)
{
   return RgFloat2D{0, 0};
}

bool RTGL1::FSR3::IsFsr3Available()
{
   return false;
}

bool RTGL1::FSR3::IsFrameGenerationAvailable() const
{
   return false;
}

#endif
