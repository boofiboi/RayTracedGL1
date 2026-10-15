// Copyright (c) 2021 Sultim Tsyrendashiev
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

#include <memory>

#include "Framebuffers.h"

namespace RTGL1
{

class RenderResolutionHelper;

// Vendor-specific swapchain hijack that some frame generators (AMD FSR3) require.
// Swapchain routes every swapchain create/destroy/acquire/present through these
// hooks when they are active. Implementations must be valid to call until the
// Swapchain object is destroyed.
class IFrameGeneratorSwapchain
{
public:
    virtual ~IFrameGeneratorSwapchain() = default;

    // If false, Swapchain must use the plain Vulkan path
    virtual bool IsActive() const = 0;

    // May be called on a live context (resize); implementation decides
    // whether to create its underlying context or reuse it
    virtual VkResult CreateSwapchain( const VkSwapchainCreateInfoKHR& createInfo,
                                      VkSwapchainKHR*                 outSwapchain ) = 0;
    virtual void     DestroySwapchain( VkSwapchainKHR swapchain )                    = 0;

    virtual VkResult AcquireNextImage( VkDevice     device,
                                       VkSwapchainKHR swapchain,
                                       uint64_t      timeout,
                                       VkSemaphore   semaphore,
                                       uint32_t*     outImageIndex )                  = 0;
    virtual VkResult QueuePresent( VkQueue queue, const VkPresentInfoKHR& presentInfo ) = 0;
    virtual VkResult GetSwapchainImages( VkDevice       device,
                                         VkSwapchainKHR swapchain,
                                         uint32_t*      pCount,
                                         VkImage*       pImages )                      = 0;

    // Block until all queued interpolated presents are done.
    // Must be called before destroying anything the pacing thread may reference.
    virtual void WaitForPresents() = 0;
};

class IFrameGenerator : public IFramebuffersDependency
{
public:
    virtual bool IsAvailable() const = 0;

    // Null, if the frame generator doesn't need to hijack the swapchain
    virtual IFrameGeneratorSwapchain* GetSwapchainHooks() = 0;

    virtual void PrepareFrame( VkCommandBuffer                        cmd,
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
                               uint32_t                               frameId ) = 0;

    virtual void CaptureHudless( VkCommandBuffer cmd,
                                 uint32_t        frameIndex,
                                 VkImage         srcImage,
                                 uint32_t        width,
                                 uint32_t        height ) = 0;

    virtual void ConfigureFrame( VkSwapchainKHR swapchain,
                                 uint32_t       frameIndex,
                                 uint32_t       width,
                                 uint32_t       height,
                                 uint32_t       frameId,
                                 bool           frameGenEnabled ) = 0;
};

}
