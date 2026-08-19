#pragma once

#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

#include "Framebuffers.h"
#include "Dx12Interop.h"

namespace RTGL1
{
class RenderResolutionHelper;

class FSR4 : public IFramebuffersDependency
{
public:
    FSR4( VkInstance instance, VkDevice device, std::shared_ptr< PhysicalDevice > physDevice, bool enableFrameGeneration );
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

    VkInstance                          instance;
    VkDevice                            device;
    std::shared_ptr< PhysicalDevice >   physDevice;
    bool                                enableFrameGeneration;
    bool                                isSupported;

#ifdef RG_USE_AMD_FSR4
    std::unique_ptr< Dx12Interop > dx12;
    HMODULE                        ffxModule{ nullptr };
    void*                          context{ nullptr };
    bool                           isContextCreated{ false };
#endif
};
}
