#include "UpscalerDLSS.h"

#include <cassert>
#include "RTGL1/RTGL1.h"
#include "CmdLabel.h"
#include "HaltonSequence.h"
#include "RenderResolutionHelper.h"
#include "NgxManager.h"
#include "DebugPrint.h"
#include "RgException.h"

#ifdef RG_USE_NVIDIA_DLSS

#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers.h>

static NVSDK_NGX_PerfQuality_Value ToNGXPerfQuality( RgRenderResolutionMode mode )
{
    switch( mode )
    {
        case RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        case RG_RENDER_RESOLUTION_MODE_PERFORMANCE:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case RG_RENDER_RESOLUTION_MODE_BALANCED:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_Balanced;
        case RG_RENDER_RESOLUTION_MODE_QUALITY:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case RG_RENDER_RESOLUTION_MODE_ULTRA_QUALITY:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_UltraQuality;
        default:
            assert( 0 );
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_Balanced;
    }
}

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

RTGL1::UpscalerDLSS::UpscalerDLSS( VkDevice                      _device,
                                   std::shared_ptr< NgxManager > _ngxManager )
    : device( _device )
    , ngxManager( std::move( _ngxManager ) )
    , pDlssFeature( nullptr )
    , prevDlssFeatureValues{}
{
}

RTGL1::UpscalerDLSS::~UpscalerDLSS()
{
    DestroyDlssFeature();
}

void RTGL1::UpscalerDLSS::DestroyDlssFeature()
{
    if( pDlssFeature != nullptr )
    {
        vkDeviceWaitIdle( device );

        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_ReleaseFeature( pDlssFeature );
        pDlssFeature       = nullptr;

        if( NVSDK_NGX_FAILED( r ) )
        {
            debug::Warning( "DLSS: NVSDK_NGX_VULKAN_ReleaseFeature fail: {}", static_cast< int >( r ) );
        }
    }
}

bool RTGL1::UpscalerDLSS::AreSameDlssFeatureValues( const RenderResolutionHelper& renderResolution ) const
{
    return prevDlssFeatureValues.renderWidth == renderResolution.Width() &&
           prevDlssFeatureValues.renderHeight == renderResolution.Height() &&
           prevDlssFeatureValues.upscaledWidth == renderResolution.UpscaledWidth() &&
           prevDlssFeatureValues.upscaledHeight == renderResolution.UpscaledHeight();
}

void RTGL1::UpscalerDLSS::SaveDlssFeatureValues( const RenderResolutionHelper& renderResolution )
{
    prevDlssFeatureValues = {
        .renderWidth    = renderResolution.Width(),
        .renderHeight   = renderResolution.Height(),
        .upscaledWidth  = renderResolution.UpscaledWidth(),
        .upscaledHeight = renderResolution.UpscaledHeight(),
    };
}

bool RTGL1::UpscalerDLSS::ValidateDlssFeature( VkCommandBuffer               cmd,
                                              const RenderResolutionHelper& renderResolution )
{
    if( !ngxManager || !ngxManager->IsInitialized() )
    {
        return false;
    }

    if( AreSameDlssFeatureValues( renderResolution ) )
    {
        return true;
    }
    SaveDlssFeatureValues( renderResolution );

    if( pDlssFeature != nullptr )
    {
        DestroyDlssFeature();
    }

    NVSDK_NGX_DLSS_Create_Params dlssParams = {
        .Feature = { .InWidth        = renderResolution.Width(),
                     .InHeight       = renderResolution.Height(),
                     .InTargetWidth  = renderResolution.UpscaledWidth(),
                     .InTargetHeight = renderResolution.UpscaledHeight() },
    };

    int& dlssCreateFeatureFlags = dlssParams.InFeatureCreateFlags;
    dlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

    uint32_t creationNodeMask   = 1;
    uint32_t visibilityNodeMask = 1;

    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(
        device, cmd, creationNodeMask, visibilityNodeMask, &pDlssFeature, ngxManager->GetParameters(), &dlssParams );
    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS: NGX_VULKAN_CREATE_DLSS_EXT1 fail: {}", static_cast< int >( r ) );

        pDlssFeature = nullptr;
        return false;
    }

    return true;
}

