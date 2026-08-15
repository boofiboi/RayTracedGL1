#include "Denoiser.h"

#include <cmath>
#include <cstring>
#include "Generated/ShaderCommonC.h"
#include "CmdLabel.h"
#include "Utils.h"

#ifdef RG_USE_NVIDIA_NRD
#include <NRDIntegration.hpp>
#endif

RTGL1::Denoiser::Denoiser( VkInstance _instance,
                           VkDevice _device,
                           VkPhysicalDevice _physDevice,
                           uint32_t _graphicsQueueFamilyIndex,
                           std::shared_ptr< Framebuffers > _framebuffers,
                           const ShaderManager& _shaderManager,
                           const GlobalUniform& _uniform )
    : instance( _instance )
    , device( _device )
    , physDevice( _physDevice )
    , queueFamilyIndex( _graphicsQueueFamilyIndex )
    , framebuffers( std::move( _framebuffers ) )
    , pipelineLayout( VK_NULL_HANDLE )
    , prepassPipeline( VK_NULL_HANDLE )
    , postprocessPipeline( VK_NULL_HANDLE )
    , renderWidth( 0 )
    , renderHeight( 0 )
    , prevJitter{}
    , currentMethod( NrdDenoiserMethod::Reblur )
#ifdef RG_USE_NVIDIA_NRD
    , reblurSettings{}
    , relaxSettings{}
    , nrdInitialized( false )
#endif
{
    VkDescriptorSetLayout setLayouts[] = {
        framebuffers->GetDescSetLayout(),
        _uniform.GetDescSetLayout(),
    };

    CreatePipelineLayout( setLayouts, static_cast< uint32_t >( std::size( setLayouts ) ) );
    CreatePipelines( &_shaderManager );
}

RTGL1::Denoiser::~Denoiser()
{
#ifdef RG_USE_NVIDIA_NRD
    if( nrdInitialized )
    {
        nrdIntegration.Destroy();
        nrdInitialized = false;
    }
#endif

    vkDestroyPipelineLayout( device, pipelineLayout, nullptr );
    DestroyPipelines();
}

void RTGL1::Denoiser::SetMethod( NrdDenoiserMethod method )
{
    currentMethod = method;
}

RTGL1::NrdDenoiserMethod RTGL1::Denoiser::GetMethod() const
{
    return currentMethod;
}

#ifdef RG_USE_NVIDIA_NRD
nrd::ReblurSettings& RTGL1::Denoiser::GetReblurSettings()
{
    return reblurSettings;
}

nrd::RelaxSettings& RTGL1::Denoiser::GetRelaxSettings()
{
    return relaxSettings;
}
#endif

void RTGL1::Denoiser::RecreateNrd( uint32_t width, uint32_t height )
{
#ifdef RG_USE_NVIDIA_NRD
    if( width == 0 || height == 0 )
    {
        return;
    }

    renderWidth = width;
    renderHeight = height;

    nrd::IntegrationCreationDesc integrationDesc = {};
    strcpy_s( integrationDesc.name, "RayTracedGL1_NRD" );
    integrationDesc.resourceWidth = static_cast< uint16_t >( width );
    integrationDesc.resourceHeight = static_cast< uint16_t >( height );
    integrationDesc.queuedFrameNum = FRAMEBUFFERS_HISTORY_LENGTH;

    const nrd::DenoiserDesc denoiserDescs[] = {
        { 0, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR },
        { 1, nrd::Denoiser::RELAX_DIFFUSE_SPECULAR },
    };

    nrd::InstanceCreationDesc instanceDesc = {};
    instanceDesc.denoisers = denoiserDescs;
    instanceDesc.denoisersNum = static_cast< uint32_t >( std::size( denoiserDescs ) );

    nri::QueueFamilyVKDesc queueFamily = {
        .queueNum = 1,
        .queueType = nri::QueueType::GRAPHICS,
        .familyIndex = queueFamilyIndex,
    };

    nri::DeviceCreationVKDesc devDesc = {
        .vkInstance = ( void* )instance,
        .vkDevice = ( void* )device,
        .vkPhysicalDevice = ( void* )physDevice,
        .queueFamilies = &queueFamily,
        .queueFamilyNum = 1,
        .minorVersion = 2,
    };

    nrd::Result res = nrdIntegration.RecreateVK( integrationDesc, instanceDesc, devDesc );
    if( res == nrd::Result::SUCCESS )
    {
        nrdInitialized = true;
    }
    else
    {
        nrdInitialized = false;
    }
#endif
}

