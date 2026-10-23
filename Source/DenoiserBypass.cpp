#include "DenoiserBypass.h"

#include "Generated/ShaderCommonC.h"
#include "CmdLabel.h"
#include "Utils.h"

RTGL1::DenoiserBypass::DenoiserBypass( VkDevice _device,
                                      std::shared_ptr< Framebuffers > _framebuffers,
                                      const ShaderManager& _shaderManager,
                                      const GlobalUniform& _uniform )
    : device( _device )
    , framebuffers( std::move( _framebuffers ) )
    , pipelineLayout( VK_NULL_HANDLE )
    , prepassPipeline( VK_NULL_HANDLE )
    , postprocessPipeline( VK_NULL_HANDLE )
{
    VkDescriptorSetLayout setLayouts[] = {
        framebuffers->GetDescSetLayout(),
        _uniform.GetDescSetLayout(),
    };

    CreatePipelineLayout( setLayouts, static_cast< uint32_t >( std::size( setLayouts ) ) );
    CreatePipelines( &_shaderManager );
}

RTGL1::DenoiserBypass::~DenoiserBypass()
{
    vkDestroyPipelineLayout( device, pipelineLayout, nullptr );
    DestroyPipelines();
}

void RTGL1::DenoiserBypass::Denoise( VkCommandBuffer cmd,
                                     uint32_t frameIndex,
                                     const std::shared_ptr< const GlobalUniform >& uniform,
                                     RgFloat2D,
                                     bool )
{
    typedef FramebufferImageIndex FI;

    const uint32_t currentWidth  = static_cast< uint32_t >( uniform->GetData()->renderWidth );
    const uint32_t currentHeight = static_cast< uint32_t >( uniform->GetData()->renderHeight );

    if( currentWidth == 0 || currentHeight == 0 )
    {
        return;
    }

    {
        CmdLabel label( cmd, "Denoiser Bypass Prepass" );

        FI inBarriers[] = {
            FI::FB_IMAGE_INDEX_IS_SKY,
            FI::FB_IMAGE_INDEX_SURFACE_POSITION,
            FI::FB_IMAGE_INDEX_NORMAL,
            FI::FB_IMAGE_INDEX_METALLIC_ROUGHNESS,
            FI::FB_IMAGE_INDEX_VIEW_DIRECTION,
            FI::FB_IMAGE_INDEX_UNFILTERED_DIRECT,
            FI::FB_IMAGE_INDEX_UNFILTERED_INDIR,
            FI::FB_IMAGE_INDEX_UNFILTERED_SPECULAR,
            FI::FB_IMAGE_INDEX_NRD_DIFFUSE_HIT_DIST,
            FI::FB_IMAGE_INDEX_NRD_SPECULAR_HIT_DIST,
            FI::FB_IMAGE_INDEX_NRD_NORMAL_ROUGHNESS,
            FI::FB_IMAGE_INDEX_NRD_VIEW_Z,
        };
        framebuffers->BarrierMultiple( cmd, frameIndex, inBarriers );

        VkDescriptorSet sets[] = {
            framebuffers->GetDescSet( frameIndex ),
            uniform->GetDescSet( frameIndex ),
        };

        vkCmdBindDescriptorSets( cmd,
                                 VK_PIPELINE_BIND_POINT_COMPUTE,
                                 pipelineLayout,
                                 0,
                                 static_cast< uint32_t >( std::size( sets ) ),
                                 sets,
                                 0,
                                 nullptr );

        const uint32_t linearType = 1;

        vkCmdPushConstants( cmd,
                            pipelineLayout,
                            VK_SHADER_STAGE_COMPUTE_BIT,
                            0,
                            sizeof( uint32_t ),
                            &linearType );

        vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prepassPipeline );

        const uint32_t wgCountX = Utils::GetWorkGroupCount( currentWidth, COMPUTE_COMPOSE_GROUP_SIZE_X );
        const uint32_t wgCountY = Utils::GetWorkGroupCount( currentHeight, COMPUTE_COMPOSE_GROUP_SIZE_Y );
        vkCmdDispatch( cmd, wgCountX, wgCountY, 1 );
    }

    {
        CmdLabel label( cmd, "Denoiser Bypass Copy" );

        const ResolutionState resolutionState = {
            .renderWidth    = currentWidth,
            .renderHeight   = currentHeight,
            .upscaledWidth  = currentWidth,
            .upscaledHeight = currentHeight,
        };

        framebuffers->CopyImage( cmd,
                                 frameIndex,
                                 FI::FB_IMAGE_INDEX_NRD_DIFFUSE_HIT_DIST,
                                 FI::FB_IMAGE_INDEX_NRD_OUT_DIFFUSE,
                                 resolutionState );
        framebuffers->CopyImage( cmd,
                                 frameIndex,
                                 FI::FB_IMAGE_INDEX_NRD_SPECULAR_HIT_DIST,
                                 FI::FB_IMAGE_INDEX_NRD_OUT_SPECULAR,
                                 resolutionState );
    }

    {
        CmdLabel label( cmd, "Denoiser Bypass Postprocess" );

        FI outBarriers[] = {
            FI::FB_IMAGE_INDEX_NRD_OUT_DIFFUSE,
            FI::FB_IMAGE_INDEX_NRD_OUT_SPECULAR,
            FI::FB_IMAGE_INDEX_ALBEDO,
            FI::FB_IMAGE_INDEX_METALLIC_ROUGHNESS,
            FI::FB_IMAGE_INDEX_THROUGHPUT,
            FI::FB_IMAGE_INDEX_IS_SKY,
            FI::FB_IMAGE_INDEX_ACID_FOG_R_T,
            FI::FB_IMAGE_INDEX_PRE_FINAL,
            FI::FB_IMAGE_INDEX_HISTOGRAM_INPUT,
        };
        framebuffers->BarrierMultiple( cmd, frameIndex, outBarriers );

        VkDescriptorSet sets[] = {
            framebuffers->GetDescSet( frameIndex ),
            uniform->GetDescSet( frameIndex ),
        };

        vkCmdBindDescriptorSets( cmd,
                                 VK_PIPELINE_BIND_POINT_COMPUTE,
                                 pipelineLayout,
                                 0,
                                 static_cast< uint32_t >( std::size( sets ) ),
                                 sets,
                                 0,
                                 nullptr );

        const uint32_t linearType = 1;

        vkCmdPushConstants( cmd,
                            pipelineLayout,
                            VK_SHADER_STAGE_COMPUTE_BIT,
                            0,
                            sizeof( uint32_t ),
                            &linearType );

        vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, postprocessPipeline );

        const uint32_t wgCountX = Utils::GetWorkGroupCount( currentWidth, COMPUTE_COMPOSE_GROUP_SIZE_X );
        const uint32_t wgCountY = Utils::GetWorkGroupCount( currentHeight, COMPUTE_COMPOSE_GROUP_SIZE_Y );
        vkCmdDispatch( cmd, wgCountX, wgCountY, 1 );

        FI postBarriers[] = {
            FI::FB_IMAGE_INDEX_PRE_FINAL,
            FI::FB_IMAGE_INDEX_HISTOGRAM_INPUT,
        };
        framebuffers->BarrierMultiple( cmd, frameIndex, postBarriers );
    }
}