RTGL1::FramebufferImageIndex RTGL1::UpscalerDLSS::Apply(
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
    if( !IsAvailable() )
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT,
                           "Nvidia DLSS is not supported (or DLSS dynamic library files are not "
                           "found). Check availability before usage." );
    }

    ValidateDlssFeature( cmd, renderResolution );

    if( pDlssFeature == nullptr )
    {
        throw RgException(
            RG_RESULT_GRAPHICS_API_ERROR,
            "Internal error of Nvidia DLSS: NGX_VULKAN_CREATE_DLSS_EXT1 has failed." );
    }

    using FI             = FramebufferImageIndex;
    const FI outputImage = FI::FB_IMAGE_INDEX_UPSCALED_PONG;

    CmdLabel label( cmd, "DLSS" );

    FI fs[] = {
        FI::FB_IMAGE_INDEX_FINAL,
        FI::FB_IMAGE_INDEX_MOTION_DLSS,
        FI::FB_IMAGE_INDEX_DEPTH_NDC,
    };
    framebuffers->BarrierMultiple( cmd, frameIndex, fs, Framebuffers::BarrierType::Storage );

    NVSDK_NGX_Coordinates sourceOffset = { 0, 0 };
    NVSDK_NGX_Dimensions  sourceSize   = {
        renderResolution.Width(),
        renderResolution.Height(),
    };
    NVSDK_NGX_Dimensions targetSize = {
        renderResolution.UpscaledWidth(),
        renderResolution.UpscaledHeight(),
    };

    NVSDK_NGX_Resource_VK unresolvedColorResource = ToNGXResource( framebuffers, frameIndex, FI::FB_IMAGE_INDEX_FINAL, sourceSize );
    NVSDK_NGX_Resource_VK resolvedColorResource   = ToNGXResource( framebuffers, frameIndex, outputImage, targetSize, true );
    NVSDK_NGX_Resource_VK motionVectorsResource   = ToNGXResource( framebuffers, frameIndex, FI::FB_IMAGE_INDEX_MOTION_DLSS, sourceSize );
    NVSDK_NGX_Resource_VK depthResource           = ToNGXResource( framebuffers, frameIndex, FI::FB_IMAGE_INDEX_DEPTH_NDC, sourceSize );
    NVSDK_NGX_Resource_VK rayLengthResource       = ToNGXResource( framebuffers, frameIndex, FI::FB_IMAGE_INDEX_DEPTH_WORLD, sourceSize );

    NVSDK_NGX_VK_DLSS_Eval_Params evalParams = {
        .Feature  = { .pInColor = &unresolvedColorResource, .pInOutput = &resolvedColorResource },
        .pInDepth = &depthResource,
        .pInMotionVectors          = &motionVectorsResource,
        .InJitterOffsetX           = jitterOffset.data[ 0 ] * ( -1 ),
        .InJitterOffsetY           = jitterOffset.data[ 1 ] * ( -1 ),
        .InRenderSubrectDimensions = sourceSize,
        .InReset                   = resetAccumulation ? 1 : 0,
        .InMVScaleX                = float( sourceSize.Width ),
        .InMVScaleY                = float( sourceSize.Height ),
        .InColorSubrectBase        = sourceOffset,
        .InDepthSubrectBase        = sourceOffset,
        .InMVSubrectBase           = sourceOffset,
        .InTranslucencySubrectBase = sourceOffset,
        .InFrameTimeDeltaInMsec    = float( timeDelta * 1000.0 ),
        .pInRayTracingHitDistance  = &rayLengthResource,
    };

    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT( cmd, pDlssFeature, ngxManager->GetParameters(), &evalParams );

    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS: NGX_VULKAN_EVALUATE_DLSS_EXT fail: {}", static_cast< int >( r ) );
    }

    return outputImage;
}

void RTGL1::UpscalerDLSS::GetOptimalSettings( uint32_t               userWidth,
                                              uint32_t               userHeight,
                                              RgRenderResolutionMode mode,
                                              uint32_t*              pOutWidth,
                                              uint32_t*              pOutHeight ) const
{
    *pOutWidth  = userWidth;
    *pOutHeight = userHeight;

    if( !ngxManager || !ngxManager->IsInitialized() )
    {
        return;
    }

    NVSDK_NGX_Parameter* pParams = ngxManager->GetParameters();
    if( pParams == nullptr )
    {
        return;
    }

    uint32_t minWidth, minHeight, maxWidth, maxHeight;
    float    dummy;

    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS( pParams,
                                                        userWidth,
                                                        userHeight,
                                                        ToNGXPerfQuality( mode ),
                                                        pOutWidth,
                                                        pOutHeight,
                                                        &maxWidth,
                                                        &maxHeight,
                                                        &minWidth,
                                                        &minHeight,
                                                        &dummy );
    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS: NGX_DLSS_GET_OPTIMAL_SETTINGS fail: {}", static_cast< int >( r ) );
    }
}

#else

RTGL1::UpscalerDLSS::UpscalerDLSS( VkDevice                      _device,
                                   std::shared_ptr< NgxManager > _ngxManager )
    : device( _device )
    , ngxManager( std::move( _ngxManager ) )
    , pDlssFeature( nullptr )
    , prevDlssFeatureValues{}
{
}

RTGL1::UpscalerDLSS::~UpscalerDLSS() = default;

void RTGL1::UpscalerDLSS::DestroyDlssFeature() {}

bool RTGL1::UpscalerDLSS::AreSameDlssFeatureValues( const RenderResolutionHelper& renderResolution ) const
{
    return false;
}

void RTGL1::UpscalerDLSS::SaveDlssFeatureValues( const RenderResolutionHelper& renderResolution ) {}

bool RTGL1::UpscalerDLSS::ValidateDlssFeature( VkCommandBuffer               cmd,
                                              const RenderResolutionHelper& renderResolution )
{
    return false;
}

RTGL1::FramebufferImageIndex RTGL1::UpscalerDLSS::Apply(
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
    throw RgException(
        RG_RESULT_WRONG_FUNCTION_ARGUMENT,
        "RTGL1 was built without DLSS support. Enable RG_WITH_NVIDIA_DLSS CMake option." );
}

void RTGL1::UpscalerDLSS::GetOptimalSettings( uint32_t               userWidth,
                                              uint32_t               userHeight,
                                              RgRenderResolutionMode mode,
                                              uint32_t*              pOutWidth,
                                              uint32_t*              pOutHeight ) const
{
    throw RgException(
        RG_RESULT_WRONG_FUNCTION_ARGUMENT,
        "RTGL1 was built without DLSS support. Enable RG_WITH_NVIDIA_DLSS CMake option." );
}

#endif

RgFloat2D RTGL1::UpscalerDLSS::GetJitter( const ResolutionState& resolutionState, uint32_t frameId ) const
{
    return HaltonSequence::GetJitter_Halton23( frameId );
}

bool RTGL1::UpscalerDLSS::IsAvailable() const
{
    return ngxManager && ngxManager->IsSuperSamplingAvailable();
}

void RTGL1::UpscalerDLSS::OnFramebuffersSizeChange( const ResolutionState& resolutionState ) {}
