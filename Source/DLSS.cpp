// Copyright (c) 2021 Sultim Tsyrendashiev
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "DLSS.h"

#include <algorithm>
#include <filesystem>
#include <regex>

#include "RTGL1/RTGL1.h"
#include "CmdLabel.h"
#include "HaltonSequence.h"
#include "RenderResolutionHelper.h"


#ifdef RG_USE_NVIDIA_DLSS


    #include <nvsdk_ngx_helpers_vk.h>
    #include <nvsdk_ngx_helpers.h>
    #include <nvsdk_ngx_helpers_dlssg_vk.h>

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

RTGL1::DLSS::DLSS( VkInstance       _instance,
                   VkDevice         _device,
                   VkPhysicalDevice _physDevice,
                   const char*      _pAppGuid,
                   bool             _enableDebug )
    : device( _device )
    , isInitialized( false )
    , pParams( nullptr )
    , pDlssFeature( nullptr )
    , pDlssgFeature( nullptr )
    , prevDlssFeatureValues{}
    , prevDlssgFeatureValues{}
{
    isInitialized = TryInit( _instance, _device, _physDevice, _pAppGuid, _enableDebug );

    if( !CheckSupport() )
    {
        Destroy();
    }
}

static const char* GetNgxResultString( NVSDK_NGX_Result result )
{
    switch( result )
    {
        case NVSDK_NGX_Result_Success: return "Success";
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "Feature not supported (NVIDIA RTX GPU required)";
        case NVSDK_NGX_Result_FAIL_PlatformError: return "Platform error (graphics API, driver, or OS error)";
        case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "Feature already exists";
        case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "Feature not found (nvngx_dlss.dll missing or unable to load)";
        case NVSDK_NGX_Result_FAIL_InvalidParameter: return "Invalid parameter";
        case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return "Scratch buffer too small";
        case NVSDK_NGX_Result_FAIL_NotInitialized: return "Not initialized";
        case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "Unsupported input format";
        case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "RW flag missing";
        case NVSDK_NGX_Result_FAIL_MissingInput: return "Missing input";
        case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "Unable to initialize feature (nvngx_dlss.dll missing or driver incompatible)";
        case NVSDK_NGX_Result_FAIL_OutOfDate: return "Out of date (NVIDIA driver update required)";
        case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "Out of GPU memory";
        case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "Unsupported format";
        case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "Unable to write to application data path";
        case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "Unsupported parameter";
        case NVSDK_NGX_Result_FAIL_Denied: return "Denied by NVIDIA driver";
        case NVSDK_NGX_Result_FAIL_NotImplemented: return "Not implemented";
        default: return "Unknown NGX error";
    }
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

#if defined( _WIN32 )
static std::wstring GetDllFolderPath()
{
    HMODULE hModule = NULL;
    if( GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast< LPCWSTR >( &GetFolderPath ),
            &hModule ) )
    {
        wchar_t modPath[ MAX_PATH ];
        if( GetModuleFileNameW( hModule, modPath, MAX_PATH ) )
        {
            std::wstring curFolderPath = modPath;
            auto         p             = curFolderPath.find_last_of( L"\\/" );
            if( p != std::wstring::npos )
            {
                return curFolderPath.substr( 0, p );
            }
        }
    }
    return {};
}
#endif

