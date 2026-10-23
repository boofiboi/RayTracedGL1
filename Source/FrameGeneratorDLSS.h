#pragma once

#include <vector>

#include "FrameGenerator.h"

#ifdef RG_USE_NVIDIA_DLSS
#include "NgxManager.h"
#endif

struct NVSDK_NGX_Handle;

namespace RTGL1
{

class MemoryAllocator;
class Queues;

class FrameGeneratorDLSS final : public IFrameGenerator, public IFrameGeneratorSwapchain
{
public:
    FrameGeneratorDLSS( VkDevice                           device,
                        VkPhysicalDevice                   physDevice,
                        std::shared_ptr< MemoryAllocator > allocator,
                        std::shared_ptr< Queues >          queues,
                        std::shared_ptr< NgxManager >      ngxManager,
                        VkFormat                           backBufferFormat );
    ~FrameGeneratorDLSS() override;

    FrameGeneratorDLSS( const FrameGeneratorDLSS& other )                = delete;
    FrameGeneratorDLSS( FrameGeneratorDLSS&& other ) noexcept            = delete;
    FrameGeneratorDLSS& operator=( const FrameGeneratorDLSS& other )     = delete;
    FrameGeneratorDLSS& operator=( FrameGeneratorDLSS&& other ) noexcept = delete;

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

    void ConfigureFrame( VkSwapchainKHR swapchain,
                         uint32_t       frameIndex,
                         uint32_t       width,
                         uint32_t       height,
                         uint32_t       frameId,
                         bool           frameGenEnabled ) override;

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
#ifdef RG_USE_NVIDIA_DLSS
    bool ValidateDlssgFeature( VkCommandBuffer cmd, uint32_t width, uint32_t height );
    void DestroyDlssgFeature();
    void CreateHudlessImages( uint32_t width, uint32_t height );
    void DestroyHudlessImages();
    void CreateInterpImages( uint32_t width, uint32_t height );
    void DestroyInterpImages();

private:
    VkDevice                           device;
    VkPhysicalDevice                   physDevice;
    std::shared_ptr< MemoryAllocator > allocator;
    std::shared_ptr< Queues >          queues;
    std::shared_ptr< NgxManager >      ngxManager;
    VkFormat                           backBufferFormat;

    NVSDK_NGX_Handle*                  pDlssgFeature;
    bool                               featureFailed;

    VkImage                            hudlessImages[ MAX_FRAMES_IN_FLIGHT ];
    VkImageView                        hudlessViews[ MAX_FRAMES_IN_FLIGHT ];
    VkDeviceMemory                     hudlessMemories[ MAX_FRAMES_IN_FLIGHT ];

    VkImage                            interpImages[ MAX_FRAMES_IN_FLIGHT ];
    VkImageView                        interpViews[ MAX_FRAMES_IN_FLIGHT ];
    VkDeviceMemory                     interpMemories[ MAX_FRAMES_IN_FLIGHT ];

    uint32_t                           featureWidth;
    uint32_t                           featureHeight;
    uint32_t                           displayWidth;
    uint32_t                           displayHeight;

    bool                               isFrameGenEnabled;
    bool                               pendingReset;

    VkCommandPool                      blitCommandPool;
    VkCommandBuffer                    blitCmd[ MAX_FRAMES_IN_FLIGHT ];
    VkSemaphore                        interpAcquireSemaphores[ MAX_FRAMES_IN_FLIGHT ];
    VkSemaphore                        interpRenderFinishedSemaphores[ MAX_FRAMES_IN_FLIGHT ];
    VkSemaphore                        realRenderFinishedSemaphores[ MAX_FRAMES_IN_FLIGHT ];
    VkFence                            blitFences[ MAX_FRAMES_IN_FLIGHT ];
    std::vector< VkImage >             swapchainImages;
    VkSwapchainKHR                     currentSwapchainHandle;
    uint32_t                           currentFrameIndex;
    bool                               swapchainHandleAlive;
#endif
};

}
