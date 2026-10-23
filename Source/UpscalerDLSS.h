#pragma once

#include <memory>
#include "Upscaler.h"

struct NVSDK_NGX_Handle;

namespace RTGL1
{

class NgxManager;
class RenderResolutionHelper;

class UpscalerDLSS : public IUpscaler
{
public:
    UpscalerDLSS( VkDevice                      device,
                  std::shared_ptr< NgxManager > ngxManager );
    ~UpscalerDLSS() override;

    UpscalerDLSS( const UpscalerDLSS& other )                = delete;
    UpscalerDLSS( UpscalerDLSS&& other ) noexcept            = delete;
    UpscalerDLSS& operator=( const UpscalerDLSS& other )     = delete;
    UpscalerDLSS& operator=( UpscalerDLSS&& other ) noexcept = delete;

    FramebufferImageIndex Apply( VkCommandBuffer                        cmd,
                                 uint32_t                               frameIndex,
                                 const std::shared_ptr< Framebuffers >& framebuffers,
                                 const RenderResolutionHelper&          renderResolution,
                                 RgFloat2D                              jitterOffset,
                                 double                                 timeDelta,
                                 float                                  nearPlane,
                                 float                                  farPlane,
                                 float                                  fovVerticalRad,
                                 bool                                   resetAccumulation ) override;

    RgFloat2D GetJitter( const ResolutionState& resolutionState, uint32_t frameId ) const override;

    bool      IsAvailable() const override;

    void      OnFramebuffersSizeChange( const ResolutionState& resolutionState ) override;

    void GetOptimalSettings( uint32_t               userWidth,
                             uint32_t               userHeight,
                             RgRenderResolutionMode mode,
                             uint32_t*              pOutWidth,
                             uint32_t*              pOutHeight ) const;

private:
    void DestroyDlssFeature();
    bool AreSameDlssFeatureValues( const RenderResolutionHelper& renderResolution ) const;
    void SaveDlssFeatureValues( const RenderResolutionHelper& renderResolution );
    bool ValidateDlssFeature( VkCommandBuffer cmd, const RenderResolutionHelper& renderResolution );

private:
    VkDevice                      device;
    std::shared_ptr< NgxManager > ngxManager;
    NVSDK_NGX_Handle*             pDlssFeature;

    struct PrevDlssFeatureValues
    {
        uint32_t renderWidth;
        uint32_t renderHeight;
        uint32_t upscaledWidth;
        uint32_t upscaledHeight;
    } prevDlssFeatureValues;
};

}
