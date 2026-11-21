#pragma once

#include <string>
#include <vector>

namespace VL
{
struct TerrainCompositedAuthorDependency
{
    std::string root;
    std::string path;
    std::string mode;
    std::string digest;
    std::vector<std::string> productionObjectIds;
};
}