bool RTGL1::DLSS::TryInit( VkInstance       instance,
                           VkDevice         device,
                           VkPhysicalDevice physDevice,
                           const char*      pAppGuid,
                           bool             enableDebug )
{
    VkPhysicalDeviceProperties devProps{};
    vkGetPhysicalDeviceProperties( physDevice, &devProps );

    if( devProps.vendorID != 0x10DE )
    {
        debug::Warning( "DLSS: Disabled - GPU is not NVIDIA (detected: {}, vendor ID 0x{:04X})",
                        devProps.deviceName,
                        devProps.vendorID );
        return false;
    }

    NVSDK_NGX_Result r;

    std::vector< std::wstring > searchDirs;
    auto addSearchDir = [ &searchDirs ]( const std::wstring& dir ) {
        if( !dir.empty() && std::find( searchDirs.begin(), searchDirs.end(), dir ) == searchDirs.end() )
        {
            searchDirs.push_back( dir );
        }
    };

#if defined( _WIN32 )
    std::wstring dllFolder = GetDllFolderPath();
    if( !dllFolder.empty() )
    {
        addSearchDir( dllFolder );
        addSearchDir( dllFolder + ( enableDebug ? L"/dev" : L"/rel" ) );
    }
#endif

    std::wstring appFolder = GetFolderPath();
    if( !appFolder.empty() )
    {
        addSearchDir( appFolder );
        addSearchDir( appFolder + L"/rt_bin" );
        addSearchDir( appFolder + ( enableDebug ? L"/dev" : L"/rel" ) );
        addSearchDir( appFolder + L"/rt_bin" + ( enableDebug ? L"/dev" : L"/rel" ) );
    }

    bool dllFound = false;
    for( const auto& dir : searchDirs )
    {
        std::filesystem::path p( dir );
        p /= "nvngx_dlss.dll";
        std::error_code ec;
        if( std::filesystem::exists( p, ec ) )
        {
            dllFound = true;
            debug::Info( "DLSS: Found nvngx_dlss.dll at: {}", p.string() );
            break;
        }
    }

    if( !dllFound )
    {
        debug::Warning( "DLSS: nvngx_dlss.dll not found in search paths" );
        for( const auto& dir : searchDirs )
        {
            std::filesystem::path p( dir );
            debug::Warning( "DLSS:   checked: {}", p.string() );
        }
    }

    std::vector< const wchar_t* > dllPaths;
    dllPaths.reserve( searchDirs.size() );
    for( const auto& dir : searchDirs )
    {
        dllPaths.push_back( dir.c_str() );
    }

    NVSDK_NGX_PathListInfo pathsInfo = {
        .Path   = dllPaths.data(),
        .Length = static_cast< uint32_t >( dllPaths.size() ),
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
    std::error_code ecDataPath;
    std::filesystem::create_directories( dataPath, ecDataPath );
    if( ecDataPath )
    {
        debug::Warning( "Failed to create DLSSTemp directory: {}", ecDataPath.message() );
    }

    r = NVSDK_NGX_VULKAN_Init_with_ProjectID( pAppGuid,
                                              NVSDK_NGX_EngineType::NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                              RG_RTGL_VERSION_API,
                                              dataPath,
                                              instance,
                                              physDevice,
                                              device,
                                              nullptr,
                                              nullptr,
                                              &commonInfo );

    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS: NVSDK_NGX_VULKAN_Init_with_ProjectID failed: {} (0x{:08X})",
                        GetNgxResultString( r ),
                        static_cast< uint32_t >( r ) );
        return false;
    }

    r = NVSDK_NGX_VULKAN_GetCapabilityParameters( &pParams );
    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS: NVSDK_NGX_VULKAN_GetCapabilityParameters failed: {} (0x{:08X})",
                        GetNgxResultString( r ),
                        static_cast< uint32_t >( r ) );

        NVSDK_NGX_VULKAN_Shutdown1( device );
        pParams = nullptr;

        return false;
    }

    return true;
}

bool RTGL1::DLSS::CheckSupport() const
{
    if( !isInitialized || pParams == nullptr )
    {
        debug::Warning( "DLSS: CheckSupport failed - not initialized or parameters null" );
        return false;
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
            debug::Warning( "DLSS: Outdated NVIDIA driver. Minimum required driver version: {}.{}",
                            minDriverVersionMajor, minDriverVersionMinor );
            return false;
        }
        else
        {
            debug::Info( "DLSS: Minimum driver check passed (min required: {}.{})", minDriverVersionMajor, minDriverVersionMinor );
        }
    }
    else
    {
        debug::Warning( "DLSS: Minimum driver version was not reported by NGX" );
    }

    int              isDlssSupported = 0;
    NVSDK_NGX_Result featureInitResult = NVSDK_NGX_Result_Success;
    NVSDK_NGX_Result r;

    r = pParams->Get( NVSDK_NGX_Parameter_SuperSampling_Available, &isDlssSupported );
    if( NVSDK_NGX_FAILED( r ) || !isDlssSupported )
    {
        r = NVSDK_NGX_Parameter_GetI( pParams,
                                      NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,
                                      reinterpret_cast< int* >( &featureInitResult ) );
        if( NVSDK_NGX_SUCCEED( r ) )
        {
            debug::Warning( "DLSS: SuperSampling not available on this GPU/platform: {} (0x{:08X})",
                            GetNgxResultString( featureInitResult ),
                            static_cast< uint32_t >( featureInitResult ) );
        }
        else
        {
            debug::Warning( "DLSS: SuperSampling_Available query returned {} (supported={})",
                            GetNgxResultString( r ), isDlssSupported );
        }

        return false;
    }

    debug::Info( "DLSS: SuperSampling (DLSS 2) is supported and ready" );
    return true;
}

