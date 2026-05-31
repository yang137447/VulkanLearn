#include "world/loading/worldLoader.h"

#include <exception>
#include <filesystem>
#include <fstream>

#include "scene/validation/sceneAssetValidator.h"

namespace VL
{

namespace
{

nlohmann::json LoadSceneJson(const std::string& scenePath)
{
    if (!std::filesystem::exists(scenePath))
    {
        throw std::runtime_error("Scene file not found: " + scenePath);
    }

    std::ifstream file(scenePath);
    if (!file.is_open())
    {
        throw std::runtime_error("Scene file not found: " + scenePath);
    }

    try
    {
        return nlohmann::json::parse(file);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Scene JSON parse failed: " + scenePath +
            " error=" + exception.what());
    }
}

} // namespace

RuntimeResult<WorldBuildPlan> WorldLoader::Load(const std::string& scenePath) const
{
    try
    {
        WorldBuildPlan buildPlan;
        buildPlan.scenePath = scenePath;
        buildPlan.sceneJson = LoadSceneJson(scenePath);
        buildPlan.sceneAssetPlan = SceneAssetValidator::BuildLoadPlan(scenePath, buildPlan.sceneJson);
        SceneAssetValidator::Validate(buildPlan.sceneAssetPlan);
        return RuntimeResult<WorldBuildPlan>::Success(std::move(buildPlan));
    }
    catch (const std::exception& exception)
    {
        return RuntimeResult<WorldBuildPlan>::Failure(MakeRuntimeError(
            "WorldLoader.LoadFailed",
            exception.what(),
            scenePath));
    }
}

} // namespace VL
