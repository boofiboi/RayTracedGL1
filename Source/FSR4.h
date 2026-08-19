#pragma once

#include <memory>
#include <vector>

#include "Framebuffers.h"

#include <FidelityFX/host/ffx_interface.h>

struct FfxFsr3UpscalerContext;

namespace RTGL1
{
class RenderResolutionHelper;

class FSR4 : public IFramebuffersDependency
{
public:
    FSR4( VkDevice device, VkPhysicalDevice physDevice, bool enableFrameGeneration );
    ~FSR4() override;

    FSR4( const FSR4& other )                = delete;
    FSR4( FSR4&& other ) noexcept            = delete;
    FSR4& operator=( const FSR4& other )     = delete;
    FSR4& operator=( FSR4&& other ) noexcept = delete;

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
                                 bool                                   resetAccumulation );

    static RgFloat2D GetJitter( const ResolutionState& resolutionState, uint32_t frameId );

    static bool IsHardwareSupported( VkPhysicalDevice physDevice );
    bool        IsFsr4Available() const;

private:
    void DestroyResources();

    VkDevice               device;
    VkPhysicalDevice       physDevice;
    bool                   enableFrameGeneration;
    bool                   isSupported;

    std::unique_ptr< FfxFsr3UpscalerContext > context;
    std::vector< uint8_t >                    scratchBuffer;
    FfxInterface                              backendInterface{};
    FfxResourceInternal                       dilatedDepthInternal{};
    FfxResourceInternal                       dilatedMotionVectorsInternal{};
    FfxResourceInternal                       reconstructedPrevNearestDepthInternal{};
    FfxResource                               dilatedDepthRes{};
    FfxResource                               dilatedMotionVectorsRes{};
    FfxResource                               reconstructedPrevNearestDepthRes{};
    bool                                      isContextCreated{ false };
};
}
