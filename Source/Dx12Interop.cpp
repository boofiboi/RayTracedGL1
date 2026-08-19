#include "Dx12Interop.h"

#ifdef RG_USE_AMD_FSR4

#include <vulkan/vulkan_win32.h>
#include <cassert>
#include "Common.h"

using namespace RTGL1;

Dx12Interop::Dx12Interop()
    : fenceEvent( nullptr )
    , vkDevice( VK_NULL_HANDLE )
    , currentFenceValue( 0 )
    , frameIndex( 0 )
    , initialized( false )
{
}

Dx12Interop::~Dx12Interop()
{
    Destroy();
}

void Dx12Interop::DestroySingleSharedTexture( SharedTexture& tex )
{
    if( vkDevice != VK_NULL_HANDLE )
    {
        if( tex.vkImage != VK_NULL_HANDLE )
        {
            vkDestroyImage( vkDevice, tex.vkImage, nullptr );
            tex.vkImage = VK_NULL_HANDLE;
        }
        if( tex.vkMemory != VK_NULL_HANDLE )
        {
            vkFreeMemory( vkDevice, tex.vkMemory, nullptr );
            tex.vkMemory = VK_NULL_HANDLE;
        }
    }

    if( tex.sharedHandle != nullptr )
    {
        CloseHandle( tex.sharedHandle );
        tex.sharedHandle = nullptr;
    }

    tex.d3d12Resource.Reset();
    tex.width = 0;
    tex.height = 0;
}

void Dx12Interop::DestroySharedTextures()
{
    DestroySingleSharedTexture( sharedColor );
    DestroySingleSharedTexture( sharedDepth );
    DestroySingleSharedTexture( sharedMotion );
    DestroySingleSharedTexture( sharedOutput );
}

void Dx12Interop::Destroy()
{
    if( !initialized )
    {
        return;
    }

    WaitForGpu();
    DestroySharedTextures();

    if( fenceEvent != nullptr )
    {
        CloseHandle( fenceEvent );
        fenceEvent = nullptr;
    }

    cmdList.Reset();
    cmdAlloc[ 0 ].Reset();
    cmdAlloc[ 1 ].Reset();
    fence.Reset();
    queue.Reset();
    device.Reset();
    adapter.Reset();
    factory.Reset();

    vkDevice = VK_NULL_HANDLE;
    physDevice.reset();
    initialized = false;
}

bool Dx12Interop::Init( VkInstance vkInstance, std::shared_ptr< PhysicalDevice > inPhysDevice, VkDevice inVkDevice )
{
    if( initialized )
    {
        return true;
    }

    vkDevice = inVkDevice;
    physDevice = inPhysDevice;

    VkPhysicalDeviceIDProperties idProps = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &idProps,
    };
    vkGetPhysicalDeviceProperties2( physDevice->Get(), &props2 );

    HRESULT hr = CreateDXGIFactory1( IID_PPV_ARGS( &factory ) );
    if( FAILED( hr ) )
    {
        return false;
    }

    Microsoft::WRL::ComPtr< IDXGIAdapter1 > candidateAdapter;
    for( UINT i = 0; factory->EnumAdapters1( i, &candidateAdapter ) != DXGI_ERROR_NOT_FOUND; ++i )
    {
        DXGI_ADAPTER_DESC1 desc;
        candidateAdapter->GetDesc1( &desc );

        if( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE )
        {
            continue;
        }

        if( idProps.deviceLUIDValid )
        {
            if( memcmp( &desc.AdapterLuid, idProps.deviceLUID, sizeof( LUID ) ) == 0 )
            {
                adapter = candidateAdapter;
                break;
            }
        }
        else
        {
            adapter = candidateAdapter;
            break;
        }
    }

    if( !adapter )
    {
        return false;
    }

    hr = D3D12CreateDevice( adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS( &device ) );
    if( FAILED( hr ) )
    {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {
        .Type     = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    hr = device->CreateCommandQueue( &queueDesc, IID_PPV_ARGS( &queue ) );
    if( FAILED( hr ) )
    {
        return false;
    }

    for( int i = 0; i < 2; i++ )
    {
        hr = device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS( &cmdAlloc[ i ] ) );
        if( FAILED( hr ) )
        {
            return false;
        }
    }

    hr = device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc[ 0 ].Get(), nullptr, IID_PPV_ARGS( &cmdList ) );
    if( FAILED( hr ) )
    {
        return false;
    }
    cmdList->Close();

    hr = device->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( &fence ) );
    if( FAILED( hr ) )
    {
        return false;
    }

    fenceEvent = CreateEventEx( nullptr, nullptr, 0, EVENT_ALL_ACCESS );
    if( fenceEvent == nullptr )
    {
        return false;
    }

    initialized = true;
    return true;
}

