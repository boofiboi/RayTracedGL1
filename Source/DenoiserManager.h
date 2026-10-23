#pragma once

#include <memory>
#include "IDenoiser.h"
#include "DenoiserNRD.h"
#include "DenoiserBypass.h"
#include "ShaderManager.h"
#include "Framebuffers.h"
#include "GlobalUniform.h"

namespace RTGL1
{

class DenoiserManager final : public IFramebuffersDependency, public IShaderDependency
{
public:
    DenoiserManager( VkInstance                             instance,
                     VkDevice                               device,
                     VkPhysicalDevice                       physDevice,
                     uint32_t                               graphicsQueueFamilyIndex,
                     std::shared_ptr< Framebuffers >        framebuffers,
                     const ShaderManager&                   shaderManager,
                     const GlobalUniform&                   uniform );
    ~DenoiserManager() override;

    DenoiserManager( const DenoiserManager& other ) = delete;
    DenoiserManager( DenoiserManager&& other ) noexcept = delete;
    DenoiserManager& operator=( const DenoiserManager& other ) = delete;
    DenoiserManager& operator=( DenoiserManager&& other ) noexcept = delete;

    void Denoise( VkCommandBuffer                               cmd,
                  uint32_t                                      frameIndex,
                  const std::shared_ptr< const GlobalUniform >& uniform,
                  RgRenderDenoiserTechnique                     technique,
                  RgFloat2D                                     jitter,
                  bool                                          resetAccumulation );

    bool IsTechniqueAvailable( RgRenderDenoiserTechnique technique ) const;

    DenoiserNRD& GetNRD();

    void OnShaderReload( const ShaderManager* shaderManager ) override;
    void OnFramebuffersSizeChange( const ResolutionState& resolutionState ) override;

private:
    std::shared_ptr< DenoiserNRD >    nrd;
    std::shared_ptr< DenoiserBypass > bypass;
};

}
