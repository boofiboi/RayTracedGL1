#include "NgxManager.h"

#include <cassert>
#include <functional>
#include <filesystem>
#include <regex>

#include "RTGL1/RTGL1.h"
#include "DebugPrint.h"
#include "RgException.h"

#ifdef RG_USE_NVIDIA_DLSS

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers.h>

#if __linux__
    #include <unistd.h>
    #include <linux/limits.h>
#endif

static void PrintCallback( const char*             message,
                           NVSDK_NGX_Logging_Level loggingLevel,
                           NVSDK_NGX_Feature       sourceComponent )
{
    RTGL1::debug::Verbose(
        "DLSS: NVSDK_NGX_Feature={}: {}", static_cast< int >( sourceComponent ), message );
}

static std::wstring GetFolderPath()
{
#if defined( _WIN32 )
    wchar_t appPath[ MAX_PATH ];
    GetModuleFileNameW( NULL, appPath, MAX_PATH );
#elif defined( __linux__ )
    wchar_t appPath[ PATH_MAX ];
    char    appPath_c[ PATH_MAX ];
    ssize_t count = readlink( "/proc/self/exe", appPath_c, PATH_MAX - 1 );

    if( count <= 0 )
    {
        return {};
    }

    appPath_c[ count ] = '\0';
    std::mbstowcs( appPath, appPath_c, PATH_MAX );
#endif

    std::wstring curFolderPath = appPath;
    auto         p             = curFolderPath.find_last_of( L"\\/" );

    if( p == std::wstring::npos )
    {
        return {};
    }

    return curFolderPath.substr( 0, p );
}

RTGL1::NgxManager::NgxManager( VkInstance       instance,
                               VkDevice         _device,
                               VkPhysicalDevice physDevice,
                               const char*      pAppGuid,
                               bool             enableDebug )
    : device( _device )
    , isInitialized( false )
    , pParams( nullptr )
{
    isInitialized = TryInit( instance, _device, physDevice, pAppGuid, enableDebug );

    if( isInitialized )
    {
        CheckDriverSupport();
    }
}

RTGL1::NgxManager::~NgxManager()
{
    Destroy();
}

bool RTGL1::NgxManager::TryInit( VkInstance       instance,
                                 VkDevice         _device,
                                 VkPhysicalDevice physDevice,
                                 const char*      pAppGuid,
                                 bool             enableDebug )
{
    NVSDK_NGX_Result r;

    std::wstring   dllPath   = GetFolderPath() + ( enableDebug ? L"/dev/" : L"/rel/" );
    const wchar_t* dllPath_c = dllPath.c_str();

    NVSDK_NGX_PathListInfo pathsInfo = {
        .Path   = &dllPath_c,
        .Length = 1,
    };

    NVSDK_NGX_FeatureCommonInfo commonInfo = {
        .PathListInfo = pathsInfo,
        .LoggingInfo =
            enableDebug
                ? NVSDK_NGX_LoggingInfo{ .LoggingCallback     = &PrintCallback,
                                         .MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON }
                : NVSDK_NGX_LoggingInfo{ .LoggingCallback     = nullptr,
                                         .MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF },
    };

    {
        const std::regex guidRegex(
            "^[{]?[0-9a-fA-F]{8}-([0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}[}]?$" );

        if( pAppGuid == nullptr )
        {
            throw RgException(
                RG_RESULT_WRONG_FUNCTION_ARGUMENT,
                "Application GUID wasn't provided. Generate and specify it to use DLSS." );
        }

        if( !std::regex_match( pAppGuid, guidRegex ) )
        {
            throw RgException(
                RG_RESULT_WRONG_FUNCTION_ARGUMENT,
                "Provided application GUID is not GUID. Generate and specify correct "
                "GUID to use DLSS." );
        }
    }

    auto dataPath = L"DLSSTemp/";
    if( enableDebug )
    {
        std::error_code err;
        std::filesystem::create_directory( dataPath, err );
        if( err )
        {
            debug::Warning( "Failed to create DLSSTemp directory. create_directory error: {}",
                            err.message() );
        }
    }

    r = NVSDK_NGX_VULKAN_Init_with_ProjectID( pAppGuid,
                                              NVSDK_NGX_EngineType::NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                              RG_RTGL_VERSION_API,
                                              dataPath,
                                              instance,
                                              physDevice,
                                              _device,
                                              nullptr,
                                              nullptr,
                                              &commonInfo );

    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS: NVSDK_NGX_VULKAN_Init_with_ProjectID fail: {}",
                        static_cast< int >( r ) );
        return false;
    }

    r = NVSDK_NGX_VULKAN_GetCapabilityParameters( &pParams );
    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS: NVSDK_NGX_VULKAN_GetCapabilityParameters fail: {}",
                        static_cast< int >( r ) );

        NVSDK_NGX_VULKAN_Shutdown1( _device );
        pParams = nullptr;

        return false;
    }

    return true;
}

void RTGL1::NgxManager::CheckDriverSupport() const
{
    if( !isInitialized || pParams == nullptr )
    {
        return;
    }

    int          needsUpdatedDriver    = 0;
    unsigned int minDriverVersionMajor = 0;
    unsigned int minDriverVersionMinor = 0;

    NVSDK_NGX_Result r_upd =
        pParams->Get( NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver );
    NVSDK_NGX_Result r_mjr = pParams->Get( NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor,
                                           &minDriverVersionMajor );
    NVSDK_NGX_Result r_mnr = pParams->Get( NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor,
                                           &minDriverVersionMinor );

    if( NVSDK_NGX_SUCCEED( r_upd ) && NVSDK_NGX_SUCCEED( r_mjr ) && NVSDK_NGX_SUCCEED( r_mnr ) )
    {
        if( needsUpdatedDriver )
        {
            debug::Warning( "DLSS: Outdated driver. Min driver version: {}", minDriverVersionMinor );
        }
        else
        {
            debug::Verbose( "DLSS: Reported Min driver version: {}", minDriverVersionMinor );
        }
    }
    else
    {
        debug::Warning( "DLSS: Minimum driver version was not reported" );
    }
}

