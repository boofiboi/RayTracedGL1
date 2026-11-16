#pragma once

#ifdef _WIN32

#ifndef _WINDOWS
    #define _WINDOWS
#endif

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <ffx_api.h>
#include <ffx_api_loader.h>

namespace RTGL1
{

inline std::wstring GetFfxExeFolderPath()
{
    wchar_t appPath[ MAX_PATH ]{};
    DWORD   len = GetModuleFileNameW( nullptr, appPath, MAX_PATH );
    if( len == 0 || len >= MAX_PATH )
    {
        return {};
    }
    std::wstring path( appPath );
    size_t       pos = path.find_last_of( L"\\/" );
    return pos == std::wstring::npos ? std::wstring{} : path.substr( 0, pos );
}

inline std::wstring GetFfxDllFolderPath()
{
    HMODULE hModule = nullptr;
    if( GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast< LPCWSTR >( &GetFfxExeFolderPath ),
            &hModule ) )
    {
        wchar_t modPath[ MAX_PATH ]{};
        DWORD   len = GetModuleFileNameW( hModule, modPath, MAX_PATH );
        if( len > 0 && len < MAX_PATH )
        {
            std::wstring path( modPath );
            size_t       pos = path.find_last_of( L"\\/" );
            if( pos != std::wstring::npos )
            {
                return path.substr( 0, pos );
            }
        }
    }
    return {};
}

inline bool LoadFfxApiFunctions( const wchar_t* siblingDll, HMODULE& outModule, ffxFunctions& outFuncs )
{
    outModule = nullptr;
    outFuncs  = {};

    std::vector< std::wstring > searchDirs;
    auto addDir = [ &searchDirs ]( const std::wstring& dir ) {
        if( !dir.empty() && std::find( searchDirs.begin(), searchDirs.end(), dir ) == searchDirs.end() )
        {
            searchDirs.push_back( dir );
        }
    };

    addDir( GetFfxDllFolderPath() );
    std::wstring exeDir = GetFfxExeFolderPath();
    if( !exeDir.empty() )
    {
        addDir( exeDir );
        addDir( exeDir + L"\\rt_bin" );
    }

    for( const auto& dir : searchDirs )
    {
        std::error_code       ec;
        std::filesystem::path loaderPath  = std::filesystem::path( dir ) / L"amd_fidelityfx_loader_vk.dll";
        std::filesystem::path siblingPath = std::filesystem::path( dir ) / siblingDll;
        if( std::filesystem::exists( loaderPath, ec ) && std::filesystem::exists( siblingPath, ec ) )
        {
            HMODULE mod = LoadLibraryExW(
                loaderPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH );
            if( mod )
            {
                outModule = mod;
                break;
            }
        }
    }

    if( !outModule )
    {
        outModule = LoadLibraryW( L"amd_fidelityfx_loader_vk.dll" );
    }

    if( !outModule )
    {
        return false;
    }

    ffxLoadFunctions( &outFuncs, outModule );
    if( !outFuncs.CreateContext || !outFuncs.DestroyContext ||
        !outFuncs.Configure || !outFuncs.Query || !outFuncs.Dispatch )
    {
        FreeLibrary( outModule );
        outModule = nullptr;
        outFuncs  = {};
        return false;
    }

    return true;
}

}

#endif
