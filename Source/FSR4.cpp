#include "FSR4.h"

#ifdef RG_USE_AMD_FSR4

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/dx12/ffx_api_dx12.h>

#include "RenderResolutionHelper.h"
#include "RgException.h"

#include <cstring>
#include <cmath>

namespace
{
static ffxFunctions ffxFuncs{};
static constexpr RTGL1::FramebufferImageIndex OUTPUT_IMAGE_INDEX =
    RTGL1::FramebufferImageIndex::FB_IMAGE_INDEX_UPSCALED_PONG;
}

bool RTGL1::FSR4::IsHardwareSupported( VkPhysicalDevice physDevice )
{
    if( physDevice == VK_NULL_HANDLE )
    {
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties( physDevice, &props );

    if( props.vendorID != 0x1002 )
    {
        return false;
    }

    if( props.deviceID >= 0x7440 )
    {
        return true;
    }

    const char* name = props.deviceName;
    if( name != nullptr )
    {
        if( strstr( name, "RX 7" ) || strstr( name, "RX 8" ) || strstr( name, "RX 9" ) ||
            strstr( name, "Radeon 7" ) || strstr( name, "Radeon 8" ) || strstr( name, "Radeon 9" ) ||
            strstr( name, "RDNA3" ) || strstr( name, "RDNA4" ) || strstr( name, "RDNA 3" ) || strstr( name, "RDNA 4" ) ||
            strstr( name, "Navi 3" ) || strstr( name, "Navi 4" ) || strstr( name, "Navi3" ) || strstr( name, "Navi4" ) ||
            strstr( name, "gfx11" ) || strstr( name, "gfx12" ) )
        {
            return true;
        }
    }

    return false;
}

bool RTGL1::FSR4::IsFsr4Available() const
{
    return isSupported;
}

RTGL1::FSR4::FSR4( VkInstance _instance, VkDevice _device, VkPhysicalDevice _physDevice, bool _enableFrameGeneration )
    : instance( _instance )
    , device( _device )
    , physDevice( _physDevice )
    , enableFrameGeneration( _enableFrameGeneration )
    , isSupported( IsHardwareSupported( _physDevice ) )
{
    if( isSupported )
    {
        dx12 = std::make_unique< Dx12Interop >();
        if( !dx12->Init( instance, physDevice, device ) )
        {
            isSupported = false;
        }
        else
        {
            ffxModule = LoadLibraryA( "amd_fidelityfx_loader_dx12.dll" );
            if( !ffxModule )
            {
                ffxModule = LoadLibraryA( "rt_bin/amd_fidelityfx_loader_dx12.dll" );
            }

            if( ffxModule )
            {
                ffxLoadFunctions( &ffxFuncs, ffxModule );
                if( !ffxFuncs.CreateContext || !ffxFuncs.Dispatch || !ffxFuncs.DestroyContext )
                {
                    isSupported = false;
                }
            }
            else
            {
                isSupported = false;
            }
        }
    }
}

RTGL1::FSR4::~FSR4()
{
    DestroyResources();

    if( dx12 )
    {
        dx12->Destroy();
        dx12.reset();
    }

    if( ffxModule )
    {
        FreeLibrary( ffxModule );
        ffxModule = nullptr;
    }
}

void RTGL1::FSR4::DestroyResources()
{
    if( isContextCreated && context && ffxFuncs.DestroyContext )
    {
        ffxContext ctx = ( ffxContext )context;
        ffxFuncs.DestroyContext( &ctx, nullptr );
        context = nullptr;
        isContextCreated = false;
    }
}

void RTGL1::FSR4::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
    if( !isSupported || !dx12 || !ffxFuncs.CreateContext )
    {
        return;
    }

    DestroyResources();

    ffxCreateBackendDX12Desc backendDesc = {
        .header = { .type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 },
        .device = dx12->GetDevice(),
    };

    ffxCreateContextDescUpscale createDesc = {
        .header = {
            .type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
            .pNext = &backendDesc.header,
        },
        .flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE,
        .maxRenderSize = { resolutionState.renderWidth, resolutionState.renderHeight },
        .maxUpscaleSize = { resolutionState.upscaledWidth, resolutionState.upscaledHeight },
        .fpMessage = nullptr,
    };

    ffxContext ctx = nullptr;
    ffxReturnCode_t r = ffxFuncs.CreateContext( &ctx, &createDesc.header, nullptr );
    if( r == FFX_API_RETURN_OK && ctx != nullptr )
    {
        context = ( void* )ctx;
        isContextCreated = true;
    }
    else
    {
        isContextCreated = false;
    }
}

