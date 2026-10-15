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

#include "Upscaler.h"

#ifdef RG_USE_AMD_FSR3
    #include <vector>
    #include <FidelityFX/host/ffx_interface.h>
#endif

struct FfxFsr3UpscalerContext;

namespace RTGL1
{

class UpscalerFSR3 : public IUpscaler
{
public:
    UpscalerFSR3( VkDevice device, VkPhysicalDevice physDevice );
    ~UpscalerFSR3() override;

    UpscalerFSR3( const UpscalerFSR3& other )                = delete;
    UpscalerFSR3( UpscalerFSR3&& other ) noexcept            = delete;
    UpscalerFSR3& operator=( const UpscalerFSR3& other )     = delete;
    UpscalerFSR3& operator=( UpscalerFSR3&& other ) noexcept = delete;

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

    bool      IsAvailable() const override;

private:
#ifdef RG_USE_AMD_FSR3
    void DestroyContextAndResources();

    VkDevice                              device;
    VkPhysicalDevice                      physDevice;

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
#endif
};

}
