#include "DenoiserManager.h"

RTGL1::DenoiserManager::DenoiserManager( VkInstance                      instance,
                                         VkDevice                        device,
                                         VkPhysicalDevice                physDevice,
                                         uint32_t                        graphicsQueueFamilyIndex,
                                         std::shared_ptr< Framebuffers > framebuffers,
                                         const ShaderManager&            shaderManager,
                                         const GlobalUniform&            uniform )
    : nrd( std::make_shared< DenoiserNRD >( instance,
                                            device,
                                            physDevice,
                                            graphicsQueueFamilyIndex,
                                            framebuffers,
                                            shaderManager,
                                            uniform ) )
    , bypass( std::make_shared< DenoiserBypass >( device, framebuffers, shaderManager, uniform ) )
{
}

RTGL1::DenoiserManager::~DenoiserManager() = default;

void RTGL1::DenoiserManager::Denoise( VkCommandBuffer                               cmd,
                                      uint32_t                                      frameIndex,
                                      const std::shared_ptr< const GlobalUniform >& uniform,
                                      RgRenderDenoiserTechnique                     technique,
                                      RgFloat2D                                     jitter,
                                      bool                                          resetAccumulation )
{
    if( technique != appliedTechnique )
    {
        appliedTechnique = technique;
        if( technique == RG_RENDER_DENOISER_TECHNIQUE_NRD_RELAX )
        {
            nrd->SetMethod( NrdDenoiserMethod::Relax );
        }
        else if( technique == RG_RENDER_DENOISER_TECHNIQUE_NRD_REBLUR )
        {
            nrd->SetMethod( NrdDenoiserMethod::Reblur );
        }
    }

    if( technique == RG_RENDER_DENOISER_TECHNIQUE_NONE )
    {
        bypass->Denoise( cmd, frameIndex, uniform, jitter, resetAccumulation );
    }
    else
    {
        nrd->Denoise( cmd, frameIndex, uniform, jitter, resetAccumulation );
    }
}

bool RTGL1::DenoiserManager::IsTechniqueAvailable( RgRenderDenoiserTechnique technique ) const
{
    switch( technique )
    {
        case RG_RENDER_DENOISER_TECHNIQUE_NONE: return true;
        case RG_RENDER_DENOISER_TECHNIQUE_NRD_REBLUR:
        case RG_RENDER_DENOISER_TECHNIQUE_NRD_RELAX:
#ifdef RG_USE_NVIDIA_NRD
            return true;
#else
            return false;
#endif
        case RG_RENDER_DENOISER_TECHNIQUE_NVIDIA_DLSS_RR: return false;
        default: return false;
    }
}

RTGL1::DenoiserNRD& RTGL1::DenoiserManager::GetNRD()
{
    return *nrd;
}

void RTGL1::DenoiserManager::OnShaderReload( const ShaderManager* shaderManager )
{
    nrd->OnShaderReload( shaderManager );
    bypass->OnShaderReload( shaderManager );
}

void RTGL1::DenoiserManager::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
    nrd->OnFramebuffersSizeChange( resolutionState );
    bypass->OnFramebuffersSizeChange( resolutionState );
}