void RTGL1::Denoiser::Denoise( VkCommandBuffer cmd,
                               uint32_t frameIndex,
                               const std::shared_ptr< const GlobalUniform >& uniform,
                               RgFloat2D jitter,
                               bool resetAccumulation )
{
    typedef FramebufferImageIndex FI;

    uint32_t currentWidth = static_cast< uint32_t >( uniform->GetData()->renderWidth );
    uint32_t currentHeight = static_cast< uint32_t >( uniform->GetData()->renderHeight );

#ifdef RG_USE_NVIDIA_NRD
    if( currentWidth != renderWidth || currentHeight != renderHeight || !nrdInitialized )
    {
        RecreateNrd( currentWidth, currentHeight );
    }

    if( !nrdInitialized )
    {
        return;
    }

    uint32_t denoiserType = ( currentMethod == NrdDenoiserMethod::Reblur ) ? 0 : 1;

    {
        CmdLabel label( cmd, "NRD Prepass" );

        FI inBarriers[] = {
            FI::FB_IMAGE_INDEX_IS_SKY,
            FI::FB_IMAGE_INDEX_SURFACE_POSITION,
            FI::FB_IMAGE_INDEX_NORMAL,
            FI::FB_IMAGE_INDEX_METALLIC_ROUGHNESS,
            FI::FB_IMAGE_INDEX_VIEW_DIRECTION,
            FI::FB_IMAGE_INDEX_UNFILTERED_DIRECT,
            FI::FB_IMAGE_INDEX_UNFILTERED_INDIR,
            FI::FB_IMAGE_INDEX_UNFILTERED_SPECULAR,
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

        vkCmdPushConstants( cmd,
                            pipelineLayout,
                            VK_SHADER_STAGE_COMPUTE_BIT,
                            0,
                            sizeof( uint32_t ),
                            &denoiserType );

        vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prepassPipeline );

        uint32_t wgCountX = Utils::GetWorkGroupCount( currentWidth, COMPUTE_COMPOSE_GROUP_SIZE_X );
        uint32_t wgCountY = Utils::GetWorkGroupCount( currentHeight, COMPUTE_COMPOSE_GROUP_SIZE_Y );
        vkCmdDispatch( cmd, wgCountX, wgCountY, 1 );
    }

    {
        CmdLabel label( cmd, "NRD Denoiser" );

        FI nrdInBarriers[] = {
            FI::FB_IMAGE_INDEX_NRD_DIFFUSE_HIT_DIST,
            FI::FB_IMAGE_INDEX_NRD_SPECULAR_HIT_DIST,
            FI::FB_IMAGE_INDEX_NRD_NORMAL_ROUGHNESS,
            FI::FB_IMAGE_INDEX_NRD_VIEW_Z,
            FI::FB_IMAGE_INDEX_MOTION,
        };
        framebuffers->BarrierMultiple( cmd, frameIndex, nrdInBarriers );

        nrdIntegration.NewFrame();

        nrd::CommonSettings commonSettings = {};
        memcpy( commonSettings.viewToClipMatrix, uniform->GetData()->projection, sizeof( float ) * 16 );
        memcpy( commonSettings.viewToClipMatrixPrev, uniform->GetData()->projectionPrev, sizeof( float ) * 16 );
        memcpy( commonSettings.worldToViewMatrix, uniform->GetData()->view, sizeof( float ) * 16 );
        memcpy( commonSettings.worldToViewMatrixPrev, uniform->GetData()->viewPrev, sizeof( float ) * 16 );

        commonSettings.cameraJitter[ 0 ] = jitter.data[ 0 ];
        commonSettings.cameraJitter[ 1 ] = jitter.data[ 1 ];
        commonSettings.cameraJitterPrev[ 0 ] = prevJitter.data[ 0 ];
        commonSettings.cameraJitterPrev[ 1 ] = prevJitter.data[ 1 ];

        commonSettings.motionVectorScale[ 0 ] = 1.0f;
        commonSettings.motionVectorScale[ 1 ] = 1.0f;
        commonSettings.motionVectorScale[ 2 ] = 0.0f;
        commonSettings.isMotionVectorInWorldSpace = false;

        commonSettings.frameIndex = uniform->GetData()->frameId;
        commonSettings.accumulationMode =
            resetAccumulation ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE;

        nrdIntegration.SetCommonSettings( commonSettings );

        if( currentMethod == NrdDenoiserMethod::Reblur )
        {
            nrdIntegration.SetDenoiserSettings( 0, &reblurSettings );
        }
        else
        {
            nrdIntegration.SetDenoiserSettings( 1, &relaxSettings );
        }

        nrd::ResourceSnapshot snapshot = {};

        auto setupResource = [ & ]( nrd::ResourceType type, FramebufferImageIndex fbIndex ) {
            auto [ img, view, fmt ] = framebuffers->GetImageHandles( fbIndex, frameIndex );
            nrd::Resource r = {};
            r.vk.image = ( uint64_t )img;
            r.vk.format = ( int32_t )fmt;
            r.state.layout = nri::Layout::GENERAL;
            r.state.stages = nri::StageBits::COMPUTE_SHADER;
            r.state.access = nri::AccessBits::SHADER_RESOURCE_STORAGE;
            snapshot.SetResource( type, r );
        };

        setupResource( nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, FI::FB_IMAGE_INDEX_NRD_DIFFUSE_HIT_DIST );
        setupResource( nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, FI::FB_IMAGE_INDEX_NRD_SPECULAR_HIT_DIST );
        setupResource( nrd::ResourceType::IN_NORMAL_ROUGHNESS, FI::FB_IMAGE_INDEX_NRD_NORMAL_ROUGHNESS );
        setupResource( nrd::ResourceType::IN_VIEWZ, FI::FB_IMAGE_INDEX_NRD_VIEW_Z );
        setupResource( nrd::ResourceType::IN_MV, FI::FB_IMAGE_INDEX_MOTION );
        setupResource( nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, FI::FB_IMAGE_INDEX_NRD_OUT_DIFFUSE );
        setupResource( nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, FI::FB_IMAGE_INDEX_NRD_OUT_SPECULAR );

        snapshot.restoreInitialState = true;

        nri::CommandBufferVKDesc cbDesc = {
            .vkCommandBuffer = ( void* )cmd,
            .queueType = nri::QueueType::GRAPHICS,
        };

        nrd::Identifier denoiserId = ( currentMethod == NrdDenoiserMethod::Reblur ) ? 0 : 1;
        nrdIntegration.DenoiseVK( &denoiserId, 1, cbDesc, snapshot );
    }

    {
        CmdLabel label( cmd, "NRD Postprocess" );

        FI outBarriers[] = {
            FI::FB_IMAGE_INDEX_NRD_OUT_DIFFUSE,
            FI::FB_IMAGE_INDEX_NRD_OUT_SPECULAR,
            FI::FB_IMAGE_INDEX_ALBEDO,
            FI::FB_IMAGE_INDEX_METALLIC_ROUGHNESS,
            FI::FB_IMAGE_INDEX_THROUGHPUT,
            FI::FB_IMAGE_INDEX_IS_SKY,
            FI::FB_IMAGE_INDEX_ACID_FOG_R_T,
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

        vkCmdPushConstants( cmd,
                            pipelineLayout,
                            VK_SHADER_STAGE_COMPUTE_BIT,
                            0,
                            sizeof( uint32_t ),
                            &denoiserType );

        vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, postprocessPipeline );

        uint32_t wgCountX = Utils::GetWorkGroupCount( currentWidth, COMPUTE_COMPOSE_GROUP_SIZE_X );
        uint32_t wgCountY = Utils::GetWorkGroupCount( currentHeight, COMPUTE_COMPOSE_GROUP_SIZE_Y );
        vkCmdDispatch( cmd, wgCountX, wgCountY, 1 );

        FI postBarriers[] = {
            FI::FB_IMAGE_INDEX_PRE_FINAL,
            FI::FB_IMAGE_INDEX_HISTOGRAM_INPUT,
        };
        framebuffers->BarrierMultiple( cmd, frameIndex, postBarriers );
    }

    prevJitter = jitter;
#endif
}