RTGL1::FramebufferImageIndex RTGL1::FSR4::Apply(
    VkCommandBuffer                        cmd,
    uint32_t                               frameIndex,
    const std::shared_ptr< Framebuffers >& framebuffers,
    const RenderResolutionHelper&          renderResolution,
    RgFloat2D                              jitterOffset,
    double                                 timeDelta,
    float                                  nearPlane,
    float                                  farPlane,
    float                                  fovVerticalRad,
    bool                                   resetAccumulation )
{
    if( !isSupported || !dx12 || !ffxFuncs.Dispatch )
    {
        return OUTPUT_IMAGE_INDEX;
    }

    if( !isContextCreated || context == nullptr )
    {
        OnFramebuffersSizeChange( renderResolution.GetResolutionState() );
        if( !isContextCreated || context == nullptr )
        {
            return OUTPUT_IMAGE_INDEX;
        }
    }

    using FI = FramebufferImageIndex;
    FI rs[] = {
        FI::FB_IMAGE_INDEX_FINAL,
        FI::FB_IMAGE_INDEX_DEPTH_NDC,
        FI::FB_IMAGE_INDEX_MOTION_DLSS,
        FI::FB_IMAGE_INDEX_REACTIVITY,
        OUTPUT_IMAGE_INDEX,
    };
    for( auto fb : rs )
    {
        framebuffers->BarrierOne( cmd, frameIndex, fb, Framebuffers::BarrierType::Storage );
    }

    HANDLE colorHandle  = framebuffers->GetWin32MemoryHandle( FI::FB_IMAGE_INDEX_FINAL, frameIndex );
    HANDLE depthHandle  = framebuffers->GetWin32MemoryHandle( FI::FB_IMAGE_INDEX_DEPTH_NDC, frameIndex );
    HANDLE motionHandle = framebuffers->GetWin32MemoryHandle( FI::FB_IMAGE_INDEX_MOTION_DLSS, frameIndex );
    HANDLE outputHandle = framebuffers->GetWin32MemoryHandle( OUTPUT_IMAGE_INDEX, frameIndex );

    if( colorHandle == NULL || depthHandle == NULL || motionHandle == NULL || outputHandle == NULL )
    {
        return OUTPUT_IMAGE_INDEX;
    }

    uint32_t rW = renderResolution.GetResolutionState().renderWidth;
    uint32_t rH = renderResolution.GetResolutionState().renderHeight;
    uint32_t uW = renderResolution.GetResolutionState().upscaledWidth;
    uint32_t uH = renderResolution.GetResolutionState().upscaledHeight;

    D3D12_RESOURCE_DESC colorDesc = {
        .Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment        = 0,
        .Width            = rW,
        .Height           = rH,
        .DepthOrArraySize = 1,
        .MipLevels        = 1,
        .Format           = DXGI_FORMAT_R11G11B10_FLOAT,
        .SampleDesc       = { .Count = 1, .Quality = 0 },
        .Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    };
    D3D12_RESOURCE_DESC depthDesc = {
        .Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment        = 0,
        .Width            = rW,
        .Height           = rH,
        .DepthOrArraySize = 1,
        .MipLevels        = 1,
        .Format           = DXGI_FORMAT_R32_FLOAT,
        .SampleDesc       = { .Count = 1, .Quality = 0 },
        .Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags            = D3D12_RESOURCE_FLAG_NONE,
    };
    D3D12_RESOURCE_DESC motionDesc = {
        .Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment        = 0,
        .Width            = rW,
        .Height           = rH,
        .DepthOrArraySize = 1,
        .MipLevels        = 1,
        .Format           = DXGI_FORMAT_R16G16_FLOAT,
        .SampleDesc       = { .Count = 1, .Quality = 0 },
        .Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags            = D3D12_RESOURCE_FLAG_NONE,
    };
    D3D12_RESOURCE_DESC outputDesc = {
        .Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment        = 0,
        .Width            = uW,
        .Height           = uH,
        .DepthOrArraySize = 1,
        .MipLevels        = 1,
        .Format           = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .SampleDesc       = { .Count = 1, .Quality = 0 },
        .Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    };

    auto colorRes  = dx12->ImportPlacedResource( colorHandle, colorDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
    auto depthRes  = dx12->ImportPlacedResource( depthHandle, depthDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
    auto motionRes = dx12->ImportPlacedResource( motionHandle, motionDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE );
    auto outputRes = dx12->ImportPlacedResource( outputHandle, outputDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );

    if( !colorRes || !depthRes || !motionRes || !outputRes )
    {
        return OUTPUT_IMAGE_INDEX;
    }

    dx12->BeginCommands();
    ID3D12GraphicsCommandList* cl = dx12->GetCommandList();

    ffxDispatchDescUpscale dispatchDesc = {
        .header                  = { .type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE },
        .commandList             = cl,
        .color                   = ffxApiGetResourceDX12( colorRes.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ ),
        .depth                   = ffxApiGetResourceDX12( depthRes.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ ),
        .motionVectors           = ffxApiGetResourceDX12( motionRes.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ ),
        .exposure                = {},
        .reactive                = {},
        .transparencyAndComposition = {},
        .output                  = ffxApiGetResourceDX12( outputRes.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS ),
        .jitterOffset            = { -jitterOffset.data[ 0 ], -jitterOffset.data[ 1 ] },
        .motionVectorScale       = { float( rW ), float( rH ) },
        .renderSize              = { rW, rH },
        .upscaleSize             = { uW, uH },
        .enableSharpening        = false,
        .sharpness               = 0.0f,
        .frameTimeDelta          = float( timeDelta * 1000.0 ),
        .preExposure             = 1.0f,
        .reset                   = resetAccumulation,
        .cameraNear              = nearPlane,
        .cameraFar               = farPlane,
        .cameraFovAngleVertical  = fovVerticalRad,
        .viewSpaceToMetersFactor = 1.0f,
        .flags                   = 0,
    };

    ffxContext ctx = ( ffxContext )context;
    ffxFuncs.Dispatch( &ctx, &dispatchDesc.header );

    dx12->EndAndExecuteCommands();

    return OUTPUT_IMAGE_INDEX;
}

namespace
{
int32_t GetFsr4JitterPhaseCount( int32_t renderWidth, int32_t displayWidth )
{
    const float basePhaseCount = 8.0f;
    const float jitterPhaseCount = basePhaseCount * powf( float( displayWidth ) / float( renderWidth ), 2.0f );
    return int32_t( ceilf( jitterPhaseCount ) );
}

float Halton( int32_t index, int32_t base )
{
    float f = 1.0f;
    float r = 0.0f;
    while( index > 0 )
    {
        f = f / float( base );
        r = r + f * float( index % base );
        index = index / base;
    }
    return r;
}
}

RgFloat2D RTGL1::FSR4::GetJitter( const ResolutionState& resolutionState, uint32_t frameId )
{
    const int32_t jitterPhaseCount = GetFsr4JitterPhaseCount(
        int32_t( resolutionState.renderWidth ), int32_t( resolutionState.upscaledWidth ) );

    const int32_t index = int32_t( frameId % uint32_t( jitterPhaseCount ) ) + 1;
    const float x = Halton( index, 2 ) - 0.5f;
    const float y = Halton( index, 3 ) - 0.5f;

    return RgFloat2D{ x, y };
}

#else

using namespace RTGL1;

FSR4::FSR4( VkInstance instance, VkDevice device, VkPhysicalDevice physDevice, bool enableFrameGeneration )
    : instance( instance ), device( device ), physDevice( physDevice ), enableFrameGeneration( enableFrameGeneration ), isSupported( false )
{
}

FSR4::~FSR4() = default;

void FSR4::OnFramebuffersSizeChange( const ResolutionState& resolutionState )
{
}

FramebufferImageIndex FSR4::Apply( VkCommandBuffer                        cmd,
                                  uint32_t                               frameIndex,
                                  const std::shared_ptr< Framebuffers >& framebuffers,
                                  const RenderResolutionHelper&          renderResolution,
                                  RgFloat2D                              jitterOffset,
                                  double                                 timeDelta,
                                  float                                  nearPlane,
                                  float                                  farPlane,
                                  float                                  fovVerticalRad,
                                  bool                                   resetAccumulation )
{
    return FramebufferImageIndex::FB_IMAGE_INDEX_UPSCALED_PONG;
}

RgFloat2D FSR4::GetJitter( const ResolutionState& resolutionState, uint32_t frameId )
{
    return RgFloat2D{ 0, 0 };
}

bool FSR4::IsHardwareSupported( VkPhysicalDevice physDevice )
{
    return false;
}

bool FSR4::IsFsr4Available() const
{
    return false;
}

#endif
