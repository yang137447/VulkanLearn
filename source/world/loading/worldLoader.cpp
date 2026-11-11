#include "world/loading/worldLoader.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "scene/validation/sceneAssetValidator.h"
#include "commonFunction.h"
#include "script/scriptBlueprintAsset.h"
#include "world/world.h"
#include "world/loading/worldGameplayPlanBuilder.h"

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
    const nlohmann::json& extraObjects,
    const nlohmann::json& sceneDocument,
    const std::unordered_map<std::string, nlohmann::json>&
        particleDocuments) const
{
    try
    {
        WorldBuildPlan buildPlan;
        buildPlan.scenePath = scenePath;
        buildPlan.sceneJson = sceneDocument.is_null() ? LoadSceneJson(scenePath) : sceneDocument;

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

        buildPlan.authoredSceneJson = buildPlan.sceneJson;
        // 资产只在隔离准备中解析一次；网格计划与托管实例消费完全相同的有效声明。
        buildPlan.sceneJson = ResolveScriptBlueprintScene(CommonFunction::GetResourcePath(), buildPlan.sceneJson, scenePath);
        buildPlan.sceneAssetPlan = SceneAssetValidator::BuildLoadPlan(
            scenePath, buildPlan.sceneJson, particleDocuments);
        SceneAssetValidator::Validate(buildPlan.sceneAssetPlan);
        // 场景级 typed Data Channel：value 语法与 Console/C# 共用
        // ParticleDataChannelValue::ParseJson，类型不再退化为 float map。
        if (buildPlan.sceneJson.contains("particleDataChannels"))
        {
            const nlohmann::json& channels =
                buildPlan.sceneJson.at("particleDataChannels");
            if (!channels.is_object())
            {
                throw std::runtime_error(
                    "Scene particleDataChannels must be an object");
            }
            if (channels.size() > kMaxParticleDataChannels)
            {
                throw std::runtime_error(
                    "Scene particleDataChannels exceeds 64 entries");
            }
            for (auto channel = channels.begin();
                 channel != channels.end(); ++channel)
            {
                std::string error;
                if (!ValidateParticleDataChannelName(channel.key(), error))
                {
                    throw std::runtime_error(
                        "Scene particleDataChannels: " + error);
                }
                ParticleDataChannelValue value;
                if (!ParticleDataChannelValue::ParseJson(
                        channel.value(), value, error))
                {
                    throw std::runtime_error(
                        "Scene particleDataChannels['" + channel.key() +
                        "']: " + error);
                }
                buildPlan.particleDataChannels.emplace(
                    channel.key(), value);
            }
        }
        // gameplay 计划只依赖已校验的场景数据；材质 key 由渲染器资源加载阶段
        // 填充，WorldLoader 不再知道任何渲染类型。
        auto gameplayPlans = WorldGameplayPlanBuilder().Build(
            buildPlan.sceneAssetPlan, buildPlan.sceneJson, scenePath);
        if (gameplayPlans.IsFailure())
        {
            return RuntimeResult<WorldBuildPlan>::Failure(gameplayPlans.Error());
        }
        buildPlan.gameplay = std::move(gameplayPlans.Value());
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
