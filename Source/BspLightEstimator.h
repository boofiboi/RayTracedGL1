#pragma once

#include "Common.h"
#include "TextureMeta.h"

#include <filesystem>
#include <vector>

namespace RTGL1
{

class BspLightEstimator
{
public:
    static std::vector< GenericLight > EstimateLights(
        const std::filesystem::path&       bspPath,
        std::string_view                   mapName,
        const std::filesystem::path&       scenesFolder,
        const TextureMetaManager&          textureMeta,
        const std::vector< GenericLight >& existingLights,
        float                              oneGameUnitInMeters );
};

}
