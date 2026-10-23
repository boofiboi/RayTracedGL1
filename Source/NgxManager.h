#pragma once

#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

struct NVSDK_NGX_Parameter;

namespace RTGL1
{

class NgxManager
{
public:
    NgxManager( VkInstance       instance,
                VkDevice         device,
                VkPhysicalDevice physDevice,
                const char*      pAppGuid,
                bool             enableDebug );
    ~NgxManager();

    NgxManager( const NgxManager& other )                = delete;
    NgxManager( NgxManager&& other ) noexcept            = delete;
    NgxManager& operator=( const NgxManager& other )     = delete;
    NgxManager& operator=( NgxManager&& other ) noexcept = delete;

    bool                 IsInitialized() const;
    NVSDK_NGX_Parameter* GetParameters() const;

    bool IsSuperSamplingAvailable() const;
    bool IsSuperSamplingDenoisingAvailable() const;
    bool IsFrameGenerationAvailable() const;

    static std::vector< const char* > GetVulkanInstanceExtensions();
    static std::vector< const char* > GetVulkanDeviceExtensions();

private:
    bool TryInit( VkInstance       instance,
                  VkDevice         device,
                  VkPhysicalDevice physDevice,
                  const char*      pAppGuid,
                  bool             enableDebug );
    void CheckDriverSupport() const;
    void Destroy();

private:
    VkDevice             device;
    bool                 isInitialized;
    NVSDK_NGX_Parameter* pParams;
};

}
