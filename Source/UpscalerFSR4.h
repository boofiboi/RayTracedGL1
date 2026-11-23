#pragma once

#include <memory>

#include "Upscaler.h"

namespace RTGL1
{

class UpscalerFSR4 : public IUpscaler
{
public:
    UpscalerFSR4( VkDevice device, VkPhysicalDevice physDevice, float worldScale = 1.0f );
    ~UpscalerFSR4() override;

    UpscalerFSR4( const UpscalerFSR4& other )                = delete;
    UpscalerFSR4( UpscalerFSR4&& other ) noexcept            = delete;
    UpscalerFSR4& operator=( const UpscalerFSR4& other )     = delete;
    UpscalerFSR4& operator=( UpscalerFSR4&& other ) noexcept = delete;

    void OnFramebuffersSizeChange( const ResolutionState& resolutionState ) override;

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

    bool IsAvailable() const override;

private:
    struct FfxState;

    void DestroyContext();

    VkDevice                    device;
    VkPhysicalDevice            physDevice;
    float                       viewSpaceToMeters{ 1.0f };
    std::unique_ptr< FfxState > ffxState;
    bool                        loaderAvailable{ false };
    bool                        isContextCreated{ false };
    bool                        contextFailed{ false };
};

}
