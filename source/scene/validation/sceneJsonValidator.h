#pragma once

#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <nlohmann/json.hpp>
#include "render/camera/cameraExposure.h"
#include "render/lighting/physicalLighting.h"
#include "world/environmentType.h"
#include "script/scriptScene.h"

namespace VL
{
inline void ValidateSceneEnvironmentJson(const nlohmann::json& object)
{
    if (object.contains("hdrPath") && !object.at("hdrPath").is_string())
        throw std::invalid_argument("Environment hdrPath must be string");
    if (object.contains("cubeSize") &&
        (!object.at("cubeSize").is_number_integer() || object.at("cubeSize").get<double>() <= 0))
        throw std::invalid_argument("Environment cubeSize must be a positive integer");
    if (!object.contains("skyParameters")) return;
    const auto& sky = object.at("skyParameters");
    if (!sky.is_object()) throw std::invalid_argument("skyParameters must be object");
    for (const char* field : {"sunColor", "zenithColor", "horizonColor", "groundColor"})
    {
        if (!sky.contains(field) || !sky.at(field).is_array() || sky.at(field).size() != 3)
            throw std::invalid_argument(std::string("Sky requires vec3: ") + field);
        for (const auto& value : sky.at(field))
            if (!value.is_number() || !std::isfinite(value.get<float>()))
                throw std::invalid_argument(std::string("Invalid sky vector: ") + field);
    }
    for (const char* field : {"sunIntensity", "sunAngularRadius", "skyGradientExponent",
        "groundGradientExponent", "sunHaloExponent", "sunHaloStrength"})
        if (!sky.contains(field) || !sky.at(field).is_number() || !std::isfinite(sky.at(field).get<float>()))
            throw std::invalid_argument(std::string("Sky requires finite number: ") + field);
}

// loader 与资产草稿共用的作者数据校验；不读模型、不创建 World 或 GPU 资源。
inline void ValidateSceneAssetJson(const nlohmann::json& scene)
{
    ValidateWorldUnit(scene);
    // description 说明场景验证的论文/知识点；标签 name 不能代替用途说明。
    if (!scene.is_object() || !scene.contains("description") ||
        !scene.at("description").is_string() || scene.at("description").get<std::string>().empty() ||
        !scene.contains("objects") || !scene.at("objects").is_array())
        throw std::invalid_argument("Scene requires non-empty description and objects array");
    if (scene.contains("spawnAssets"))
    {
        const auto& paths = scene.at("spawnAssets");
        if (!paths.is_array() || paths.size() > 128)
            throw std::invalid_argument("spawnAssets requires at most 128 paths");
        for (const auto& path : paths)
            if (!path.is_string() || path.get<std::string>().empty())
                throw std::invalid_argument("spawnAssets requires non-empty asset paths");
    }
    bool hasCamera = false;
    std::set<std::string> identities;
    std::set<std::string> actorIds;
    for (const auto& object : scene.at("objects"))
    {
        if (!object.is_object()) throw std::invalid_argument("Scene object must be an object");
        for (const char* field : {"name", "type"})
            if (!object.contains(field) || !object.at(field).is_string() ||
                object.at(field).get<std::string>().empty())
                throw std::invalid_argument(std::string("Scene object requires string: ") + field);
        const std::string type = object.at("type"), name = object.at("name");
        if (name.rfind("__blueprint/", 0) == 0)
            throw std::invalid_argument("Scene object name uses reserved Actor mesh prefix");
        if (type == "blueprintInstance")
        {
            ValidateScriptBlueprint(object);
            if (!actorIds.insert(object.at("id").get<std::string>()).second)
                throw std::invalid_argument("Duplicate Actor id");
        }
        if (type == "environment")
        {
            // 正式 WorldBuilder 消费的是嵌套 environment，不能只校验外层对象而漏掉实际作者数据。
            const auto& environment = object.at("environment");
            if (!environment.is_object()) throw std::invalid_argument("environment must be object");
            const auto environmentType = ParseEnvironmentType(environment.at("type").get<std::string>());
            ValidateSceneEnvironmentJson(environment);
            if (environment.contains("intensity"))
                ReadPhysicalNumber(environment, "intensity", 0, std::numeric_limits<float>::max());
            if (environmentType == EnvironmentType::Hdri &&
                (!environment.contains("hdrPath") || environment.at("hdrPath").get<std::string>().empty()))
                throw std::invalid_argument("HDRI requires hdrPath");
        }
        if (!identities.insert(type + "|" + (type == "blueprintInstance"
            ? object.at("id").get<std::string>() : name)).second)
            throw std::invalid_argument("Scene object identity is ambiguous: " + type + "|" + name);
        if (type != "mesh" && type != "directionalLight" && type != "pointLight" &&
            type != "spotLight" && type != "squareAreaLight" && type != "camera" &&
            type != "environment" && type != "blueprintInstance" && type != "balloon" &&
            type != "glassPane")
            throw std::invalid_argument("Unknown scene object type: " + type);
        for (const char* field : {"position", "rotation", "scale", "normal", "color", "look_at"})
        {
            const bool required = (std::string(field) == "position" && type != "environment") ||
                (std::string(field) == "rotation" && type != "environment" && type != "squareAreaLight") ||
                (std::string(field) == "scale" && (type == "mesh" || type == "camera" ||
                    type == "blueprintInstance" || type == "balloon" || type == "glassPane")) ||
                (std::string(field) == "normal" && type == "squareAreaLight");
            if (!required && !object.contains(field)) continue;
            if (!object.contains(field) || !object.at(field).is_array() || object.at(field).size() != 3)
                throw std::invalid_argument(type + "|" + name + " requires vec3: " + field);
            double lengthSquared = 0;
            for (const auto& component : object.at(field))
            {
                if (!component.is_number() || !std::isfinite(component.get<double>()) ||
                    !std::isfinite(static_cast<float>(component.get<double>())))
                    throw std::invalid_argument(std::string("Invalid scene vector: ") + field);
                const double number = component.get<double>();
                if (std::string(field) == "scale" && std::abs(number) < 1.0e-6)
                    throw std::invalid_argument("Scene scale must be non-zero");
                lengthSquared += number * number;
            }
            if (std::string(field) == "normal" && lengthSquared <= 1.0e-12)
                throw std::invalid_argument("Area light normal must be non-zero");
        }
        if (type == "mesh")
        {
            for (const char* field : {"modelPath", "materialOverride"})
            {
                if (std::string(field) == "materialOverride" && !object.contains(field)) continue;
                if (!object.contains(field) || !object.at(field).is_string() ||
                    object.at(field).get<std::string>().empty())
                    throw std::invalid_argument(std::string("Mesh requires path: ") + field);
            }
        }
        if (type == "balloon")
        {
            if (!object.contains("materialInstance") ||
                !object.at("materialInstance").is_string() ||
                object.at("materialInstance").get<std::string>().empty())
                throw std::invalid_argument("Balloon requires materialInstance path");
            const double radius = ReadPhysicalNumber(object, "radius", 0, 1.0e6);
            const double thickness = ReadPhysicalNumber(object, "thickness", 0, radius);
            const double pressure = ReadPhysicalNumber(object, "pressure", 0, 1.0e6);
            const double burstStrain = ReadPhysicalNumber(object, "burstStrain", 0, 10.0);
            const double restContraction = ReadPhysicalNumber(object, "restContraction", 0, 1.0);
            if (radius <= 0.0 || thickness <= 0.0 || thickness >= radius)
                throw std::invalid_argument("Balloon requires 0 < thickness < radius");
            if (pressure < 0.0 || burstStrain <= 0.0 || restContraction <= 0.0)
                throw std::invalid_argument("Balloon pressure/strain/contraction must be positive");
            if (!object.contains("subdivisions") || !object.at("subdivisions").is_number_integer() ||
                object.at("subdivisions").get<int>() < 0 ||
                object.at("subdivisions").get<int>() > 3)
                throw std::invalid_argument("Balloon subdivisions must be in [0, 3]");
            if (!object.contains("siteCount") || !object.at("siteCount").is_number_integer() ||
                object.at("siteCount").get<int>() < 4 ||
                object.at("siteCount").get<int>() > 64)
                throw std::invalid_argument("Balloon siteCount must be in [4, 64]");
            if (!object.contains("seed") || !object.at("seed").is_number_integer() ||
                object.at("seed").get<long long>() < 0)
                throw std::invalid_argument("Balloon requires non-negative integer seed");
        }
        if (type == "glassPane")
        {
            if (!object.contains("materialInstance") ||
                !object.at("materialInstance").is_string() ||
                object.at("materialInstance").get<std::string>().empty())
                throw std::invalid_argument("Glass pane requires materialInstance path");
            const double width = ReadPhysicalNumber(object, "width", 0, 1.0e6);
            const double height = ReadPhysicalNumber(object, "height", 0, 1.0e6);
            const double thickness = ReadPhysicalNumber(
                object, "thickness", 0, std::min(width, height));
            if (width <= 0.0 || height <= 0.0)
                throw std::invalid_argument("Glass pane requires positive width and height");
            if (thickness <= 0.0 || thickness >= std::min(width, height))
                throw std::invalid_argument(
                    "Glass pane requires 0 < thickness < min(width, height)");
            if (!object.contains("siteCount") || !object.at("siteCount").is_number_integer() ||
                object.at("siteCount").get<int>() < 4 ||
                object.at("siteCount").get<int>() > 64)
                throw std::invalid_argument("Glass pane siteCount must be in [4, 64]");
            if (!object.contains("seed") || !object.at("seed").is_number_integer() ||
                object.at("seed").get<long long>() < 0)
                throw std::invalid_argument("Glass pane requires non-negative integer seed");
            if (object.contains("subfracture") && !object.at("subfracture").is_boolean())
                throw std::invalid_argument("Glass pane subfracture must be boolean");
        }
        if (type == "directionalLight" || type == "pointLight" ||
            type == "spotLight" || type == "squareAreaLight") ReadCanonicalLightIntensity(object);
        if (type == "camera")
        {
            hasCamera = true;
            ReadCameraExposure(object);
            // 按消费端 float 校验，double 中不同的裁剪面不能在发布后舍入成同一个值。
            const float fov = static_cast<float>(ReadPhysicalNumber(object, "fov", 0, 179));
            const float nearClip = static_cast<float>(ReadPhysicalNumber(object, "near_clip", 0,
                std::numeric_limits<float>::max()));
            const float farClip = static_cast<float>(ReadPhysicalNumber(object, "far_clip", 0,
                std::numeric_limits<float>::max()));
            if (fov <= 0 || fov >= 179 || nearClip <= 0 || farClip <= nearClip)
                throw std::invalid_argument("Camera requires 0 < FOV < 179 and 0 < near < far");
            if (object.contains("look_at") && object.at("look_at") == object.at("position"))
                throw std::invalid_argument("Camera look_at must differ from position");
        }
    }
    if (!hasCamera) throw std::invalid_argument("Scene has no camera");
}
} // namespace VL
