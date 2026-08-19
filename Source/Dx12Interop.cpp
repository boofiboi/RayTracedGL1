#include "Dx12Interop.h"

#ifdef RG_USE_AMD_FSR4

#include <vulkan/vulkan_win32.h>
#include <cassert>
#include "Common.h"

using namespace RTGL1;

Dx12Interop::Dx12Interop()
    : fenceSharedHandle( NULL )
    , vkTimelineSemaphore( VK_NULL_HANDLE )
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

void Dx12Interop::Destroy()
{
    if( !initialized )
    {
        return;
    }

    if( queue && fence )
    {
        uint64_t val = ++currentFenceValue;
        queue->Signal( fence.Get(), val );
        if( fence->GetCompletedValue() < val )
        {
            HANDLE event = CreateEventEx( nullptr, nullptr, 0, EVENT_ALL_ACCESS );
            if( event )
            {
                fence->SetEventOnCompletion( val, event );
                WaitForSingleObject( event, 2000 );
                CloseHandle( event );
            }
        }
    }

    if( vkDevice != VK_NULL_HANDLE && vkTimelineSemaphore != VK_NULL_HANDLE )
    {
        vkDestroySemaphore( vkDevice, vkTimelineSemaphore, nullptr );
        vkTimelineSemaphore = VK_NULL_HANDLE;
    }

    if( fenceSharedHandle != NULL )
    {
        CloseHandle( fenceSharedHandle );
        fenceSharedHandle = NULL;
    }

    cmdList.Reset();
    cmdAlloc[ 0 ].Reset();
    cmdAlloc[ 1 ].Reset();
    fence.Reset();
    queue.Reset();
    device.Reset();
    adapter.Reset();
    factory.Reset();

    initialized = false;
}

bool Dx12Interop::Init( VkInstance vkInstance, VkPhysicalDevice vkPhysDevice, VkDevice inVkDevice )
{
    if( initialized )
    {
        return true;
    }

    vkDevice = inVkDevice;

    VkPhysicalDeviceIDProperties idProps = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &idProps,
    };
    vkGetPhysicalDeviceProperties2( vkPhysDevice, &props2 );

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

    hr = device->CreateFence( 0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS( &fence ) );
    if( FAILED( hr ) )
    {
        return false;
    }

    hr = device->CreateSharedHandle( fence.Get(), nullptr, GENERIC_ALL, nullptr, &fenceSharedHandle );
    if( FAILED( hr ) || fenceSharedHandle == NULL )
    {
        return false;
    }

    PFN_vkImportSemaphoreWin32HandleKHR pfnVkImportSemaphoreWin32Handle =
        ( PFN_vkImportSemaphoreWin32HandleKHR )vkGetDeviceProcAddr( vkDevice, "vkImportSemaphoreWin32HandleKHR" );

    if( pfnVkImportSemaphoreWin32Handle != nullptr )
    {
        VkSemaphoreTypeCreateInfo timelineInfo = {
            .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue  = 0,
        };
        VkSemaphoreCreateInfo semInfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timelineInfo,
        };
        VkResult r = vkCreateSemaphore( vkDevice, &semInfo, nullptr, &vkTimelineSemaphore );
        if( r == VK_SUCCESS )
        {
            VkImportSemaphoreWin32HandleInfoKHR importInfo = {
                .sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
                .semaphore  = vkTimelineSemaphore,
                .flags      = 0,
                .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
                .handle     = fenceSharedHandle,
            };
            r = pfnVkImportSemaphoreWin32Handle( vkDevice, &importInfo );
            if( r != VK_SUCCESS )
            {
                vkDestroySemaphore( vkDevice, vkTimelineSemaphore, nullptr );
                vkTimelineSemaphore = VK_NULL_HANDLE;
            }
        }
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

VkSemaphore Dx12Interop::GetVkTimelineSemaphore() const
{
    return vkTimelineSemaphore;
}

Microsoft::WRL::ComPtr< ID3D12Resource > Dx12Interop::ImportPlacedResource(
    HANDLE win32MemoryHandle,
    const D3D12_RESOURCE_DESC& desc,
    D3D12_RESOURCE_STATES initialState )
{
    if( !device || win32MemoryHandle == NULL )
    {
        return nullptr;
    }

    Microsoft::WRL::ComPtr< ID3D12Heap > heap;
    HRESULT hr = device->OpenSharedHandle( win32MemoryHandle, IID_PPV_ARGS( &heap ) );
    if( FAILED( hr ) || !heap )
    {
        return nullptr;
    }

    Microsoft::WRL::ComPtr< ID3D12Resource > resource;
    hr = device->CreatePlacedResource(
        heap.Get(),
        0,
        &desc,
        initialState,
        nullptr,
        IID_PPV_ARGS( &resource ) );

    if( FAILED( hr ) )
    {
        return nullptr;
    }

    return resource;
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

void Dx12Interop::SyncVulkanToDx12( uint64_t fenceVal )
{
    if( queue && fence )
    {
        queue->Wait( fence.Get(), fenceVal );
    }
}

void Dx12Interop::SyncDx12ToVulkan( uint64_t fenceVal )
{
    if( queue && fence )
    {
        queue->Signal( fence.Get(), fenceVal );
    }
}

uint64_t Dx12Interop::GetNextFenceValue()
{
    return ++currentFenceValue;
}

#endif
