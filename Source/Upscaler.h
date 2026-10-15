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

class IUpscaler : public IFramebuffersDependency
{
public:
    virtual FramebufferImageIndex Apply( VkCommandBuffer                        cmd,
                                         uint32_t                               frameIndex,
                                         const std::shared_ptr< Framebuffers >& framebuffers,
                                         const RenderResolutionHelper&          renderResolution,
                                         RgFloat2D                              jitterOffset,
                                         double                                 timeDelta,
                                         float                                  nearPlane,
                                         float                                  farPlane,
                                         float                                  fovVerticalRad,
                                         bool                                   resetAccumulation ) = 0;

    virtual RgFloat2D GetJitter( const ResolutionState& resolutionState, uint32_t frameId ) const = 0;

    virtual bool      IsAvailable() const = 0;
};

}
