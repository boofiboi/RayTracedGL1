#pragma once

#include <memory>
#include "IDenoiser.h"
#include "ShaderManager.h"
#include "Framebuffers.h"
#include "GlobalUniform.h"

namespace RTGL1
{

class DenoiserBypass final : public IDenoiser, public IShaderDependency
{
public:
    DenoiserBypass( VkDevice device,
                    std::shared_ptr< Framebuffers > framebuffers,
                    const ShaderManager& shaderManager,
                    const GlobalUniform& uniform );
    ~DenoiserBypass() override;

    DenoiserBypass( const DenoiserBypass& other ) = delete;
    DenoiserBypass( DenoiserBypass&& other ) noexcept = delete;
    DenoiserBypass& operator=( const DenoiserBypass& other ) = delete;
    DenoiserBypass& operator=( DenoiserBypass&& other ) noexcept = delete;

    void Denoise( VkCommandBuffer cmd,
                  uint32_t frameIndex,
                  const std::shared_ptr< const GlobalUniform >& uniform,
                  RgFloat2D jitter,
                  bool resetAccumulation ) override;

    void OnShaderReload( const ShaderManager* shaderManager ) override;
    void OnFramebuffersSizeChange( const ResolutionState& resolutionState ) override;

private:
    void CreatePipelineLayout( VkDescriptorSetLayout* pSetLayouts, uint32_t setLayoutCount );
    void CreatePipelines( const ShaderManager* shaderManager );
    void DestroyPipelines();

private:
    VkDevice device;

    std::shared_ptr< Framebuffers > framebuffers;

    VkPipelineLayout pipelineLayout;
    VkPipeline prepassPipeline;
    VkPipeline postprocessPipeline;
};

}