void RTGL1::Denoiser::OnShaderReload( const ShaderManager* shaderManager )
{
    DestroyPipelines();
    CreatePipelines( shaderManager );
}

void RTGL1::Denoiser::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
    RecreateNrd( resolutionState.renderWidth, resolutionState.renderHeight );
}

void RTGL1::Denoiser::CreatePipelineLayout( VkDescriptorSetLayout* pSetLayouts, uint32_t setLayoutCount )
{
    VkPushConstantRange pushConst = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof( uint32_t ),
    };

    VkPipelineLayoutCreateInfo plLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = setLayoutCount,
        .pSetLayouts = pSetLayouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConst,
    };

    VkResult r = vkCreatePipelineLayout( device, &plLayoutInfo, nullptr, &pipelineLayout );
    VK_CHECKERROR( r );
    SET_DEBUG_NAME( device, pipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "NRD Denoiser pipeline layout" );
}

void RTGL1::Denoiser::DestroyPipelines()
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

void RTGL1::Denoiser::CreatePipelines( const ShaderManager* shaderManager )
{
    {
        VkComputePipelineCreateInfo plInfo = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = shaderManager->GetStageInfo( "CNrdPrepass" ),
            .layout = pipelineLayout,
        };

        VkResult r = vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &prepassPipeline );
        VK_CHECKERROR( r );
        SET_DEBUG_NAME( device, prepassPipeline, VK_OBJECT_TYPE_PIPELINE, "NRD Prepass pipeline" );
    }

    {
        VkComputePipelineCreateInfo plInfo = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = shaderManager->GetStageInfo( "CNrdPostprocess" ),
            .layout = pipelineLayout,
        };

        VkResult r = vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &postprocessPipeline );
        VK_CHECKERROR( r );
        SET_DEBUG_NAME( device, postprocessPipeline, VK_OBJECT_TYPE_PIPELINE, "NRD Postprocess pipeline" );
    }
}
