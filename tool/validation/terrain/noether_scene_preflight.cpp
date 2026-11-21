#include "world/loading/worldLoader.h"

#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    const auto result = VL::WorldLoader().Load(argv[1]);
    nlohmann::json report{{"gpuExecuted", false}, {"scene", argv[1]},
        {"nativeWorldLoaderPassed", result.IsSuccess()}};
    if (result.IsFailure())
        report["error"] = VL::FormatRuntimeError(result.Error());
    else
    {
        size_t terrains = 0;
        for (const auto& object : result.Value().sceneAssetPlan.objectPlans)
            terrains += object.terrainBuildPlan.has_value();
        report["objects"] = result.Value().sceneAssetPlan.objectPlans.size();
        report["preparedTerrains"] = terrains;
    }
    std::ofstream(argv[2]) << report.dump(2);
    std::cout << report.dump(2) << std::endl;
    return result.IsSuccess() ? 0 : 1;
}