ID3D12Device* Dx12Interop::GetDevice() const
{
    return device.Get();
}

ID3D12CommandQueue* Dx12Interop::GetQueue() const
{
    return queue.Get();
}

ID3D12GraphicsCommandList* Dx12Interop::GetCommandList() const
{
    return cmdList.Get();
}

bool Dx12Interop::CreateSharedTexture(
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT dxgiFormat,
    VkFormat vkFormat,
    D3D12_RESOURCE_FLAGS d3dFlags,
    VkImageUsageFlags vkUsage,
    SharedTexture& outTex )
{
    DestroySingleSharedTexture( outTex );

    outTex.width = width;
    outTex.height = height;

    D3D12_HEAP_PROPERTIES heapProps = {
        .Type                 = D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask     = 1,
        .VisibleNodeMask      = 1,
    };

    D3D12_RESOURCE_DESC desc = {
        .Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment        = 0,
        .Width            = width,
        .Height           = height,
        .DepthOrArraySize = 1,
        .MipLevels        = 1,
        .Format           = dxgiFormat,
        .SampleDesc       = { .Count = 1, .Quality = 0 },
        .Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags            = d3dFlags,
    };

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_SHARED,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS( &outTex.d3d12Resource ) );
    if( FAILED( hr ) || !outTex.d3d12Resource )
    {
        return false;
    }

    hr = device->CreateSharedHandle(
        outTex.d3d12Resource.Get(),
        nullptr,
        GENERIC_ALL,
        nullptr,
        &outTex.sharedHandle );
    if( FAILED( hr ) || outTex.sharedHandle == nullptr )
    {
        return false;
    }

    VkExternalMemoryImageCreateInfo extImageInfo = {
        .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
    };

    VkImageCreateInfo imgInfo = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext         = &extImageInfo,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = vkFormat,
        .extent        = { width, height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = vkUsage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkResult r = vkCreateImage( vkDevice, &imgInfo, nullptr, &outTex.vkImage );
    if( r != VK_SUCCESS )
    {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements( vkDevice, outTex.vkImage, &memReqs );

    VkImportMemoryWin32HandleInfoKHR importInfo = {
        .sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
        .handle     = outTex.sharedHandle,
    };

    VkMemoryAllocateInfo allocInfo = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &importInfo,
        .allocationSize  = memReqs.size,
        .memoryTypeIndex = physDevice->GetMemoryTypeIndex( memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ),
    };

    r = vkAllocateMemory( vkDevice, &allocInfo, nullptr, &outTex.vkMemory );
    if( r != VK_SUCCESS )
    {
        return false;
    }

    r = vkBindImageMemory( vkDevice, outTex.vkImage, outTex.vkMemory, 0 );
    if( r != VK_SUCCESS )
    {
        return false;
    }

    return true;
}

bool Dx12Interop::RecreateSharedTextures( uint32_t renderWidth, uint32_t renderHeight, uint32_t upscaledWidth, uint32_t upscaledHeight )
{
    if( !initialized )
    {
        return false;
    }

    bool ok = true;

    ok &= CreateSharedTexture(
        renderWidth,
        renderHeight,
        DXGI_FORMAT_R11G11B10_FLOAT,
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        sharedColor );

    ok &= CreateSharedTexture(
        renderWidth,
        renderHeight,
        DXGI_FORMAT_R32_FLOAT,
        VK_FORMAT_R32_SFLOAT,
        D3D12_RESOURCE_FLAG_NONE,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        sharedDepth );

    ok &= CreateSharedTexture(
        renderWidth,
        renderHeight,
        DXGI_FORMAT_R16G16_FLOAT,
        VK_FORMAT_R16G16_SFLOAT,
        D3D12_RESOURCE_FLAG_NONE,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        sharedMotion );

    ok &= CreateSharedTexture(
        upscaledWidth,
        upscaledHeight,
        DXGI_FORMAT_R11G11B10_FLOAT,
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        sharedOutput );

    return ok;
}

void Dx12Interop::BeginCommands()
{
    frameIndex = ( frameIndex + 1 ) % 2;
    cmdAlloc[ frameIndex ]->Reset();
    cmdList->Reset( cmdAlloc[ frameIndex ].Get(), nullptr );
}

void Dx12Interop::EndAndExecuteCommands()
{
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    queue->ExecuteCommandLists( 1, lists );
}

void Dx12Interop::WaitForGpu()
{
    if( queue && fence && fenceEvent )
    {
        uint64_t val = ++currentFenceValue;
        queue->Signal( fence.Get(), val );
        if( fence->GetCompletedValue() < val )
        {
            fence->SetEventOnCompletion( val, fenceEvent );
            WaitForSingleObject( fenceEvent, INFINITE );
        }
    }
}

#endif
