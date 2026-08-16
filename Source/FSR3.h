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

#include <memory>
#include <vector>

#include "Framebuffers.h"

#include <FidelityFX/host/ffx_interface.h>

struct FfxFsr3UpscalerContext;

namespace RTGL1
{
class RenderResolutionHelper;

class FSR3 : public IFramebuffersDependency
{
public:
    FSR3( VkDevice device, VkPhysicalDevice physDevice, bool enableFrameGeneration );
    ~FSR3() override;

    FSR3( const FSR3& other )                = delete;
    FSR3( FSR3&& other ) noexcept            = delete;
    FSR3& operator=( const FSR3& other )     = delete;
    FSR3& operator=( FSR3&& other ) noexcept = delete;

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

    void PrepareFrameGeneration( VkCommandBuffer                        cmd,
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
                                 uint64_t                               frameId );

    void ConfigureFrameGeneration( VkSwapchainKHR swapchain,
                                   VkImage        hudlessImage,
                                   VkFormat       hudlessFormat,
                                   uint32_t       width,
                                   uint32_t       height,
                                   uint64_t       frameId );

    static RgFloat2D GetJitter( const ResolutionState& resolutionState, uint32_t frameId );

    static bool IsFsr3Available();
    bool        IsFrameGenerationAvailable() const;
private:
    void DestroyResources();

    VkDevice               device;
    VkPhysicalDevice       physDevice;
    bool                   enableFrameGeneration;

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
    void*                                     fgContext{ nullptr };
};
}
