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
    return Load(scenePath, nlohmann::json::array());
}

RuntimeResult<WorldBuildPlan> WorldLoader::Load(
    const std::string& scenePath,
    const nlohmann::json& extraObjects) const
{
    try
    {
        WorldBuildPlan buildPlan;
        buildPlan.scenePath = scenePath;
        buildPlan.sceneJson = LoadSceneJson(scenePath);

        // 附加对象在**校验之前**并入 objects，因此它们走的是与场景作者写的对象
        // 完全相同的路径。这里刻意不做"部分容错"：形状不对就直接抛，
        // 由下面的 catch 统一变成加载失败 —— 静默丢掉一个注入对象会让模型编辑器
        // 显示一个空场景，而那是很难从现象反推原因的。
        if (extraObjects.is_array() && !extraObjects.empty())
        {
            if (!buildPlan.sceneJson.contains("objects") ||
                !buildPlan.sceneJson["objects"].is_array())
            {
                throw std::runtime_error(
                    "Cannot inject scene objects: the scene has no \"objects\" array");
            }
            for (const auto& object : extraObjects)
            {
                if (!object.is_object())
                {
                    throw std::runtime_error(
                        "Cannot inject scene object: every entry must be a JSON object");
                }
                buildPlan.sceneJson["objects"].push_back(object);
            }
        }

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