void RTGL1::NgxManager::Destroy()
{
    if( isInitialized )
    {
        vkDeviceWaitIdle( device );

        if( pParams != nullptr )
        {
            NVSDK_NGX_VULKAN_DestroyParameters( pParams );
            pParams = nullptr;
        }

        NVSDK_NGX_VULKAN_Shutdown1( device );
        isInitialized = false;
    }
}

bool RTGL1::NgxManager::IsInitialized() const
{
    return isInitialized && pParams != nullptr;
}

NVSDK_NGX_Parameter* RTGL1::NgxManager::GetParameters() const
{
    return pParams;
}

bool RTGL1::NgxManager::IsSuperSamplingAvailable() const
{
    if( !isInitialized || pParams == nullptr )
    {
        return false;
    }

    int              isDlssSupported = 0;
    NVSDK_NGX_Result r = pParams->Get( NVSDK_NGX_Parameter_SuperSampling_Available, &isDlssSupported );
    if( NVSDK_NGX_FAILED( r ) || !isDlssSupported )
    {
        return false;
    }

    return true;
}

bool RTGL1::NgxManager::IsSuperSamplingDenoisingAvailable() const
{
    if( !isInitialized || pParams == nullptr )
    {
        return false;
    }

    int              isDlssdSupported = 0;
    NVSDK_NGX_Result r = pParams->Get( NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &isDlssdSupported );
    if( NVSDK_NGX_FAILED( r ) || !isDlssdSupported )
    {
        return false;
    }

    return true;
}

bool RTGL1::NgxManager::IsFrameGenerationAvailable() const
{
    if( !isInitialized || pParams == nullptr )
    {
        return false;
    }

    int              isDlssgSupported = 0;
    NVSDK_NGX_Result r = pParams->Get( NVSDK_NGX_Parameter_FrameGeneration_Available, &isDlssgSupported );
    if( NVSDK_NGX_FAILED( r ) || !isDlssgSupported )
    {
        return false;
    }

    return true;
}

std::vector< const char* > RTGL1::NgxManager::GetVulkanInstanceExtensions()
{
    uint32_t     instanceExtCount;
    const char** ppInstanceExts;
    uint32_t     deviceExtCount;
    const char** ppDeviceExts;

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(
        &instanceExtCount, &ppInstanceExts, &deviceExtCount, &ppDeviceExts );
    if( !NVSDK_NGX_SUCCEED( r ) )
    {
        debug::Warning( "DLSS: NVSDK_NGX_VULKAN_RequiredExtensions fail: {}",
                        static_cast< int >( r ) );
        return {};
    }

    std::vector< const char* > v;

    for( uint32_t i = 0; i < instanceExtCount; i++ )
    {
        v.push_back( ppInstanceExts[ i ] );
    }

    return v;
}

std::vector< const char* > RTGL1::NgxManager::GetVulkanDeviceExtensions()
{
    uint32_t     instanceExtCount;
    const char** ppInstanceExts;
    uint32_t     deviceExtCount;
    const char** ppDeviceExts;

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(
        &instanceExtCount, &ppInstanceExts, &deviceExtCount, &ppDeviceExts );
    if( !NVSDK_NGX_SUCCEED( r ) )
    {
        debug::Warning( "DLSS: NVSDK_NGX_VULKAN_RequiredExtensions fail: {}",
                        static_cast< int >( r ) );
        return {};
    }

    std::vector< const char* > v;

    for( uint32_t i = 0; i < deviceExtCount; i++ )
    {
        if( strcmp( ppDeviceExts[ i ], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME ) == 0 )
        {
            continue;
        }

        v.push_back( ppDeviceExts[ i ] );
    }

    return v;
}

#else

RTGL1::NgxManager::NgxManager( VkInstance       instance,
                               VkDevice         _device,
                               VkPhysicalDevice physDevice,
                               const char*      pAppGuid,
                               bool             enableDebug )
    : device( _device )
    , isInitialized( false )
    , pParams( nullptr )
{
}

RTGL1::NgxManager::~NgxManager() = default;

bool RTGL1::NgxManager::TryInit( VkInstance       instance,
                                 VkDevice         _device,
                                 VkPhysicalDevice physDevice,
                                 const char*      pAppGuid,
                                 bool             enableDebug )
{
    return false;
}

void RTGL1::NgxManager::CheckDriverSupport() const {}

void RTGL1::NgxManager::Destroy() {}

bool RTGL1::NgxManager::IsInitialized() const
{
    return false;
}

NVSDK_NGX_Parameter* RTGL1::NgxManager::GetParameters() const
{
    return nullptr;
}

bool RTGL1::NgxManager::IsSuperSamplingAvailable() const
{
    return false;
}

bool RTGL1::NgxManager::IsSuperSamplingDenoisingAvailable() const
{
    return false;
}

bool RTGL1::NgxManager::IsFrameGenerationAvailable() const
{
    return false;
}

std::vector< const char* > RTGL1::NgxManager::GetVulkanInstanceExtensions()
{
    return {};
}

std::vector< const char* > RTGL1::NgxManager::GetVulkanDeviceExtensions()
{
    return {};
}

#endif
