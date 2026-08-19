#pragma once

#ifdef RG_USE_AMD_FSR4

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>

namespace RTGL1
{

class Dx12Interop
{
public:
    Dx12Interop();
    ~Dx12Interop();

    Dx12Interop( const Dx12Interop& ) = delete;
    Dx12Interop& operator=( const Dx12Interop& ) = delete;

    bool Init( VkInstance vkInstance, VkPhysicalDevice vkPhysDevice, VkDevice vkDevice );
    void Destroy();

    ID3D12Device* GetDevice() const;
    ID3D12CommandQueue* GetQueue() const;
    ID3D12GraphicsCommandList* GetCommandList() const;
    VkSemaphore GetVkTimelineSemaphore() const;

    Microsoft::WRL::ComPtr< ID3D12Resource > ImportPlacedResource(
        HANDLE win32MemoryHandle,
        const D3D12_RESOURCE_DESC& desc,
        D3D12_RESOURCE_STATES initialState );

    void BeginCommands();
    void EndAndExecuteCommands();

    void SyncVulkanToDx12( uint64_t fenceVal );
    void SyncDx12ToVulkan( uint64_t fenceVal );

    uint64_t GetNextFenceValue();

private:
    Microsoft::WRL::ComPtr< IDXGIFactory4 > factory;
    Microsoft::WRL::ComPtr< IDXGIAdapter1 > adapter;
    Microsoft::WRL::ComPtr< ID3D12Device > device;
    Microsoft::WRL::ComPtr< ID3D12CommandQueue > queue;
    Microsoft::WRL::ComPtr< ID3D12CommandAllocator > cmdAlloc[ 2 ];
    Microsoft::WRL::ComPtr< ID3D12GraphicsCommandList > cmdList;
    Microsoft::WRL::ComPtr< ID3D12Fence > fence;

    HANDLE fenceSharedHandle;
    VkSemaphore vkTimelineSemaphore;
    VkDevice vkDevice;
    uint64_t currentFenceValue;
    uint32_t frameIndex;
    bool initialized;
};

}

#endif