RTGL1::DLSS::~DLSS()
{
    Destroy();
}

void RTGL1::DLSS::DestroyDlssFeature()
{
    assert( pDlssFeature != nullptr );

    vkDeviceWaitIdle( device );

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_ReleaseFeature( pDlssFeature );
    pDlssFeature       = nullptr;

    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS: NVSDK_NGX_VULKAN_ReleaseFeature failed: {} (0x{:08X})",
                        GetNgxResultString( r ),
                        static_cast< uint32_t >( r ) );
    }
}

void RTGL1::DLSS::DestroyDlssgFeature()
{
    if( pDlssgFeature != nullptr )
    {
        vkDeviceWaitIdle( device );

        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_ReleaseFeature( pDlssgFeature );
        pDlssgFeature       = nullptr;

        if( NVSDK_NGX_FAILED( r ) )
        {
            debug::Warning( "DLSS-G: NVSDK_NGX_VULKAN_ReleaseFeature failed: {} (0x{:08X})",
                            GetNgxResultString( r ),
                            static_cast< uint32_t >( r ) );
        }
    }

    for( auto& [ img, view ] : viewCache )
    {
        if( view != VK_NULL_HANDLE )
        {
            vkDestroyImageView( device, view, nullptr );
        }
    }
    viewCache.clear();
}