void RTGL1::DenoiserBypass::OnShaderReload( const ShaderManager* shaderManager )
{
    DestroyPipelines();
    CreatePipelines( shaderManager );
}

void RTGL1::DenoiserBypass::OnFramebuffersSizeChange( const ResolutionState& )
{
}

void RTGL1::DenoiserBypass::CreatePipelineLayout( VkDescriptorSetLayout* pSetLayouts,
                                                  uint32_t               setLayoutCount )
{
    VkPushConstantRange pushConst = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof( uint32_t ),
    };

    VkPipelineLayoutCreateInfo plLayoutInfo = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = setLayoutCount,
        .pSetLayouts            = pSetLayouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pushConst,
    };

    VkResult r = vkCreatePipelineLayout( device, &plLayoutInfo, nullptr, &pipelineLayout );
    VK_CHECKERROR( r );
    SET_DEBUG_NAME( device, pipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Denoiser Bypass pipeline layout" );
}

void RTGL1::DenoiserBypass::DestroyPipelines()
{
    if( prepassPipeline != VK_NULL_HANDLE )
    {
        vkDestroyPipeline( device, prepassPipeline, nullptr );
        prepassPipeline = VK_NULL_HANDLE;
    }
    if( postprocessPipeline != VK_NULL_HANDLE )
    {
        vkDestroyPipeline( device, postprocessPipeline, nullptr );
        postprocessPipeline = VK_NULL_HANDLE;
    }
}

void RTGL1::DenoiserBypass::CreatePipelines( const ShaderManager* shaderManager )
{
    {
        VkComputePipelineCreateInfo plInfo = {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = shaderManager->GetStageInfo( "CNrdPrepass" ),
            .layout = pipelineLayout,
        };

        VkResult r = vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &prepassPipeline );
        VK_CHECKERROR( r );
        SET_DEBUG_NAME( device, prepassPipeline, VK_OBJECT_TYPE_PIPELINE, "Denoiser Bypass prepass pipeline" );
    }

    {
        VkComputePipelineCreateInfo plInfo = {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = shaderManager->GetStageInfo( "CNrdPostprocess" ),
            .layout = pipelineLayout,
        };

        VkResult r = vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &postprocessPipeline );
        VK_CHECKERROR( r );
        SET_DEBUG_NAME( device, postprocessPipeline, VK_OBJECT_TYPE_PIPELINE, "Denoiser Bypass postprocess pipeline" );
    }
}
