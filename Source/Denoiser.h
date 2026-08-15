#pragma once

#include <memory>
#include "ShaderManager.h"
#include "Framebuffers.h"
#include "GlobalUniform.h"
#include "IFramebuffersDependency.h"

#ifdef RG_USE_NVIDIA_NRD
#include <NRI.h>
#include <Extensions/NRIRayTracing.h>
#include <Extensions/NRIHelper.h>
#include <Extensions/NRIWrapperVK.h>
#include <NRD.h>
#include <NRDIntegration.h>
#endif

namespace RTGL1
{

enum class NrdDenoiserMethod
{
    Reblur,
    Relax
};

class Denoiser final : public IShaderDependency, public IFramebuffersDependency
{
public:
    Denoiser( VkInstance instance,
              VkDevice device,
              VkPhysicalDevice physDevice,
              uint32_t graphicsQueueFamilyIndex,
              std::shared_ptr< Framebuffers > framebuffers,
              const ShaderManager& shaderManager,
              const GlobalUniform& uniform );
    ~Denoiser() override;

    Denoiser( const Denoiser& other ) = delete;
    Denoiser( Denoiser&& other ) noexcept = delete;
    Denoiser& operator=( const Denoiser& other ) = delete;
    Denoiser& operator=( Denoiser&& other ) noexcept = delete;

    void Denoise( VkCommandBuffer cmd,
                  uint32_t frameIndex,
                  const std::shared_ptr< const GlobalUniform >& uniform,
                  RgFloat2D jitter,
                  bool resetAccumulation );

    void OnShaderReload( const ShaderManager* shaderManager ) override;
    void OnFramebuffersSizeChange( const ResolutionState& resolutionState ) override;

    void SetMethod( NrdDenoiserMethod method );
    NrdDenoiserMethod GetMethod() const;

#ifdef RG_USE_NVIDIA_NRD
    nrd::ReblurSettings& GetReblurSettings();
    nrd::RelaxSettings& GetRelaxSettings();
#endif

private:
    void CreatePipelineLayout( VkDescriptorSetLayout* pSetLayouts, uint32_t setLayoutCount );
    void CreatePipelines( const ShaderManager* shaderManager );
    void DestroyPipelines();
    void RecreateNrd( uint32_t width, uint32_t height );

private:
    VkInstance instance;
    VkDevice device;
    VkPhysicalDevice physDevice;
    uint32_t queueFamilyIndex;

    std::shared_ptr< Framebuffers > framebuffers;

    VkPipelineLayout pipelineLayout;
    VkPipeline prepassPipeline;
    VkPipeline postprocessPipeline;

    uint32_t renderWidth;
    uint32_t renderHeight;
    RgFloat2D prevJitter;

    NrdDenoiserMethod currentMethod;

#ifdef RG_USE_NVIDIA_NRD
    nrd::Integration nrdIntegration;
    nrd::ReblurSettings reblurSettings;
    nrd::RelaxSettings relaxSettings;
    bool nrdInitialized;
#endif
};

}