VkImageView RTGL1::DLSS::GetOrCreateView( VkImage image, VkFormat format, VkImageAspectFlags aspect )
{
    if( image == VK_NULL_HANDLE )
    {
        return VK_NULL_HANDLE;
    }
    auto it = viewCache.find( image );
    if( it != viewCache.end() )
    {
        return it->second;
    }
    VkImageViewCreateInfo viewInfo = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange = {
            .aspectMask     = aspect,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    VkImageView view = VK_NULL_HANDLE;
    VkResult r = vkCreateImageView( device, &viewInfo, nullptr, &view );
    if( r == VK_SUCCESS )
    {
        viewCache[ image ] = view;
    }
    return view;
}

void RTGL1::DLSS::Destroy()
{
    if( isInitialized )
    {
        vkDeviceWaitIdle( device );

        if( pDlssFeature != nullptr )
        {
            DestroyDlssFeature();
        }

        if( pDlssgFeature != nullptr )
        {
            DestroyDlssgFeature();
        }

        NVSDK_NGX_VULKAN_DestroyParameters( pParams );
        NVSDK_NGX_VULKAN_Shutdown1( device );

        pParams       = nullptr;
        isInitialized = false;
    }
}

bool RTGL1::DLSS::IsDlssAvailable() const
{
    return isInitialized && pParams != nullptr /* && pDlssFeature != nullptr */;
}

bool RTGL1::DLSS::IsDlssgAvailable() const
{
    if( !isInitialized || pParams == nullptr )
    {
        return false;
    }
    int available = 0;
    NVSDK_NGX_Result r = pParams->Get( NVSDK_NGX_Parameter_FrameGeneration_Available, &available );
    if( NVSDK_NGX_SUCCEED( r ) && available )
    {
        return true;
    }
    r = pParams->Get( NVSDK_NGX_Parameter_FrameInterpolation_Available, &available );
    return NVSDK_NGX_SUCCEED( r ) && available;
}

static NVSDK_NGX_PerfQuality_Value ToNGXPerfQuality( RgRenderResolutionMode mode )
{
    switch( mode )
    {
        case RG_RENDER_RESOLUTION_MODE_CUSTOM:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_DLAA;
        case RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        case RG_RENDER_RESOLUTION_MODE_PERFORMANCE:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case RG_RENDER_RESOLUTION_MODE_BALANCED:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_Balanced;
        case RG_RENDER_RESOLUTION_MODE_QUALITY:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case RG_RENDER_RESOLUTION_MODE_ULTRA_QUALITY:
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_UltraQuality;
        default:
            assert( 0 );
            return NVSDK_NGX_PerfQuality_Value::NVSDK_NGX_PerfQuality_Value_Balanced;
    }
}

bool RTGL1::DLSS::AreSameDlssFeatureValues( const RenderResolutionHelper& renderResolution ) const
{
    return prevDlssFeatureValues.renderWidth == renderResolution.Width() &&
           prevDlssFeatureValues.renderHeight == renderResolution.Height() &&
           prevDlssFeatureValues.upscaledWidth == renderResolution.UpscaledWidth() &&
           prevDlssFeatureValues.upscaledHeight == renderResolution.UpscaledHeight();
}

void RTGL1::DLSS::SaveDlssFeatureValues( const RenderResolutionHelper& renderResolution )
{
    prevDlssFeatureValues = {
        .renderWidth    = renderResolution.Width(),
        .renderHeight   = renderResolution.Height(),
        .upscaledWidth  = renderResolution.UpscaledWidth(),
        .upscaledHeight = renderResolution.UpscaledHeight(),
    };
}

bool RTGL1::DLSS::ValidateDlssFeature( VkCommandBuffer               cmd,
                                       const RenderResolutionHelper& renderResolution )
{
    if( !isInitialized || pParams == nullptr )
    {
        return false;
    }


    if( AreSameDlssFeatureValues( renderResolution ) )
    {
        return true;
    }
    SaveDlssFeatureValues( renderResolution );


    if( pDlssFeature != nullptr )
    {
        DestroyDlssFeature();
    }


    NVSDK_NGX_DLSS_Create_Params dlssParams = {
        .Feature = { .InWidth        = renderResolution.Width(),
                     .InHeight       = renderResolution.Height(),
                     .InTargetWidth  = renderResolution.UpscaledWidth(),
                     .InTargetHeight = renderResolution.UpscaledHeight() },
    };
    // dlssParams.Feature.InPerfQualityValue =
    // ToNGXPerfQuality(renderResolution.GetResolutionMode());

    int& dlssCreateFeatureFlags = dlssParams.InFeatureCreateFlags;
    // motion vectors are in render resolution, not target resolution
    dlssCreateFeatureFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    dlssCreateFeatureFlags |= 0; // NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    dlssCreateFeatureFlags |= 0; // NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    dlssCreateFeatureFlags |= 0; // NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    dlssCreateFeatureFlags |= 0; // NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

    // only one phys device
    uint32_t creationNodeMask   = 1;
    uint32_t visibilityNodeMask = 1;


    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(
        cmd, creationNodeMask, visibilityNodeMask, &pDlssFeature, pParams, &dlssParams );
    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS: NGX_VULKAN_CREATE_DLSS_EXT failed: {} (0x{:08X})",
                        GetNgxResultString( r ),
                        static_cast< uint32_t >( r ) );

        pDlssFeature = nullptr;
        return false;
    }

    return true;
}

bool RTGL1::DLSS::ValidateDlssgFeature( VkCommandBuffer cmd,
                                        uint32_t        width,
                                        uint32_t        height,
                                        VkFormat        backBufferFormat )
{
    if( !isInitialized || pParams == nullptr )
    {
        return false;
    }

    if( prevDlssgFeatureValues.width == width && prevDlssgFeatureValues.height == height &&
        prevDlssgFeatureValues.format == backBufferFormat && pDlssgFeature != nullptr )
    {
        return true;
    }

    prevDlssgFeatureValues.width  = width;
    prevDlssgFeatureValues.height = height;
    prevDlssgFeatureValues.format = backBufferFormat;

    if( pDlssgFeature != nullptr )
    {
        DestroyDlssgFeature();
    }

    NVSDK_NGX_DLSSG_Create_Params dlssgParams = {
        .Width                    = width,
        .Height                   = height,
        .NativeBackbufferFormat   = static_cast< unsigned int >( backBufferFormat ),
        .RenderWidth              = width,
        .RenderHeight             = height,
        .DynamicResolutionScaling = false,
    };

    uint32_t creationNodeMask   = 1;
    uint32_t visibilityNodeMask = 1;

    NVSDK_NGX_Result r = NGX_VK_CREATE_DLSSG(
        cmd, creationNodeMask, visibilityNodeMask, &pDlssgFeature, pParams, &dlssgParams );
    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS-G: NGX_VK_CREATE_DLSSG failed: {} (0x{:08X})",
                        GetNgxResultString( r ),
                        static_cast< uint32_t >( r ) );
        pDlssgFeature = nullptr;
        return false;
    }

    return true;
}

