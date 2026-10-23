#pragma once

#include <memory>
#include "Framebuffers.h"

namespace RTGL1
{

class GlobalUniform;

class IDenoiser : public IFramebuffersDependency
{
public:
    virtual void Denoise( VkCommandBuffer cmd,
                          uint32_t frameIndex,
                          const std::shared_ptr< const GlobalUniform >& uniform,
                          RgFloat2D jitter,
                          bool resetAccumulation ) = 0;
};

}
