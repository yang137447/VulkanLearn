#pragma once

#include "engine/runtimeCommandExecutor.h"
#include "renderSystem.h"
#include "world/actorIdentity.h"
#include "world/world.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace VL
{
namespace
{
const char* ToString(BloomParameter parameter)
{
    switch (parameter)
    {
    case BloomParameter::Strength:
        return "strength";
    case BloomParameter::Threshold:
        return "threshold";
    case BloomParameter::Knee:
        return "knee";
    case BloomParameter::Clamp:
        return "clamp";
    }

    return "unknown";
}

std::string FormatVector3(const Eigen::Vector3f& value)
{
    return "(" + std::to_string(value.x()) + ", " +
        std::to_string(value.y()) + ", " +
        std::to_string(value.z()) + ")";
}

std::string FormatColorTint(const Eigen::Vector4f& value)
{
    return "(" + std::to_string(value.x()) + ", " +
        std::to_string(value.y()) + ", " +
        std::to_string(value.z()) + ", " +
        std::to_string(value.w()) + ")";
}

// 可见性写入端：把"大纲身份 → World 对象"这一步收在一个地方，
// 让命令分支只说类型语义，不重复 World 的表结构。返回 false 表示对象不在当前 World。
bool ApplyActorVisibility(
    World& world,
    const EditorSceneActorIdentity& identity,
    bool visible)
{
    switch (identity.type)
    {
    case EditorSceneActorType::Mesh:
        return world.SetMeshSceneObjectVisible(identity.name, visible);
    case EditorSceneActorType::Terrain:
        // 地形在 World 里仍是 chunk mesh；同名 Mesh/Terrain 必须按类型分开写入。
        return world.SetTerrainSceneObjectVisible(identity.name, visible);
    case EditorSceneActorType::DirectionalLight:
    case EditorSceneActorType::PointLight:
    case EditorSceneActorType::SpotLight:
        return world.SetLightVisible(identity.name, visible);
    case EditorSceneActorType::SquareAreaLight:
        return world.SetSquareAreaLightVisible(identity.name, visible);
    case EditorSceneActorType::ParticleSystem:
        return world.SetParticleSystemVisible(identity.name, visible);
    // 相机 / 环境 / 场景风没有渲染侧可见性；蓝图实例的可见性属于脚本状态
    //（destroyed）。调用方已用 SceneActorHasVisibilityToggle() 挡住，这里不提供任何
    // 默认成功路径。
    case EditorSceneActorType::Camera:
    case EditorSceneActorType::Environment:
    case EditorSceneActorType::SceneWind:
    case EditorSceneActorType::Script:
        return false;
    }
    return false;
}

std::string ResolveScreenshotPath(
    const RuntimeCommand& command,
    const RuntimeConfig& runtimeConfig)
{
    std::filesystem::path requestedPath(command.stringValue);
    if (requestedPath.empty())
    {
        requestedPath = "hair_debug.bmp";
    }
    if (requestedPath.extension().empty())
    {
        requestedPath += ".bmp";
    }
    else if (requestedPath.extension() != ".bmp" &&
             requestedPath.extension() != ".BMP")
    {
        requestedPath.replace_extension(".bmp");
    }
    if (requestedPath.is_absolute())
    {
        return requestedPath.lexically_normal().string();
    }

    return (
        std::filesystem::path(runtimeConfig.GetResourcePath()) /
        "Generated" /
        "Screenshots" /
        requestedPath).lexically_normal().string();
}

template <typename T>
std::unordered_map<std::string, std::uintptr_t> CaptureSharedResourcePointers(
    const std::unordered_map<std::string, std::shared_ptr<T>>& resources)
{
    std::unordered_map<std::string, std::uintptr_t> pointers;
    pointers.reserve(resources.size());
    for (const auto& [key, resource] : resources)
    {
        pointers.emplace(key, reinterpret_cast<std::uintptr_t>(resource.get()));
    }
    return pointers;
}

double RoundSceneFloat(float value)
{
    return std::round(
        static_cast<double>(value) * 10000.0) /
        10000.0;
}

nlohmann::ordered_json BuildDirectionalLightShadowJson(
    const CsmSettings& settings)
{
    nlohmann::ordered_json shadowJson =
        nlohmann::ordered_json::object();
    shadowJson["castShadows"] = settings.castShadows;
    shadowJson["dynamicShadowDistance"] =
        RoundSceneFloat(settings.dynamicShadowDistance);
    shadowJson["dynamicShadowCascades"] =
        settings.cascadeCount;
    shadowJson["cascadeDistributionExponent"] =
        RoundSceneFloat(settings.cascadeDistributionExponent);
    shadowJson["cascadeTransitionFraction"] =
        RoundSceneFloat(settings.cascadeTransitionFraction);
    shadowJson["shadowDistanceFadeoutFraction"] =
        RoundSceneFloat(settings.shadowDistanceFadeoutFraction);
    shadowJson["shadowBias"] =
        RoundSceneFloat(settings.shadowBias);
    shadowJson["shadowSlopeBias"] =
        RoundSceneFloat(settings.shadowSlopeBias);
    shadowJson["shadowCascadeBiasDistribution"] =
        RoundSceneFloat(
            settings.shadowCascadeBiasDistribution);
    return shadowJson;
}

nlohmann::ordered_json* FindPrimaryDirectionalLight(
    nlohmann::ordered_json& sceneJson)
{
    if (!sceneJson.contains("objects") ||
        !sceneJson["objects"].is_array())
    {
        return nullptr;
    }

    for (nlohmann::ordered_json& objectJson :
         sceneJson["objects"])
    {
        if (objectJson.is_object() &&
            objectJson.value("type", std::string()) ==
                "directionalLight")
        {
            return &objectJson;
        }
    }
    return nullptr;
}
} // namespace
} // namespace VL