bool RTGL1::DLSS::EvaluateDlssg( VkCommandBuffer cmd,
                                 VkImage         backbuffer,
                                 VkImage         outputInterp,
                                 VkImage         depth,
                                 VkImageView     depthView,
                                 VkImage         mvecs,
                                 VkImageView     mvecsView,
                                 VkImage         hudless,
                                 VkImageView     hudlessView,
                                 uint32_t        upscaledWidth,
                                 uint32_t        upscaledHeight,
                                 uint32_t        renderWidth,
                                 uint32_t        renderHeight,
                                 VkFormat        backBufferFormat,
                                 const float*    pView,
                                 float           cameraNear,
                                 float           cameraFar,
                                 float           fovVerticalRad,
                                 bool            reset )
{
    if( !IsDlssgAvailable() || pDlssgFeature == nullptr )
    {
        return false;
    }

    VkImageSubresourceRange colorRange = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    VkImageView backbufferView = GetOrCreateView( backbuffer, backBufferFormat, VK_IMAGE_ASPECT_COLOR_BIT );
    VkImageView outputInterpView = GetOrCreateView( outputInterp, backBufferFormat, VK_IMAGE_ASPECT_COLOR_BIT );

    NVSDK_NGX_Resource_VK backbufferRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        backbufferView, backbuffer, colorRange, backBufferFormat, upscaledWidth, upscaledHeight, false );
    NVSDK_NGX_Resource_VK outputInterpRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        outputInterpView, outputInterp, colorRange, backBufferFormat, upscaledWidth, upscaledHeight, true );
    NVSDK_NGX_Resource_VK depthRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        depthView, depth, colorRange, VK_FORMAT_R32_SFLOAT, renderWidth, renderHeight, false );
    NVSDK_NGX_Resource_VK mvecsRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        mvecsView, mvecs, colorRange, VK_FORMAT_R16G16_SFLOAT, renderWidth, renderHeight, false );
    NVSDK_NGX_Resource_VK hudlessRes = ( hudless != VK_NULL_HANDLE && hudlessView != VK_NULL_HANDLE ) ? NVSDK_NGX_Create_ImageView_Resource_VK(
        hudlessView, hudless, colorRange, backBufferFormat, upscaledWidth, upscaledHeight, false ) : NVSDK_NGX_Resource_VK{};

    NVSDK_NGX_VK_DLSSG_Eval_Params evalParams = {
        .pBackbuffer        = &backbufferRes,
        .pDepth             = &depthRes,
        .pMVecs             = &mvecsRes,
        .pHudless           = ( hudless != VK_NULL_HANDLE && hudlessView != VK_NULL_HANDLE ) ? &hudlessRes : nullptr,
        .pOutputInterpFrame = &outputInterpRes,
    };

    NVSDK_NGX_DLSSG_Opt_Eval_Params optParams = {};
    optParams.multiFrameCount       = 1;
    optParams.multiFrameIndex       = 1;
    optParams.cameraMotionIncluded  = 1;
    optParams.reset                 = reset ? 1 : 0;
    optParams.mvecScale[ 0 ]        = float( renderWidth );
    optParams.mvecScale[ 1 ]        = float( renderHeight );
    optParams.cameraNear            = cameraNear;
    optParams.cameraFar             = cameraFar;
    optParams.cameraFOV             = fovVerticalRad;
    optParams.cameraAspectRatio     = float( upscaledWidth ) / float( upscaledHeight );
    optParams.mvecsSubrectSize      = { renderWidth, renderHeight };
    optParams.depthSubrectSize      = { renderWidth, renderHeight };
    optParams.backbufferSubrectSize = { upscaledWidth, upscaledHeight };
    optParams.outputInterpSubrectSize = { upscaledWidth, upscaledHeight };
    if( hudless != VK_NULL_HANDLE && hudlessView != VK_NULL_HANDLE )
    {
        optParams.hudLessSubrectSize = { upscaledWidth, upscaledHeight };
    }

    if( pView != nullptr )
    {
        optParams.cameraRight[ 0 ]   = pView[ 0 ];
        optParams.cameraRight[ 1 ]   = pView[ 4 ];
        optParams.cameraRight[ 2 ]   = pView[ 8 ];
        optParams.cameraUp[ 0 ]      = pView[ 1 ];
        optParams.cameraUp[ 1 ]      = pView[ 5 ];
        optParams.cameraUp[ 2 ]      = pView[ 9 ];
        optParams.cameraFwd[ 0 ]     = -pView[ 2 ];
        optParams.cameraFwd[ 1 ]     = -pView[ 6 ];
        optParams.cameraFwd[ 2 ]     = -pView[ 10 ];

        const float tx = pView[ 12 ];
        const float ty = pView[ 13 ];
        const float tz = pView[ 14 ];
        optParams.cameraPos[ 0 ]     = -( pView[ 0 ] * tx + pView[ 1 ] * ty + pView[ 2 ] * tz );
        optParams.cameraPos[ 1 ]     = -( pView[ 4 ] * tx + pView[ 5 ] * ty + pView[ 6 ] * tz );
        optParams.cameraPos[ 2 ]     = -( pView[ 8 ] * tx + pView[ 9 ] * ty + pView[ 10 ] * tz );
    }

    NVSDK_NGX_Result r = NGX_VK_EVALUATE_DLSSG(
        cmd, pDlssgFeature, pParams, &evalParams, &optParams );
    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS-G: NGX_VK_EVALUATE_DLSSG failed: {} (0x{:08X})",
                        GetNgxResultString( r ),
                        static_cast< uint32_t >( r ) );
        return false;
    }

    return true;
}

