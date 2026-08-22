// Copyright (c) 2020-2021 Sultim Tsyrendashiev
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

#include <list>
#include <vector>

#include "Common.h"
#include "CommandBufferManager.h"
#include "ISwapchainDependency.h"

namespace RTGL1
{

class Queues;

class Swapchain
{
public:
    Swapchain( VkDevice                                device,
               VkSurfaceKHR                            surface,
               VkPhysicalDevice                        physDevice,
               std::shared_ptr< CommandBufferManager > cmdManager,
               std::shared_ptr< Queues >               queues                = nullptr,
               bool                                    enableFrameGeneration = false );
    ~Swapchain();

    Swapchain( const Swapchain& other )     = delete;
    Swapchain( Swapchain&& other ) noexcept = delete;
    Swapchain& operator=( const Swapchain& other ) = delete;
    Swapchain& operator=( Swapchain&& other ) noexcept = delete;

    bool       RequestVsync( bool enable );

    void       AcquireImage( VkSemaphore imageAvailableSemaphore );
    VkResult   Present( VkQueue queue, const VkPresentInfoKHR* pPresentInfo );
    void       BlitForPresent( VkCommandBuffer cmd,
                               VkImage         srcImage,
                               uint32_t        srcImageWidth,
                               uint32_t        srcImageHeight,
                               VkFilter        filter,
                               VkImageLayout   srcImageLayout = VK_IMAGE_LAYOUT_GENERAL );
    void       BlitPreviousForPresent( VkCommandBuffer cmd );
    void       OnQueuePresent( VkResult queuePresentResult );

    void Subscribe( std::shared_ptr< ISwapchainDependency > subscriber );

    VkFormat           GetSurfaceFormat() const;
    uint32_t           GetWidth() const;
    uint32_t           GetHeight() const;
    uint32_t           GetCurrentImageIndex() const;
    uint32_t           GetImageCount() const;
    VkImageView        GetImageView( uint32_t index ) const;
    VkImage            GetImage( uint32_t index ) const;
    const VkImageView* GetImageViews() const;
    VkSwapchainKHR     GetHandle() const;

    bool               IsExtentOptimal() const;
    bool               IsFrameGenerationEnabled() const;

private:
    VkExtent2D     GetOptimalExtent() const;

    // vkGetPhysicalDeviceSurfaceCapabilitiesKHR is an expensive synchronous
    // query, its result must be cached between frames and refreshed only
    // when the presentation engine reports that the surface has changed
    const VkSurfaceCapabilitiesKHR& GetCapabilities() const;
    void                            InvalidateCapabilities() const;

    bool           TryRecreate( const VkExtent2D& newExtent, bool vsync );

    void           Create( uint32_t       newWidth,
                           uint32_t       newHeight,
                           bool           vsync,
                           VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE );
    void           Destroy();
    VkSwapchainKHR DestroyWithoutSwapchain();

    void           CallCreateSubscribers();
    void           CallDestroySubscribers();

private:
    VkDevice                                           device;
    VkSurfaceKHR                                       surface;
    VkPhysicalDevice                                   physDevice;
    std::shared_ptr< CommandBufferManager >            cmdManager;
    std::shared_ptr< Queues >                          queues;
    bool                                               enableFrameGeneration;

    VkSurfaceFormatKHR                                 surfaceFormat;
    VkPresentModeKHR                                   presentModeVsync;
    VkPresentModeKHR                                   presentModeImmediate;

    bool                                               requestedVsync;
    VkExtent2D                                         surfaceExtent;
    bool                                               isVsync;

    VkSwapchainKHR                                     swapchain;
    std::vector< VkImage >                             swapchainImages;
    std::vector< VkImageView >                         swapchainViews;

    uint32_t                                           currentSwapchainIndex;

    mutable VkSurfaceCapabilitiesKHR                   cachedCapabilities;
    mutable bool                                       capabilitiesDirty;

    void*                                              fgSwapchainContext;
    void*                                              pfnCreateSwapchainFFX;
    void*                                              pfnDestroySwapchainFFX;
    void*                                              pfnGetSwapchainImagesKHR;
    void*                                              pfnAcquireNextImageKHR;
    void*                                              pfnQueuePresentKHR;

    std::list< std::weak_ptr< ISwapchainDependency > > subscribers;
};

}