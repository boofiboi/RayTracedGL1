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

#pragma once

#include "FrameGenerator.h"

#ifdef RG_USE_AMD_FSR3
    #include <atomic>

    #include <ffx_api/ffx_api.h>
    #include <ffx_api/vk/ffx_api_vk.h>
#endif

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
                        float                              worldScale );
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
#ifdef RG_USE_AMD_FSR3
    void CreateFgContext( uint32_t displayWidth, uint32_t displayHeight );
    void DestroyFgContext();
    void CreateHudlessImages( uint32_t width, uint32_t height );
    void DestroyHudlessImages();

    VkDevice                           device;
    VkPhysicalDevice                   physDevice;
    std::shared_ptr< MemoryAllocator > allocator;
    std::shared_ptr< Queues >          queues;
    VkFormat                           backBufferFormat;
    float                              worldScale{ 1.0f };

    ffxContext                         fgContext{ nullptr };
    ffxContext                         swapchainContext{ nullptr };

    PFN_vkCreateSwapchainFFXAPI        pfnCreateSwapchainFFX{ nullptr };
    PFN_vkDestroySwapchainFFXAPI       pfnDestroySwapchainFFX{ nullptr };
    PFN_vkGetSwapchainImagesKHR        pfnGetSwapchainImages{ nullptr };
    PFN_vkAcquireNextImageKHR          pfnAcquireNextImage{ nullptr };
    PFN_vkQueuePresentKHR              pfnQueuePresent{ nullptr };

    bool                               fgContextFailed{ false };
    bool                               swapchainContextFailed{ false };
    bool                               swapchainHandleAlive{ false };

    std::atomic< bool >                pendingReset{ false };

    VkImage                            hudlessImages[ MAX_FRAMES_IN_FLIGHT ]{};
    VkDeviceMemory                     hudlessMemories[ MAX_FRAMES_IN_FLIGHT ]{};
#endif
};

}