static NVSDK_NGX_Resource_VK ToNGXResource(
    const std::shared_ptr< RTGL1::Framebuffers >& framebuffers,
    uint32_t                                      frameIndex,
    RTGL1::FramebufferImageIndex                  imageIndex,
    NVSDK_NGX_Dimensions                          size,
    bool                                          withWriteAccess = false )
{
    auto [ image, view, format ] = framebuffers->GetImageHandles( imageIndex, frameIndex );

    VkImageSubresourceRange subresourceRange = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    return NVSDK_NGX_Create_ImageView_Resource_VK(
        view, image, subresourceRange, format, size.Width, size.Height, withWriteAccess );
}

RTGL1::FramebufferImageIndex RTGL1::DLSS::Apply(
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
    if( !IsDlssAvailable() )
    {
        throw RgException( RG_RESULT_WRONG_FUNCTION_ARGUMENT,
                           "Nvidia DLSS is not supported (or DLSS dynamic library files are not "
                           "found). Check availability before usage." );
    }


    ValidateDlssFeature( cmd, renderResolution );


    if( pDlssFeature == nullptr )
    {
        throw RgException(
            RG_RESULT_GRAPHICS_API_ERROR,
            "Internal error of Nvidia DLSS: NGX_VULKAN_CREATE_DLSS_EXT has failed." );
    }


    using FI             = FramebufferImageIndex;
    const FI outputImage = FI::FB_IMAGE_INDEX_UPSCALED_PONG;


    CmdLabel label( cmd, "DLSS" );


    FI fs[] = {
        FI::FB_IMAGE_INDEX_FINAL,
        FI::FB_IMAGE_INDEX_MOTION_DLSS,
        FI::FB_IMAGE_INDEX_DEPTH_NDC,
    };
    framebuffers->BarrierMultiple( cmd, frameIndex, fs, Framebuffers::BarrierType::Storage );


    NVSDK_NGX_Coordinates sourceOffset = { 0, 0 };
    NVSDK_NGX_Dimensions  sourceSize   = {
        renderResolution.Width(),
        renderResolution.Height(),
    };
    NVSDK_NGX_Dimensions targetSize = {
        renderResolution.UpscaledWidth(),
        renderResolution.UpscaledHeight(),
    };


    // clang-format off
    NVSDK_NGX_Resource_VK unresolvedColorResource = ToNGXResource( framebuffers, frameIndex, FI::FB_IMAGE_INDEX_FINAL, sourceSize );
    NVSDK_NGX_Resource_VK resolvedColorResource   = ToNGXResource( framebuffers, frameIndex, outputImage, targetSize, true );
    NVSDK_NGX_Resource_VK motionVectorsResource   = ToNGXResource( framebuffers, frameIndex, FI::FB_IMAGE_INDEX_MOTION_DLSS, sourceSize );
    NVSDK_NGX_Resource_VK depthResource           = ToNGXResource( framebuffers, frameIndex, FI::FB_IMAGE_INDEX_DEPTH_NDC, sourceSize );
    NVSDK_NGX_Resource_VK rayLengthResource       = ToNGXResource( framebuffers, frameIndex, FI::FB_IMAGE_INDEX_DEPTH_WORLD, sourceSize );
    // clang-format on


    NVSDK_NGX_VK_DLSS_Eval_Params evalParams = {
        .Feature  = { .pInColor = &unresolvedColorResource, .pInOutput = &resolvedColorResource },
        .pInDepth = &depthResource,
        .pInMotionVectors          = &motionVectorsResource,
        .InJitterOffsetX           = jitterOffset.data[ 0 ] * ( -1 ),
        .InJitterOffsetY           = jitterOffset.data[ 1 ] * ( -1 ),
        .InRenderSubrectDimensions = sourceSize,
        .InReset                   = resetAccumulation ? 1 : 0,
        .InMVScaleX                = float( sourceSize.Width ),
        .InMVScaleY                = float( sourceSize.Height ),
        .InColorSubrectBase        = sourceOffset,
        .InDepthSubrectBase        = sourceOffset,
        .InMVSubrectBase           = sourceOffset,
        .InTranslucencySubrectBase = sourceOffset,
        .InFrameTimeDeltaInMsec    = float( timeDelta * 1000.0 ),
        .pInRayTracingHitDistance  = &rayLengthResource,
    };

    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT( cmd, pDlssFeature, pParams, &evalParams );

    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS: NGX_VULKAN_EVALUATE_DLSS_EXT failed: {} (0x{:08X})",
                        GetNgxResultString( r ),
                        static_cast< uint32_t >( r ) );
    }

    return outputImage;
}

