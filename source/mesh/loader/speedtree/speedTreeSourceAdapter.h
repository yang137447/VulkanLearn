#pragma once

#include <string>
#include <vector>
#include "../../meshAssetTypes.h"

struct SpeedTreeSourceData
{
    std::vector<std::string> materialNames;
    ModelResource modelResource;
};

// Adapter boundary for reading SpeedTree source data from .stsdk or a future official SDK wrapper.
// It translates external SpeedTree concepts into importer-owned intermediate data; it does not
// expose SDK types to RendererMeshLoader, MeshAssetValidator, or rendering code.
class SpeedTreeSourceAdapter
{
public:
    void ValidateSource(const std::string& sourcePath, const std::string& modelDataPath) const;
    SpeedTreeSourceData ReadSource(const std::string& sourcePath, const std::string& modelDataPath) const;
};
