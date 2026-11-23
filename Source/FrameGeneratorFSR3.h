#pragma once

#include "FrameGenerator.h"

namespace RTGL1
{

class MemoryAllocator;
class Queues;

class FrameGeneratorFSR3 : public IFrameGenerator, public IFrameGeneratorSwapchain
{
public:
    FrameGeneratorFSR3( VkDevice                           device,
                        VkPhysicalDevice                   physDevice,
                        std::shared_ptr< MemoryAllocator > allocator,
                        std::shared_ptr< Queues >          queues,
                        VkFormat                           backBufferFormat,
                        float                              worldScale = 1.0f );
    ~FrameGeneratorFSR3() override;

    FrameGeneratorFSR3( const FrameGeneratorFSR3& other )                = delete;
    FrameGeneratorFSR3( FrameGeneratorFSR3&& other ) noexcept            = delete;
    FrameGeneratorFSR3& operator=( const FrameGeneratorFSR3& other )     = delete;
    FrameGeneratorFSR3& operator=( FrameGeneratorFSR3&& other ) noexcept = delete;

    bool                      IsAvailable() const override;
    IFrameGeneratorSwapchain* GetSwapchainHooks() override;

    void OnFramebuffersSizeChange( const ResolutionState& resolutionState ) override;

    void PrepareFrame( VkCommandBuffer                        cmd,
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
                       uint32_t                               frameId ) override;

    void CaptureHudless( VkCommandBuffer cmd,
                         uint32_t        frameIndex,
                         VkImage         srcImage,
                         uint32_t        width,
                         uint32_t        height ) override;

    void SetDlss( std::shared_ptr< class DLSS > dlss ) override;

    void ConfigureFrame( VkSwapchainKHR swapchain,
                         uint32_t       frameIndex,
                         uint32_t       width,
                         uint32_t       height,
                         uint32_t       frameId,
                         bool           frameGenEnabled,
                         bool           useDlssG = false ) override;

    bool     IsActive() const override;
    VkResult CreateSwapchain( const VkSwapchainCreateInfoKHR& createInfo,
                              VkSwapchainKHR*                 outSwapchain ) override;
    void     DestroySwapchain( VkSwapchainKHR swapchain ) override;
    VkResult AcquireNextImage( VkDevice       device,
                               VkSwapchainKHR swapchain,
                               uint64_t       timeout,
                               VkSemaphore    semaphore,
                               uint32_t*      outImageIndex ) override;
    VkResult QueuePresent( VkQueue queue, const VkPresentInfoKHR& presentInfo ) override;
    VkResult GetSwapchainImages( VkDevice       device,
                                 VkSwapchainKHR swapchain,
                                 uint32_t*      pCount,
                                 VkImage*       pImages ) override;
    void     WaitForPresents() override;

private:
    struct Impl;
    std::unique_ptr< Impl > impl;
};

}