void RTGL1::DLSS::GetOptimalSettings( uint32_t               userWidth,
                                      uint32_t               userHeight,
                                      RgRenderResolutionMode mode,
                                      uint32_t*              pOutWidth,
                                      uint32_t*              pOutHeight ) const
{
    *pOutWidth  = userWidth;
    *pOutHeight = userHeight;

    if( mode == RG_RENDER_RESOLUTION_MODE_CUSTOM )
    {
        return;
    }

    if( !isInitialized || pParams == nullptr )
    {
        return;
    }

    uint32_t minWidth, minHeight, maxWidth, maxHeight;
    float    dummy;

    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS( pParams,
                                                        userWidth,
                                                        userHeight,
                                                        ToNGXPerfQuality( mode ),
                                                        pOutWidth,
                                                        pOutHeight,
                                                        &maxWidth,
                                                        &maxHeight,
                                                        &minWidth,
                                                        &minHeight,
                                                        &dummy );
    if( NVSDK_NGX_FAILED( r ) )
    {
        debug::Warning( "DLSS: NGX_DLSS_GET_OPTIMAL_SETTINGS failed: {} (0x{:08X})",
                        GetNgxResultString( r ),
                        static_cast< uint32_t >( r ) );
    }
}

std::vector< const char* > RTGL1::DLSS::GetDlssVulkanInstanceExtensions()
{
    uint32_t     instanceExtCount;
    const char** ppInstanceExts;
    uint32_t     deviceExtCount;
    const char** ppDeviceExts;

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(
        &instanceExtCount, &ppInstanceExts, &deviceExtCount, &ppDeviceExts );
    if( !NVSDK_NGX_SUCCEED( r ) )
    {
        debug::Warning( "DLSS: NVSDK_NGX_VULKAN_RequiredExtensions failed: {} (0x{:08X})",
                        GetNgxResultString( r ),
                        static_cast< uint32_t >( r ) );
    }

    std::vector< const char* > v;

    for( uint32_t i = 0; i < instanceExtCount; i++ )
    {
        v.push_back( ppInstanceExts[ i ] );
    }

    return v;
}

