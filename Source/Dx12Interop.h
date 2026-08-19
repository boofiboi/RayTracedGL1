#pragma once

#ifdef RG_USE_AMD_FSR4

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>

#include "PhysicalDevice.h"

namespace RTGL1
{

struct SharedTexture
{
    Microsoft::WRL::ComPtr< ID3D12Resource > d3d12Resource;
    HANDLE                                   sharedHandle{ nullptr };
    VkImage                                  vkImage{ VK_NULL_HANDLE };
    VkDeviceMemory                           vkMemory{ VK_NULL_HANDLE };
    uint32_t                                 width{ 0 };
    uint32_t                                 height{ 0 };
};

class Dx12Interop
{
public:
    Dx12Interop();
    ~Dx12Interop();

    Dx12Interop( const Dx12Interop& ) = delete;
    Dx12Interop& operator=( const Dx12Interop& ) = delete;

    bool Init( VkInstance vkInstance, std::shared_ptr< PhysicalDevice > physDevice, VkDevice vkDevice );
    void Destroy();

    ID3D12Device* GetDevice() const;
    ID3D12CommandQueue* GetQueue() const;
    ID3D12GraphicsCommandList* GetCommandList() const;

    bool RecreateSharedTextures( uint32_t renderWidth, uint32_t renderHeight, uint32_t upscaledWidth, uint32_t upscaledHeight );
    void DestroySharedTextures();

    const SharedTexture& GetColorTexture() const { return sharedColor; }
    const SharedTexture& GetDepthTexture() const { return sharedDepth; }
    const SharedTexture& GetMotionTexture() const { return sharedMotion; }
    const SharedTexture& GetOutputTexture() const { return sharedOutput; }

    void BeginCommands();
    void EndAndExecuteCommands();
    void WaitForGpu();

private:
    bool CreateSharedTexture(
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT dxgiFormat,
        VkFormat vkFormat,
        D3D12_RESOURCE_FLAGS d3dFlags,
        VkImageUsageFlags vkUsage,
        SharedTexture& outTex );

    void DestroySingleSharedTexture( SharedTexture& tex );

private:
    Microsoft::WRL::ComPtr< IDXGIFactory4 > factory;
    Microsoft::WRL::ComPtr< IDXGIAdapter1 > adapter;
    Microsoft::WRL::ComPtr< ID3D12Device > device;
    Microsoft::WRL::ComPtr< ID3D12CommandQueue > queue;
    Microsoft::WRL::ComPtr< ID3D12CommandAllocator > cmdAlloc[ 2 ];
    Microsoft::WRL::ComPtr< ID3D12GraphicsCommandList > cmdList;
    Microsoft::WRL::ComPtr< ID3D12Fence > fence;
    HANDLE fenceEvent;

    SharedTexture sharedColor;
    SharedTexture sharedDepth;
    SharedTexture sharedMotion;
    SharedTexture sharedOutput;

    VkDevice vkDevice;
    std::shared_ptr< PhysicalDevice > physDevice;
    uint64_t currentFenceValue;
    uint32_t frameIndex;
    bool initialized;
};

}

#endif
