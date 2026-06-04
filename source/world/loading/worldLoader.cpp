#include "world/loading/worldLoader.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "scene/validation/sceneAssetValidator.h"

namespace VL
{

namespace
{

bool ContainsText(std::string_view value, std::string_view token)
{
    return value.find(token) != std::string_view::npos;
}

std::string ClassifyWorldLoaderError(std::string_view message)
{
    if (ContainsText(message, "Mesh ") ||
        ContainsText(message, "mesh ") ||
        ContainsText(message, "model") ||
        ContainsText(message, "model importer") ||
        ContainsText(message, "modelPath=") ||
        ContainsText(message, ".obj") ||
        ContainsText(message, ".fbx") ||
        ContainsText(message, ".srt"))
    {
        return "Mesh.LoadFailed";
    }

    return "Scene.LoadFailed";
}

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
        const std::string errorMessage = exception.what();
        return RuntimeResult<WorldBuildPlan>::Failure(MakeRuntimeError(
            ClassifyWorldLoaderError(errorMessage),
            errorMessage,
            scenePath));
    }
}

} // namespace VL