std::vector< const char* > RTGL1::DLSS::GetDlssVulkanDeviceExtensions()
{
    uint32_t     instanceExtCount;
    const char** ppInstanceExts;
    uint32_t     deviceExtCount;
    const char** ppDeviceExts;

    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_RequiredExtensions(
        &instanceExtCount, &ppInstanceExts, &deviceExtCount, &ppDeviceExts );
    if( !NVSDK_NGX_SUCCEED( r ) )
    {
        debug::Warning( "DLSS: NVSDK_NGX_VULKAN_RequiredExtensions failed: {} (0x{:08X})",
                        GetNgxResultString( r ),
                        static_cast< uint32_t >( r ) );
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


RTGL1::DLSS::DLSS( VkInstance       _instance,
                   VkDevice         _device,
                   VkPhysicalDevice _physDevice,
                   const char*      _pAppGuid,
                   bool             _enableDebug )
    : device( _device )
    , isInitialized( false )
    , pParams( nullptr )
    , pDlssFeature( nullptr )
    , pDlssgFeature( nullptr )
    , prevDlssFeatureValues{}
    , prevDlssgFeatureValues{}
{
    debug::Warning( "DLSS: Library was built without DLSS support (RG_WITH_NVIDIA_DLSS was OFF)" );
}
RTGL1::DLSS::~DLSS() {}

RTGL1::FramebufferImageIndex RTGL1::DLSS::Apply(
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
    throw RgException(
        RG_RESULT_WRONG_FUNCTION_ARGUMENT,
        "RTGL1 was built without DLSS support. Enable RG_WITH_NVIDIA_DLSS CMake option." );
}

void RTGL1::DLSS::GetOptimalSettings( uint32_t               userWidth,
                                      uint32_t               userHeight,
                                      RgRenderResolutionMode mode,
                                      uint32_t*              pOutWidth,
                                      uint32_t*              pOutHeight ) const
{
    *pOutWidth  = userWidth;
    *pOutHeight = userHeight;
}

bool RTGL1::DLSS::IsDlssAvailable() const
{
    return false;
}

bool RTGL1::DLSS::IsDlssgAvailable() const
{
    return false;
}

void RTGL1::DLSS::DestroyDlssgFeature() {}

bool RTGL1::DLSS::ValidateDlssgFeature( VkCommandBuffer, uint32_t, uint32_t, VkFormat )
{
    return false;
}

VkImageView RTGL1::DLSS::GetOrCreateView( VkImage, VkFormat, VkImageAspectFlags )
{
    return VK_NULL_HANDLE;
}

bool RTGL1::DLSS::EvaluateDlssg( VkCommandBuffer, VkImage, VkImage, VkImage, VkImageView, VkImage, VkImageView, VkImage, VkImageView, uint32_t, uint32_t, uint32_t, uint32_t, VkFormat, const float*, float, float, float, bool )
{
    return false;
}

std::vector< const char* > RTGL1::DLSS::GetDlssVulkanInstanceExtensions()
{
    return {};
}
std::vector< const char* > RTGL1::DLSS::GetDlssVulkanDeviceExtensions()
{
    return {};
}


// private
bool RTGL1::DLSS::TryInit( VkInstance       instance,
                           VkDevice         device,
                           VkPhysicalDevice physDevice,
                           const char*      pAppGuid,
                           bool             enableDebug )
{
    return false;
}
bool RTGL1::DLSS::CheckSupport() const
{
    return false;
}
void RTGL1::DLSS::Destroy() {}
void RTGL1::DLSS::DestroyDlssFeature() {}
bool RTGL1::DLSS::AreSameDlssFeatureValues( const RenderResolutionHelper& renderResolution ) const
{
    return false;
}
void RTGL1::DLSS::SaveDlssFeatureValues( const RenderResolutionHelper& renderResolution ) {}
bool RTGL1::DLSS::ValidateDlssFeature( VkCommandBuffer               cmd,
                                       const RenderResolutionHelper& renderResolution )
{
    return false;
}


#endif // RG_USE_NVIDIA_DLSS

RgFloat2D RTGL1::DLSS::GetJitter( const ResolutionState& resolutionState, uint32_t frameId ) const
{
    return HaltonSequence::GetJitter_Halton23( frameId );
}

bool RTGL1::DLSS::IsAvailable() const
{
    return IsDlssAvailable();
}

void RTGL1::DLSS::OnFramebuffersSizeChange( const ResolutionState& resolutionState ) {}
