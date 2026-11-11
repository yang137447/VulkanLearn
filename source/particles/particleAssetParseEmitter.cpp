#include "particles/particleAssetInternal.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "particles/particleBeam.h"
#include "particles/particleFlipbook.h"

namespace VL
{
namespace ParticleAssetDetail
{
ParticleEmitterAsset ParseEmitter(
    const nlohmann::json& json,
    size_t index,
    bool implicit,
    const std::filesystem::path& resourceRoot,
    const ParticleCollisionResourceCache* collisionCache)
{
    if (!json.is_object())
    {
        throw std::invalid_argument(
            "Particle emitter must be a JSON object: " +
            std::to_string(index));
    }

    ParticleEmitterAsset emitter;
    const std::string defaultName =
        "emitter" + std::to_string(index);
    // implicit 发射器的 name 属于系统；旧 PS 根对象的 name 不能被当成
    // 发射器名，否则场景对象改名会悄悄改变运行时 emitter 身份。
    emitter.name = implicit
        ? defaultName
        : json.value("name", defaultName);
    if (!implicit)
    {
        emitter.localPosition = ReadVec3(
            json, "position", emitter.localPosition);
    }
    emitter.localSpace = json.value("localSpace", emitter.localSpace);
    emitter.renderMode = json.value(
        "renderMode", emitter.renderMode);
    if (json.contains("billboardAlignment"))
    {
        if (!json.at("billboardAlignment").is_string())
        {
            throw std::invalid_argument(
                "Particle billboardAlignment must be a string");
        }
        emitter.billboardAlignment =
            json.at("billboardAlignment").get<std::string>();
    }
    // fixed 模式的发射器局部基；其它 alignment 也接受作者化数值，
    // 但渲染端只在 billboardAlignment == "fixed" 时消费。
    emitter.billboardNormal = ReadVec3(
        json, "billboardNormal", emitter.billboardNormal);
    emitter.billboardUp = ReadVec3(
        json, "billboardUp", emitter.billboardUp);
    emitter.billboardShadows = json.value(
        "billboardShadows", emitter.billboardShadows);
    emitter.billboardLighting = json.value(
        "billboardLighting", emitter.billboardLighting);
    emitter.billboardAmbient = ReadFiniteFloat(
        json, "billboardAmbient", emitter.billboardAmbient);
    emitter.billboardLightingStrength = ReadFiniteFloat(
        json,
        "billboardLightingStrength",
        emitter.billboardLightingStrength);
    emitter.billboardSpecular = ReadFiniteFloat(
        json,
        "billboardSpecular",
        emitter.billboardSpecular);
    emitter.billboardRoughness = ReadFiniteFloat(
        json,
        "billboardRoughness",
        emitter.billboardRoughness);
    emitter.billboardForwardScattering = ReadFiniteFloat(
        json,
        "billboardForwardScattering",
        emitter.billboardForwardScattering);
    emitter.billboardDensity = ReadFiniteFloat(
        json,
        "billboardDensity",
        emitter.billboardDensity);
    if (emitter.renderMode != "billboard" &&
        emitter.renderMode != "ribbon" &&
        emitter.renderMode != "mesh" &&
        emitter.renderMode != "skeletalMeshTris" &&
        emitter.renderMode != "skeletalMeshBones" &&
        emitter.renderMode != "beam" &&
        emitter.renderMode != "distortion" &&
        emitter.renderMode != "light" &&
        emitter.renderMode != "decal")
    {
        throw std::invalid_argument(
            "Particle renderMode must be 'billboard', 'ribbon', 'mesh', "
            "'skeletalMeshTris', 'skeletalMeshBones', "
            "'beam', 'distortion', 'light' or 'decal'");
    }
    if (json.contains("curveInterpolation"))
    {
        if (!json.at("curveInterpolation").is_string())
        {
            throw std::invalid_argument(
                "Particle curveInterpolation must be a string");
        }
        if (!ParseParticleCurveInterpolation(
                json.at("curveInterpolation").get<std::string>(),
                emitter.curveInterpolation))
        {
            throw std::invalid_argument(
                "Particle curveInterpolation must be 'linear', "
                "'hermite' or 'bezier'");
        }
    }
    emitter.ribbonWidth = ReadFiniteFloat(
        json, "ribbonWidth", emitter.ribbonWidth);
    emitter.ribbonSegments = ReadCountField(
        json, "ribbonSegments", emitter.ribbonSegments);
    emitter.ribbonFade = ReadFiniteFloat(
        json, "ribbonFade", emitter.ribbonFade);
    emitter.ribbonUVTiling = ReadFiniteFloat(
        json, "ribbonUVTiling", emitter.ribbonUVTiling);
    emitter.ribbonPivot = ReadFiniteFloat(
        json, "ribbonPivot", emitter.ribbonPivot);
    emitter.ribbonTwist = ReadFiniteFloat(
        json, "ribbonTwist", emitter.ribbonTwist);
    if (json.contains("ribbonWidthKeys"))
    {
        const nlohmann::json& widthKeys = json.at("ribbonWidthKeys");
        if (!widthKeys.is_array())
        {
            throw std::invalid_argument(
                "Particle ribbonWidthKeys requires an array");
        }
        if (widthKeys.size() > kMaxParticleCurveKeys)
        {
            throw std::invalid_argument(
                "Particle ribbonWidthKeys allows at most 8 keys");
        }
        emitter.ribbonWidthKeys.clear();
        emitter.ribbonWidthKeys.reserve(widthKeys.size());
        float previousTime = -1.0f;
        for (const nlohmann::json& entry : widthKeys)
        {
            if (!entry.is_object())
            {
                throw std::invalid_argument(
                    "Particle ribbonWidthKeys entry must be an object");
            }
            ParticleRibbonWidthKey key;
            key.time = ReadFiniteFloat(entry, "time", 0.0f);
            key.width = ReadFiniteFloat(entry, "width", 1.0f);
            key.tangentIn = ReadFiniteFloat(
                entry, "tangentIn", 0.0f);
            key.tangentOut = ReadFiniteFloat(
                entry, "tangentOut", 0.0f);
            if (key.time < 0.0f || key.time > 1.0f ||
                key.time <= previousTime)
            {
                throw std::invalid_argument(
                    "Particle ribbonWidthKeys times must be in [0,1] "
                    "and strictly increasing");
            }
            if (key.width < 0.0f || key.width > 8.0f)
            {
                throw std::invalid_argument(
                    "Particle ribbonWidthKeys width must be in [0,8]");
            }
            previousTime = key.time;
            emitter.ribbonWidthKeys.push_back(key);
        }
    }
    if (json.contains("ribbonColorKeys"))
    {
        const nlohmann::json& colorKeys = json.at("ribbonColorKeys");
        if (!colorKeys.is_array() ||
            colorKeys.size() > kMaxParticleCurveKeys)
        {
            throw std::invalid_argument(
                "Particle ribbonColorKeys allows at most 8 keys");
        }
        emitter.ribbonColorKeys.clear();
        float previousTime = -1.0f;
        for (const nlohmann::json& entry : colorKeys)
        {
            if (!entry.is_object())
            {
                throw std::invalid_argument(
                    "Particle ribbonColorKeys entry must be an object");
            }
            ParticleRibbonColorKey key;
            key.time = ReadFiniteFloat(entry, "time", 0.0f);
            key.color = ReadVec4(entry, "color", key.color);
            key.tangentIn = ReadVec4Tangent(entry, "tangentIn");
            key.tangentOut = ReadVec4Tangent(entry, "tangentOut");
            if (key.time < 0.0f || key.time > 1.0f ||
                key.time <= previousTime)
            {
                throw std::invalid_argument(
                    "Particle ribbonColorKeys times must be in [0,1] "
                    "and strictly increasing");
            }
            previousTime = key.time;
            emitter.ribbonColorKeys.push_back(key);
        }
    }
    if (json.contains("beam"))
    {
        const nlohmann::json& beam = json.at("beam");
        if (!beam.is_object())
        {
            throw std::invalid_argument(
                "Particle beam must be an object");
        }
        emitter.beam.start = ReadVec3(
            beam, "start", emitter.beam.start);
        emitter.beam.end = ReadVec3(
            beam, "end", emitter.beam.end);
        emitter.beam.segments = ReadCountField(
            beam, "segments", emitter.beam.segments);
        emitter.beam.width = ReadFiniteFloat(
            beam, "width", emitter.beam.width);
        emitter.beam.count = ReadCountField(
            beam, "count", emitter.beam.count);
        emitter.beam.noiseAmplitude = ReadFiniteFloat(
            beam, "noiseAmplitude", emitter.beam.noiseAmplitude);
        emitter.beam.noiseFrequency = ReadFiniteFloat(
            beam, "noiseFrequency", emitter.beam.noiseFrequency);
        emitter.beam.noiseSpeed = ReadFiniteFloat(
            beam, "noiseSpeed", emitter.beam.noiseSpeed);
        emitter.beam.endpointJitter = ReadFiniteFloat(
            beam, "endpointJitter", emitter.beam.endpointJitter);
        emitter.beam.branchCount = ReadCountField(
            beam, "branchCount", emitter.beam.branchCount);
        emitter.beam.branchStart = ReadFiniteFloat(
            beam, "branchStart", emitter.beam.branchStart);
        emitter.beam.branchLength = ReadFiniteFloat(
            beam, "branchLength", emitter.beam.branchLength);
        emitter.beam.branchSpread = ReadFiniteFloat(
            beam, "branchSpread", emitter.beam.branchSpread);
        if (beam.contains("colorKeys"))
        {
            const nlohmann::json& keys = beam.at("colorKeys");
            if (!keys.is_array() || keys.empty() ||
                keys.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    "Particle beam.colorKeys requires 1..8 entries");
            }
            for (const nlohmann::json& keyJson : keys)
            {
                if (!keyJson.is_object())
                {
                    throw std::invalid_argument(
                        "Particle beam color key must be an object");
                }
                ParticleBeamColorKey key;
                key.time = ReadFiniteFloat(
                    keyJson, "time", key.time);
                key.color = ReadVec4(
                    keyJson, "color", key.color);
                key.tangentIn = ReadVec4Tangent(
                    keyJson, "tangentIn");
                key.tangentOut = ReadVec4Tangent(
                    keyJson, "tangentOut");
                emitter.beam.colorKeys.push_back(key);
            }
        }
        if (beam.contains("widthKeys"))
        {
            const nlohmann::json& keys = beam.at("widthKeys");
            if (!keys.is_array() || keys.empty() ||
                keys.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    "Particle beam.widthKeys requires 1..8 entries");
            }
            for (const nlohmann::json& keyJson : keys)
            {
                if (!keyJson.is_object())
                {
                    throw std::invalid_argument(
                        "Particle beam width key must be an object");
                }
                ParticleBeamWidthKey key;
                key.time = ReadFiniteFloat(
                    keyJson, "time", key.time);
                key.width = ReadFiniteFloat(
                    keyJson, "width", key.width);
                key.tangentIn = ReadFiniteFloat(
                    keyJson, "tangentIn", 0.0f);
                key.tangentOut = ReadFiniteFloat(
                    keyJson, "tangentOut", 0.0f);
                emitter.beam.widthKeys.push_back(key);
            }
        }
        if (beam.contains("arcs"))
        {
            const nlohmann::json& arcs = beam.at("arcs");
            if (!arcs.is_array() || arcs.empty() ||
                arcs.size() > kMaxParticleBeamCount)
            {
                throw std::invalid_argument(
                    "Particle beam.arcs requires 1..16 entries");
            }
            for (const nlohmann::json& arcJson : arcs)
            {
                if (!arcJson.is_object())
                {
                    throw std::invalid_argument(
                        "Particle beam arc must be an object");
                }
                ParticleBeamArc arc;
                arc.colorStart = ReadVec3(
                    arcJson, "colorStart", arc.colorStart);
                arc.colorEnd = ReadVec3(
                    arcJson, "colorEnd", arc.colorEnd);
                arc.widthScale = ReadFiniteFloat(
                    arcJson, "widthScale", arc.widthScale);
                arc.intensity = ReadFiniteFloat(
                    arcJson, "intensity", arc.intensity);
                arc.noisePhase = ReadFiniteFloat(
                    arcJson, "noisePhase", arc.noisePhase);
                emitter.beam.arcs.push_back(arc);
            }
        }
        if (beam.contains("hitScan"))
        {
            const nlohmann::json& hitScan = beam.at("hitScan");
            if (!hitScan.is_object())
            {
                throw std::invalid_argument(
                    "Particle beam.hitScan must be an object");
            }
            if (hitScan.contains("enabled"))
            {
                if (!hitScan.at("enabled").is_boolean())
                {
                    throw std::invalid_argument(
                        "Particle beam.hitScan.enabled must be boolean");
                }
                emitter.beam.hitScan.enabled =
                    hitScan.at("enabled").get<bool>();
            }
            else
            {
                emitter.beam.hitScan.enabled = true;
            }
            emitter.beam.hitScan.planeNormal = ReadVec3(
                hitScan, "planeNormal",
                emitter.beam.hitScan.planeNormal);
            emitter.beam.hitScan.planeOffset = ReadFiniteFloat(
                hitScan, "planeOffset",
                emitter.beam.hitScan.planeOffset);
            emitter.beam.hitScan.maxDistance = ReadFiniteFloat(
                hitScan, "maxDistance",
                emitter.beam.hitScan.maxDistance);
        }
    }
    if (json.contains("light"))
    {
        const nlohmann::json& light = json.at("light");
        if (!light.is_object())
        {
            throw std::invalid_argument(
                "Particle light must be an object");
        }
        emitter.hasLight = true;
        emitter.light.intensity = ReadFiniteFloat(
            light, "intensity", emitter.light.intensity);
        emitter.light.radius = ReadFiniteFloat(
            light, "radius", emitter.light.radius);
        emitter.light.castShadows = light.value(
            "castShadows", emitter.light.castShadows);
        emitter.light.shadowBias = ReadFiniteFloat(
            light, "shadowBias", emitter.light.shadowBias);
        // iesProfile 与 iesTexture 是同一种逐像素角度剖面的两个名字；
        // 同时出现时拒绝，避免作者以为后者会覆盖前者。
        if (light.contains("iesProfile") && light.contains("iesTexture"))
        {
            throw std::invalid_argument(
                "Particle light must not declare both iesProfile and "
                "iesTexture");
        }
        const char* iesField = light.contains("iesTexture")
            ? "iesTexture"
            : (light.contains("iesProfile") ? "iesProfile" : nullptr);
        if (iesField != nullptr)
        {
            const nlohmann::json& ies = light.at(iesField);
            if (!ies.is_array() || ies.size() > 64)
            {
                throw std::invalid_argument(
                    std::string("Particle light.") + iesField +
                    " allows at most 64 samples");
            }
            std::vector<float>& samples = light.contains("iesTexture")
                ? emitter.light.iesTexture
                : emitter.light.iesProfile;
            samples.clear();
            for (const nlohmann::json& sample : ies)
            {
                if (!sample.is_number())
                {
                    throw std::invalid_argument(
                        std::string("Particle light.") + iesField +
                        " samples must be numbers");
                }
                const float value = sample.get<float>();
                if (!std::isfinite(value) || value < 0.0f || value > 64.0f)
                {
                    throw std::invalid_argument(
                        std::string("Particle light.") + iesField +
                        " samples must be finite and in [0,64]");
                }
                samples.push_back(value);
            }
            // 两个名字共用同一份数据，运行时只看 iesProfile。
            if (!emitter.light.iesTexture.empty())
            {
                emitter.light.iesProfile = emitter.light.iesTexture;
            }
        }
        if (light.contains("brightnessKeys"))
        {
            const nlohmann::json& keys = light.at("brightnessKeys");
            if (!keys.is_array() || keys.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    "Particle light.brightnessKeys allows at most 8 keys");
            }
            emitter.light.brightnessKeys.clear();
            float previousTime = -1.0f;
            for (const nlohmann::json& entry : keys)
            {
                if (!entry.is_object())
                {
                    throw std::invalid_argument(
                        "Particle light.brightnessKeys entry must be an "
                        "object");
                }
                ParticleLightBrightnessKey key;
                key.time = ReadFiniteFloat(entry, "time", 0.0f);
                // 兼容只写 brightness 的简写：同一 key 也接受 "value"。
                key.brightness = ReadFiniteFloat(
                    entry,
                    entry.contains("brightness") ? "brightness" : "value",
                    key.brightness);
                key.color = ReadVec3(entry, "color", key.color);
                if (key.time < 0.0f || key.time > 1.0f ||
                    key.time <= previousTime)
                {
                    throw std::invalid_argument(
                        "Particle light.brightnessKeys times must be in "
                        "[0,1] and strictly increasing");
                }
                if (key.brightness < 0.0f || key.brightness > 64.0f)
                {
                    throw std::invalid_argument(
                        "Particle light.brightnessKeys brightness must be "
                        "in [0,64]");
                }
                previousTime = key.time;
                emitter.light.brightnessKeys.push_back(key);
            }
            // 没有逐寿命颜色时，颜色乘数保持白，亮度曲线只缩放功率。
        }
    }
    if (json.contains("decal"))
    {
        const nlohmann::json& decal = json.at("decal");
        if (!decal.is_object())
        {
            throw std::invalid_argument(
                "Particle decal must be an object");
        }
        emitter.hasDecal = true;
        emitter.decal.size = ReadVec3(
            decal, "size", emitter.decal.size);
        emitter.decal.rotation = ReadVec3(
            decal, "rotation", emitter.decal.rotation);
        emitter.decal.color = ReadVec3(
            decal, "color", emitter.decal.color);
        emitter.decal.opacity = ReadFiniteFloat(
            decal, "opacity", emitter.decal.opacity);
        emitter.decal.edgeSoftness = ReadFiniteFloat(
            decal, "edgeSoftness", emitter.decal.edgeSoftness);
        emitter.decal.noiseStrength = ReadFiniteFloat(
            decal, "noiseStrength", emitter.decal.noiseStrength);
        emitter.decal.roughness = ReadFiniteFloat(
            decal, "roughness", emitter.decal.roughness);
        emitter.decal.metallic = ReadFiniteFloat(
            decal, "metallic", emitter.decal.metallic);
        emitter.decal.ambientOcclusion = ReadFiniteFloat(
            decal, "ambientOcclusion", emitter.decal.ambientOcclusion);
        emitter.decal.normalStrength = ReadFiniteFloat(
            decal, "normalStrength", emitter.decal.normalStrength);
        emitter.decal.emissive = ReadVec3(
            decal, "emissive", emitter.decal.emissive);
        emitter.decal.emissiveStrength = ReadFiniteFloat(
            decal, "emissiveStrength", emitter.decal.emissiveStrength);
        if (decal.contains("decalResponse"))
        {
            if (!decal.at("decalResponse").is_string())
            {
                throw std::invalid_argument(
                    "Particle decal.decalResponse must be a string");
            }
            const std::string response =
                decal.at("decalResponse").get<std::string>();
            if (!TryParseDecalResponse(
                    response, emitter.decal.responseMask))
            {
                throw std::invalid_argument(
                    "Particle decal has unknown decalResponse: " +
                    response);
            }
        }
        for (const char* field :
            {"albedoMap", "normalMap", "ormMap"})
        {
            if (!decal.contains(field))
            {
                continue;
            }
            if (!decal.at(field).is_string())
            {
                throw std::invalid_argument(
                    std::string("Particle decal.") + field +
                    " must be a string");
            }
            const std::string value =
                decal.at(field).get<std::string>();
            if (field == std::string("albedoMap"))
                emitter.decal.albedoMap = value;
            else if (field == std::string("normalMap"))
                emitter.decal.normalMap = value;
            else
                emitter.decal.ormMap = value;
        }
        emitter.decal.sortOrder = ReadCountField(
            decal, "sortOrder", emitter.decal.sortOrder);
    }
    if (json.contains("mesh"))
    {
        const nlohmann::json& mesh = json.at("mesh");
        if (!mesh.is_object())
        {
            throw std::invalid_argument(
                "Particle mesh must be an object");
        }
        if (mesh.contains("path"))
        {
            if (!mesh.at("path").is_string())
            {
                throw std::invalid_argument(
                    "Particle mesh.path must be a string");
            }
            emitter.meshPath = mesh.at("path").get<std::string>();
        }
        emitter.meshScale = ReadVec3(
            mesh, "scale", emitter.meshScale);
        emitter.meshUseSizeCurve = mesh.value(
            "useSizeCurve", emitter.meshUseSizeCurve);
        emitter.meshSizeScale = ReadFiniteFloat(
            mesh, "sizeScale", emitter.meshSizeScale);
        if (mesh.contains("alignment"))
        {
            if (!mesh.at("alignment").is_string())
            {
                throw std::invalid_argument(
                    "Particle mesh.alignment must be a string");
            }
            emitter.meshAlignment =
                mesh.at("alignment").get<std::string>();
        }
        if (mesh.contains("materialInstancePath"))
        {
            if (!mesh.at("materialInstancePath").is_string())
            {
                throw std::invalid_argument(
                    "Particle mesh.materialInstancePath must be a string");
            }
            emitter.materialInstancePath =
                mesh.at("materialInstancePath").get<std::string>();
        }
    }
    if (json.contains("materialInstancePath"))
    {
        if (!json.at("materialInstancePath").is_string())
        {
            throw std::invalid_argument(
                "Particle materialInstancePath must be a string");
        }
        emitter.materialInstancePath =
            json.at("materialInstancePath").get<std::string>();
    }
    if (json.contains("skeletalPose"))
    {
        const nlohmann::json& pose = json.at("skeletalPose");
        if (!pose.is_object())
        {
            throw std::invalid_argument(
                "Particle skeletalPose must be an object");
        }
        emitter.skeletalPose.clip = pose.value(
            "clip", emitter.skeletalPose.clip);
        emitter.skeletalPose.time = ReadFiniteFloat(
            pose, "time", emitter.skeletalPose.time);
        emitter.skeletalPose.rate = ReadFiniteFloat(
            pose, "rate", emitter.skeletalPose.rate);
        emitter.skeletalPose.loop = pose.value(
            "loop", emitter.skeletalPose.loop);
    }
    emitter.skeletalBoneRadius = ReadFiniteFloat(
        json, "skeletalBoneRadius", emitter.skeletalBoneRadius);
    if (json.contains("spawnSource"))
    {
        const nlohmann::json& source = json.at("spawnSource");
        if (!source.is_object())
        {
            throw std::invalid_argument(
                "Particle spawnSource must be an object");
        }
        emitter.spawnSource.mode = source.value(
            "mode", emitter.spawnSource.mode);
        emitter.spawnSource.parentEmitter = source.value(
            "parentEmitter", emitter.spawnSource.parentEmitter);
        emitter.spawnSource.parentEvent = source.value(
            "parentEvent", emitter.spawnSource.parentEvent);
        emitter.spawnSource.perEvent = ReadCountField(
            source, "perEvent", emitter.spawnSource.perEvent);
        emitter.spawnSource.offset = ReadVec3(
            source, "offset", emitter.spawnSource.offset);
        emitter.spawnSource.normalVelocityScale = ReadFiniteFloat(
            source,
            "normalVelocityScale",
            emitter.spawnSource.normalVelocityScale);
    }
    if (emitter.spawnSource.mode == "none")
    {
        if (emitter.renderMode == "skeletalMeshTris")
        {
            emitter.spawnSource.mode = "skeletalTris";
        }
        else if (emitter.renderMode == "skeletalMeshBones")
        {
            emitter.spawnSource.mode = "skeletalBones";
        }
    }
    if (json.contains("dissolve"))
    {
        const nlohmann::json& dissolve = json.at("dissolve");
        if (!dissolve.is_object())
        {
            throw std::invalid_argument(
                "Particle dissolve must be an object");
        }
        emitter.dissolve.enabled = dissolve.value(
            "enabled", true);
        emitter.dissolve.start = ReadFiniteFloat(
            dissolve, "start", emitter.dissolve.start);
        emitter.dissolve.end = ReadFiniteFloat(
            dissolve, "end", emitter.dissolve.end);
        emitter.dissolve.edgeWidth = ReadFiniteFloat(
            dissolve, "edgeWidth", emitter.dissolve.edgeWidth);
        emitter.dissolve.edgeColor = ReadVec3(
            dissolve, "edgeColor", emitter.dissolve.edgeColor);
        emitter.dissolve.edgeStrength = ReadFiniteFloat(
            dissolve, "edgeStrength", emitter.dissolve.edgeStrength);
        emitter.dissolve.noiseScale = ReadFiniteFloat(
            dissolve, "noiseScale", emitter.dissolve.noiseScale);
        emitter.dissolve.noiseSpeed = ReadFiniteFloat(
            dissolve, "noiseSpeed", emitter.dissolve.noiseSpeed);
    }
    if (json.contains("materialBindings"))
    {
        const nlohmann::json& bindings = json.at("materialBindings");
        if (!bindings.is_object())
        {
            throw std::invalid_argument(
                "Particle materialBindings must be an object");
        }
        for (auto binding = bindings.begin();
             binding != bindings.end(); ++binding)
        {
            if (!binding.value().is_string() ||
                binding.value().get<std::string>().empty())
            {
                throw std::invalid_argument(
                    "Particle materialBindings values must be non-empty strings");
            }
            emitter.materialBindings[binding.key()] =
                binding.value().get<std::string>();
        }
    }
    if (json.contains("attributeBindings"))
    {
        const nlohmann::json& bindings = json.at("attributeBindings");
        if (!bindings.is_object())
        {
            throw std::invalid_argument(
                "Particle attributeBindings must be an object");
        }
        for (auto binding = bindings.begin();
             binding != bindings.end(); ++binding)
        {
            if (!binding.value().is_string() ||
                binding.value().get<std::string>().empty())
            {
                throw std::invalid_argument(
                    "Particle attributeBindings values must be non-empty strings");
            }
            emitter.attributeBindings[binding.key()] =
                binding.value().get<std::string>();
        }
    }
    // mesh 照明与几何参数同属 renderer 层；非 mesh 模式也可以作者化，
    // 但渲染端只在 renderMode=="mesh" 时消费。
    emitter.meshLighting = json.value(
        "meshLighting", emitter.meshLighting);
    emitter.meshAmbient = ReadFiniteFloat(
        json, "meshAmbient", emitter.meshAmbient);
    if (json.contains("castShadows"))
    {
        if (!json.at("castShadows").is_boolean())
        {
            throw std::invalid_argument(
                "Particle castShadows must be a boolean");
        }
        emitter.castShadows =
            json.at("castShadows").get<bool>();
    }
    // 显式 useFixedBounds 优先；省略时只要存在 fixedBounds 对象就启用，
    // 因此作者可以只写包围球数据，也可以写 false 明确关闭。
    emitter.useFixedBounds = json.contains("useFixedBounds")
        ? json.value("useFixedBounds", false)
        : json.contains("fixedBounds");
    if (json.contains("fixedBounds"))
    {
        const nlohmann::json& fixedBounds = json.at("fixedBounds");
        if (!fixedBounds.is_object())
        {
            throw std::invalid_argument(
                "Particle fixedBounds must be an object");
        }
        emitter.fixedBoundsCenter = ReadVec3(
            fixedBounds, "center", emitter.fixedBoundsCenter);
        emitter.fixedBoundsExtent = ReadVec3(
            fixedBounds, "extent", emitter.fixedBoundsExtent);
    }

    emitter.maxParticles = ReadCountField(
        json, "maxParticles", emitter.maxParticles);
    emitter.burst = json.value("burst", emitter.burst);
    emitter.burstInterval = ReadFiniteFloat(
        json, "burstInterval", emitter.burstInterval);
    emitter.burstDelay = ReadFiniteFloat(
        json, "burstDelay", emitter.burstDelay);
    emitter.burstPhase = ReadFiniteFloat(
        json, "burstPhase", emitter.burstPhase);
    emitter.burstCount = ReadCountField(
        json, "burstCount", emitter.burstCount);
    emitter.spawnRate = ReadFiniteFloat(
        json, "spawnRate", emitter.spawnRate);
    emitter.spawnPerUnit = ReadFiniteFloat(
        json, "spawnPerUnit", emitter.spawnPerUnit);
    emitter.drag = ReadFiniteFloat(json, "drag", emitter.drag);
    emitter.softness = ReadFiniteFloat(
        json, "softness", emitter.softness);
    emitter.softParticles =
        json.value("softParticles", emitter.softParticles);
    emitter.softParticleDistance = ReadFiniteFloat(
        json, "softParticleDistance", emitter.softParticleDistance);
    emitter.softParticleTapRadius = ReadFiniteFloat(
        json, "softParticleTapRadius", emitter.softParticleTapRadius);
    emitter.softParticleThickness = ReadFiniteFloat(
        json, "softParticleThickness", emitter.softParticleThickness);
    emitter.sortParticles =
        json.value("sortParticles", emitter.sortParticles);
    // sortOrder 与 sortParticles 一样按展平后的 emitter 根字段解析；它不
    // 改变池内逐粒子排序，只决定整个 emitter（或全局 alpha 池）在透明
    // 绘制序列中的位置。
    emitter.sortOrder = ReadIntField(
        json, "sortOrder", emitter.sortOrder, -1000, 1000);
    emitter.opacity = ReadFiniteFloat(
        json, "opacity", emitter.opacity);
    emitter.distortionStrength = ReadFiniteFloat(
        json, "distortionStrength",
        emitter.distortionStrength);
    emitter.maxDrawDistance = ReadFiniteFloat(
        json, "maxDrawDistance", emitter.maxDrawDistance);
    emitter.fadeDistance = ReadFiniteFloat(
        json, "fadeDistance", emitter.fadeDistance);
    emitter.minScreenSize = ReadFiniteFloat(
        json, "minScreenSize", emitter.minScreenSize);
    emitter.maxScreenSize = ReadFiniteFloat(
        json, "maxScreenSize", emitter.maxScreenSize);
    emitter.lodDistance = ReadFiniteFloat(
        json, "lodDistance", emitter.lodDistance);
    emitter.lodSpawnRateScale = ReadFiniteFloat(
        json, "lodSpawnRateScale", emitter.lodSpawnRateScale);
    if (json.contains("lod"))
    {
        const nlohmann::json& lod = json.at("lod");
        if (!lod.is_object())
        {
            throw std::invalid_argument(
                "Particle lod must be an object");
        }
        if (lod.contains("tiers"))
        {
            const nlohmann::json& tiers = lod.at("tiers");
            if (!tiers.is_array() ||
                tiers.size() > kMaxParticleLodTiers)
            {
                throw std::invalid_argument(
                    "Particle lod.tiers requires an array of at most "
                    "4 entries");
            }
            emitter.lodTiers.reserve(tiers.size());
            for (const nlohmann::json& entry : tiers)
            {
                if (!entry.is_object())
                {
                    throw std::invalid_argument(
                        "Particle lod tier must be an object");
                }
                ParticleLodTier tier;
                tier.distance = ReadFiniteFloat(
                    entry, "distance", tier.distance);
                tier.spawnRateScale = ReadFiniteFloat(
                    entry,
                    "spawnRateScale",
                    tier.spawnRateScale);
                tier.renderScale = ReadFiniteFloat(
                    entry, "renderScale", tier.renderScale);
                tier.fadeRange = ReadFiniteFloat(
                    entry, "fadeRange", tier.fadeRange);
                emitter.lodTiers.push_back(tier);
            }
        }
    }

    if (json.contains("lifetime"))
    {
        const nlohmann::json& lifetime = json.at("lifetime");
        if (!lifetime.is_array() || lifetime.size() != 2)
        {
            throw std::invalid_argument(
                "Particle lifetime requires numeric [min, max]");
        }
        emitter.lifetimeMin = ReadFiniteNumber(
            lifetime.at(0), "lifetime.min");
        emitter.lifetimeMax = ReadFiniteNumber(
            lifetime.at(1), "lifetime.max");
    }

    if (json.contains("lifetimeScaleKeys"))
    {
        const nlohmann::json& keys = json.at("lifetimeScaleKeys");
        if (!keys.is_array() ||
            keys.size() > kMaxParticleCurveKeys)
        {
            throw std::invalid_argument(
                "Particle lifetimeScaleKeys requires 0..8 entries");
        }
        for (const nlohmann::json& keyJson : keys)
        {
            if (!keyJson.is_object())
            {
                throw std::invalid_argument(
                    "Particle lifetime scale key must be an object");
            }
            ParticleLifetimeScaleKey key;
            key.time = ReadFiniteFloat(keyJson, "time", key.time);
            key.scale = ReadFiniteFloat(
                keyJson, "scale", key.scale);
            emitter.lifetimeScaleKeys.push_back(key);
        }
        // 重复 time 合法，稳定排序保留作者书写顺序供编辑器后续处理。
        std::stable_sort(
            emitter.lifetimeScaleKeys.begin(),
            emitter.lifetimeScaleKeys.end(),
            [](const ParticleLifetimeScaleKey& lhs,
               const ParticleLifetimeScaleKey& rhs)
            {
                return lhs.time < rhs.time;
            });
    }

    if (json.contains("size"))
    {
        const nlohmann::json& size = json.at("size");
        if (!size.is_array() || size.size() != 2)
        {
            throw std::invalid_argument(
                "Particle size requires [start, end]");
        }
        emitter.sizeStart = ReadFiniteNumber(size.at(0), "size.start");
        emitter.sizeEnd = ReadFiniteNumber(size.at(1), "size.end");
    }
    emitter.rotation = ReadFiniteFloat(
        json, "rotation", emitter.rotation);
    emitter.rotationSpeed = ReadFiniteFloat(
        json, "rotationSpeed", emitter.rotationSpeed);
    emitter.rotationRandom = json.value(
        "rotationRandom", emitter.rotationRandom);

    if (json.contains("velocity"))
    {
        const nlohmann::json& velocity = json.at("velocity");
        if (!velocity.is_object())
        {
            throw std::invalid_argument(
                "Particle velocity must be an object");
        }
        emitter.velocityMin = ReadVec3(
            velocity, "min", emitter.velocityMin);
        emitter.velocityMax = ReadVec3(
            velocity, "max", emitter.velocityMax);
    }

    if (json.contains("shape"))
    {
        const nlohmann::json& shape = json.at("shape");
        if (!shape.is_object())
        {
            throw std::invalid_argument(
                "Particle shape must be an object");
        }
        if (shape.contains("type") && !shape.at("type").is_string())
        {
            throw std::invalid_argument(
                "Particle shape.type must be a string");
        }
        emitter.shapeType = shape.value("type", emitter.shapeType);
        if (emitter.shapeType != "sphere" &&
            emitter.shapeType != "box" &&
            emitter.shapeType != "cone" &&
            emitter.shapeType != "cylinder" &&
            emitter.shapeType != "disc" &&
            emitter.shapeType != "ring")
        {
            throw std::invalid_argument(
                "Particle shape.type must be 'sphere', 'box', 'cone', "
                "'cylinder', 'disc', or 'ring'");
        }
        // 所有字段都读取，未使用字段仍参与有限性与非负校验，避免资产里
        // 隐藏 NaN/Inf 或负数在切换形状后突然生效。
        emitter.shapeRadius = ReadFiniteFloat(
            shape, "radius", emitter.shapeRadius);
        emitter.shapeExtent = ReadVec3(
            shape, "extent", emitter.shapeExtent);
        emitter.shapeHeight = ReadFiniteFloat(
            shape, "height", emitter.shapeHeight);
        emitter.shapeInnerRadius = ReadFiniteFloat(
            shape, "innerRadius", emitter.shapeInnerRadius);
    }

    if (json.contains("collision"))
    {
        const nlohmann::json& collision = json.at("collision");
        if (!collision.is_object())
        {
            throw std::invalid_argument(
                "Particle collision must be an object");
        }
        emitter.collisionType = collision.value(
            "type", emitter.collisionType);
        if (emitter.collisionType != "none" &&
            emitter.collisionType != "plane" &&
            emitter.collisionType != "box" &&
            emitter.collisionType != "capsule" &&
            emitter.collisionType != "sdf" &&
            emitter.collisionType != "mesh")
        {
            throw std::invalid_argument(
                "Particle collision type must be 'none', 'plane', 'box', "
                "'capsule', 'sdf', or 'mesh'");
        }
        if (collision.contains("plane"))
        {
            const nlohmann::json& plane = collision.at("plane");
            if (!plane.is_object())
            {
                throw std::invalid_argument(
                    "Particle collision plane must be an object");
            }
            emitter.collisionPlaneNormal =
                NormalizeParticleCollisionPlaneNormal(
                    ReadVec3(
                        plane,
                        "normal",
                        emitter.collisionPlaneNormal),
                    "collision.plane.normal");
            emitter.collisionPlaneOffset = ReadFiniteFloat(
                plane,
                "offset",
                emitter.collisionPlaneOffset);
        }
        if (collision.contains("box"))
        {
            const nlohmann::json& box = collision.at("box");
            if (!box.is_object())
            {
                throw std::invalid_argument(
                    "Particle collision box must be an object");
            }
            emitter.collisionBoxMin = ReadVec3(
                box,
                "min",
                emitter.collisionBoxMin);
            emitter.collisionBoxMax = ReadVec3(
                box,
                "max",
                emitter.collisionBoxMax);
        }
        if (collision.contains("capsule"))
        {
            const nlohmann::json& capsule = collision.at("capsule");
            if (!capsule.is_object())
            {
                throw std::invalid_argument(
                    "Particle collision capsule must be an object");
            }
            emitter.collisionCapsuleA = ReadVec3(
                capsule,
                "a",
                emitter.collisionCapsuleA);
            emitter.collisionCapsuleB = ReadVec3(
                capsule,
                "b",
                emitter.collisionCapsuleB);
            emitter.collisionCapsuleRadius = ReadFiniteFloat(
                capsule,
                "radius",
                emitter.collisionCapsuleRadius);
        }
        if (collision.contains("sdf"))
        {
            const nlohmann::json& sdf = collision.at("sdf");
            if (!sdf.is_object())
            {
                throw std::invalid_argument(
                    "Particle collision sdf must be an object");
            }
            if (sdf.contains("path"))
            {
                if (!sdf.at("path").is_string())
                {
                    throw std::invalid_argument(
                        "Particle collision.sdf.path must be a string");
                }
                emitter.collisionSdfPath =
                    sdf.at("path").get<std::string>();
            }
        }
        if (collision.contains("mesh"))
        {
            const nlohmann::json& mesh = collision.at("mesh");
            if (!mesh.is_object())
            {
                throw std::invalid_argument(
                    "Particle collision mesh must be an object");
            }
            if (mesh.contains("path"))
            {
                if (!mesh.at("path").is_string())
                {
                    throw std::invalid_argument(
                        "Particle collision.mesh.path must be a string");
                }
                emitter.collisionMeshPath =
                    mesh.at("path").get<std::string>();
            }
        }
        emitter.collisionBounce = ReadFiniteFloat(
            collision,
            "bounce",
            emitter.collisionBounce);
        emitter.collisionFriction = ReadFiniteFloat(
            collision,
            "friction",
            emitter.collisionFriction);
        emitter.collisionRadius = ReadFiniteFloat(
            collision,
            "radius",
            emitter.collisionRadius);
        emitter.killOnCollision = collision.value(
            "killOnCollision",
            emitter.killOnCollision);
        emitter.collisionCcd = collision.value(
            "ccd",
            emitter.collisionCcd);
        if (emitter.collisionType == "sdf")
        {
            if (emitter.collisionSdfPath.empty())
            {
                throw std::invalid_argument(
                    "Particle collision type 'sdf' requires "
                    "collision.sdf.path");
            }
            if (collisionCache != nullptr)
            {
                const auto cached = collisionCache->find(
                    ParticleCollisionResourceCacheKey(
                        emitter.collisionType,
                        emitter.collisionSdfPath));
                if (cached != collisionCache->end())
                {
                    emitter.collisionSdf = cached->second;
                }
            }
            if (!emitter.collisionSdf)
            {
                emitter.collisionSdf =
                    LoadParticleSdfAsset(
                        emitter.collisionSdfPath, resourceRoot);
            }
        }
        else if (emitter.collisionType == "mesh")
        {
            if (emitter.collisionMeshPath.empty())
            {
                throw std::invalid_argument(
                    "Particle collision type 'mesh' requires "
                    "collision.mesh.path");
            }
            // 网格在提交前烘成 SDF；运行时 GPU 只看到 SDF 类型 4，
            // 不会退回 plane/box 或静默关闭碰撞。
            if (collisionCache != nullptr)
            {
                const auto cached = collisionCache->find(
                    ParticleCollisionResourceCacheKey(
                        emitter.collisionType,
                        emitter.collisionMeshPath));
                if (cached != collisionCache->end())
                {
                    emitter.collisionSdf = cached->second;
                }
            }
            if (!emitter.collisionSdf)
            {
                emitter.collisionSdf =
                    LoadParticleCollisionMeshAsset(
                        emitter.collisionMeshPath,
                        resourceRoot,
                        emitter.collisionRadius);
            }
        }
    }

    if (json.contains("events"))
    {
        const nlohmann::json& events = json.at("events");
        if (!events.is_object())
        {
            throw std::invalid_argument(
                "Particle events must be an object");
        }
        emitter.emitSpawnEvent = events.value(
            "spawn", emitter.emitSpawnEvent);
        emitter.emitDeathEvent = events.value(
            "death", emitter.emitDeathEvent);
        emitter.emitCollisionEvent = events.value(
            "collision", emitter.emitCollisionEvent);
    }
    emitter.randomSeed = ReadCountField(
        json, "randomSeed", emitter.randomSeed);
    emitter.deterministicRandom = json.value(
        "deterministicRandom", emitter.deterministicRandom);
    emitter.fixedTimeStep = ReadFiniteFloat(
        json, "fixedTimeStep", emitter.fixedTimeStep);

    emitter.gravity = ReadVec3(
        json, "gravity", emitter.gravity);
    if (json.contains("curlNoise"))
    {
        const nlohmann::json& curl = json.at("curlNoise");
        if (!curl.is_object())
        {
            throw std::invalid_argument(
                "Particle curlNoise must be an object");
        }
        emitter.curlNoiseStrength = ReadFiniteFloat(
            curl, "strength", emitter.curlNoiseStrength);
        emitter.curlNoiseFrequency = ReadFiniteFloat(
            curl, "frequency", emitter.curlNoiseFrequency);
        emitter.curlNoiseSpeed = ReadFiniteFloat(
            curl, "speed", emitter.curlNoiseSpeed);
    }
    if (json.contains("vortex"))
    {
        const nlohmann::json& vortex = json.at("vortex");
        if (!vortex.is_object())
        {
            throw std::invalid_argument(
                "Particle vortex must be an object");
        }
        emitter.vortexStrength = ReadFiniteFloat(
            vortex, "strength", emitter.vortexStrength);
        emitter.vortexRadius = ReadFiniteFloat(
            vortex, "radius", emitter.vortexRadius);
        emitter.vortexAxis = ReadVec3(
            vortex, "axis", emitter.vortexAxis);
    }
    if (json.contains("attractor"))
    {
        const nlohmann::json& attractor = json.at("attractor");
        if (!attractor.is_object())
        {
            throw std::invalid_argument(
                "Particle attractor must be an object");
        }
        emitter.attractorPosition = ReadVec3(
            attractor, "position", emitter.attractorPosition);
        emitter.attractorStrength = ReadFiniteFloat(
            attractor, "strength", emitter.attractorStrength);
        emitter.attractorRadius = ReadFiniteFloat(
            attractor, "radius", emitter.attractorRadius);
    }
    emitter.wind = ReadVec3(json, "wind", emitter.wind);
    emitter.colorStart = ReadVec4(
        json, "colorStart", emitter.colorStart);
    emitter.colorEnd = ReadVec4(
        json, "colorEnd", emitter.colorEnd);

    if (json.contains("colorKeys"))
    {
        const nlohmann::json& keys = json.at("colorKeys");
        if (!keys.is_array() || keys.empty() ||
            keys.size() > kMaxParticleCurveKeys)
        {
            throw std::invalid_argument(
                "Particle colorKeys requires 1..8 entries");
        }
        for (const nlohmann::json& keyJson : keys)
        {
            if (!keyJson.is_object())
            {
                throw std::invalid_argument(
                    "Particle color key must be an object");
            }
            ParticleColorKey key;
            key.time = ReadFiniteFloat(keyJson, "time", key.time);
            key.color = ReadVec4(
                keyJson, "color", key.color);
            key.tangentIn = ReadVec4Tangent(
                keyJson, "tangentIn");
            key.tangentOut = ReadVec4Tangent(
                keyJson, "tangentOut");
            emitter.colorKeys.push_back(key);
        }
        std::sort(
            emitter.colorKeys.begin(),
            emitter.colorKeys.end(),
            [](const ParticleColorKey& lhs,
               const ParticleColorKey& rhs)
            {
                return lhs.time < rhs.time;
            });
    }

    if (json.contains("sizeKeys"))
    {
        const nlohmann::json& keys = json.at("sizeKeys");
        if (!keys.is_array() || keys.empty() ||
            keys.size() > kMaxParticleCurveKeys)
        {
            throw std::invalid_argument(
                "Particle sizeKeys requires 1..8 entries");
        }
        for (const nlohmann::json& keyJson : keys)
        {
            if (!keyJson.is_object())
            {
                throw std::invalid_argument(
                    "Particle size key must be an object");
            }
            ParticleSizeKey key;
            key.time = ReadFiniteFloat(keyJson, "time", key.time);
            key.size = ReadFiniteFloat(keyJson, "size", key.size);
            key.tangentIn = ReadFiniteFloat(
                keyJson, "tangentIn", 0.0f);
            key.tangentOut = ReadFiniteFloat(
                keyJson, "tangentOut", 0.0f);
            emitter.sizeKeys.push_back(key);
        }
        std::sort(
            emitter.sizeKeys.begin(),
            emitter.sizeKeys.end(),
            [](const ParticleSizeKey& lhs,
               const ParticleSizeKey& rhs)
            {
                return lhs.time < rhs.time;
            });
    }

    emitter.texturePath =
        json.value("texture", emitter.texturePath);
    emitter.eooPacked = json.value(
        "eooPacked", emitter.eooPacked);
    emitter.eooEmissiveStrength = ReadFiniteFloat(
        json, "eooEmissiveStrength", emitter.eooEmissiveStrength);
    if (json.contains("flipbook"))
    {
        const nlohmann::json& flipbook = json.at("flipbook");
        if (!flipbook.is_object())
        {
            throw std::invalid_argument(
                "Particle flipbook must be an object");
        }
        emitter.flipbookColumns = ReadCountField(
            flipbook, "columns", emitter.flipbookColumns);
        emitter.flipbookRows = ReadCountField(
            flipbook, "rows", emitter.flipbookRows);
        emitter.flipbookFramesPerSecond = ReadFiniteFloat(
            flipbook, "fps", emitter.flipbookFramesPerSecond);
        emitter.flipbookStartFrame = ReadFrameIndex(
            flipbook, "startFrame", emitter.flipbookStartFrame);
        // 只有作者显式写出 endFrame 时才把它当作真实帧号；
        // 省略时解析为 atlas 最后一帧，而不是把 0 永远当哨兵。
        const uint32_t frameCount =
            emitter.flipbookColumns * emitter.flipbookRows;
        emitter.flipbookEndFrame = flipbook.contains("endFrame")
            ? ReadFrameIndex(flipbook, "endFrame", 0)
            : (frameCount > 0 ? frameCount - 1 : 0);
        emitter.flipbookLoop = flipbook.value(
            "loop", emitter.flipbookLoop);
        emitter.flipbookRandomStartFrame = flipbook.value(
            "randomStartFrame",
            emitter.flipbookRandomStartFrame);
        emitter.flipbookPerParticle = flipbook.value(
            "perParticle",
            emitter.flipbookPerParticle);
        emitter.flipbookBlend = flipbook.value(
            "blend",
            emitter.flipbookBlend);
        emitter.flipbookPlayRate = ReadFiniteFloat(
            flipbook,
            "playRate",
            emitter.flipbookPlayRate);
        emitter.flipbookStartFrameJitter = ReadFiniteFloat(
            flipbook,
            "startFrameJitter",
            emitter.flipbookStartFrameJitter);
        auto readUvCurve = [&](const char* field,
                               std::vector<ParticleFlipbookUvKey>& keys)
        {
            if (!flipbook.contains(field))
            {
                return;
            }
            const nlohmann::json& curve = flipbook.at(field);
            if (!curve.is_array() ||
                curve.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    std::string("Particle flipbook.") + field +
                    " allows at most 8 keys");
            }
            keys.clear();
            float previousTime = -1.0f;
            for (const nlohmann::json& entry : curve)
            {
                if (!entry.is_object())
                {
                    throw std::invalid_argument(
                        std::string("Particle flipbook.") + field +
                        " entry must be an object");
                }
                ParticleFlipbookUvKey key;
                key.time = ReadFiniteFloat(entry, "time", 0.0f);
                const nlohmann::json& value = entry.at("value");
                if (!value.is_array() || value.size() != 2)
                {
                    throw std::invalid_argument(
                        std::string("Particle flipbook.") + field +
                        " value requires [x,y]");
                }
                key.value = Eigen::Vector2f(
                    ReadFiniteNumber(value.at(0), "value.x"),
                    ReadFiniteNumber(value.at(1), "value.y"));
                if (key.time < 0.0f || key.time > 1.0f ||
                    key.time <= previousTime)
                {
                    throw std::invalid_argument(
                        std::string("Particle flipbook.") + field +
                        " times must be in [0,1] and strictly increasing");
                }
                previousTime = key.time;
                keys.push_back(key);
            }
        };
        readUvCurve("uvOffsetKeys", emitter.flipbookUvOffsetKeys);
        readUvCurve("uvScaleKeys", emitter.flipbookUvScaleKeys);
        if (flipbook.contains("playRateKeys"))
        {
            const nlohmann::json& curve = flipbook.at("playRateKeys");
            if (!curve.is_array() ||
                curve.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    "Particle flipbook.playRateKeys allows at most 8 keys");
            }
            emitter.flipbookPlayRateKeys.clear();
            float previousTime = -1.0f;
            for (const nlohmann::json& entry : curve)
            {
                if (!entry.is_object())
                {
                    throw std::invalid_argument(
                        "Particle flipbook.playRateKeys entry must be an object");
                }
                ParticleFlipbookRateKey key;
                key.time = ReadFiniteFloat(entry, "time", 0.0f);
                key.rate = ReadFiniteFloat(entry, "rate", 1.0f);
                if (key.time < 0.0f || key.time > 1.0f ||
                    key.time <= previousTime)
                {
                    throw std::invalid_argument(
                        "Particle flipbook.playRateKeys times must be in "
                        "[0,1] and strictly increasing");
                }
                if (key.rate < 0.0f || key.rate > 16.0f)
                {
                    throw std::invalid_argument(
                        "Particle flipbook.playRateKeys rate must be in [0,16]");
                }
                previousTime = key.time;
                emitter.flipbookPlayRateKeys.push_back(key);
            }
        }

        if (flipbook.contains("framePivotKeys"))
        {
            const nlohmann::json& keys =
                flipbook.at("framePivotKeys");
            if (!keys.is_array() || keys.empty() ||
                keys.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    "Particle flipbook.framePivotKeys requires 1..8 "
                    "entries");
            }
            float previousTime = -1.0f;
            for (const nlohmann::json& entry : keys)
            {
                if (!entry.is_object())
                {
                    throw std::invalid_argument(
                        "Particle flipbook.framePivotKeys entry must be "
                        "an object");
                }
                ParticleFlipbookPivotKey key;
                key.time = ReadFiniteFloat(entry, "time", 0.0f);
                key.pivot = ReadVec2(
                    entry, "pivot", key.pivot);
                key.tangentIn = ReadVec2Tangent(
                    entry, "tangentIn");
                key.tangentOut = ReadVec2Tangent(
                    entry, "tangentOut");
                if (key.time < 0.0f || key.time > 1.0f ||
                    key.time <= previousTime)
                {
                    throw std::invalid_argument(
                        "Particle flipbook.framePivotKeys times must be "
                        "in [0,1] and strictly increasing");
                }
                for (int axis = 0; axis < 2; ++axis)
                {
                    if (key.pivot[axis] < -8.0f ||
                        key.pivot[axis] > 8.0f)
                    {
                        throw std::invalid_argument(
                            "Particle flipbook.framePivotKeys pivot must "
                            "be in [-8,8]");
                    }
                }
                previousTime = key.time;
                emitter.flipbookFramePivotKeys.push_back(key);
            }
        }

        if (flipbook.contains("frameRotationKeys"))
        {
            const nlohmann::json& keys =
                flipbook.at("frameRotationKeys");
            if (!keys.is_array() || keys.empty() ||
                keys.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    "Particle flipbook.frameRotationKeys requires 1..8 "
                    "entries");
            }
            float previousTime = -1.0f;
            for (const nlohmann::json& entry : keys)
            {
                if (!entry.is_object())
                {
                    throw std::invalid_argument(
                        "Particle flipbook.frameRotationKeys entry must "
                        "be an object");
                }
                ParticleFlipbookRotationKey key;
                key.time = ReadFiniteFloat(entry, "time", 0.0f);
                key.degrees = ReadFiniteFloat(
                    entry, "degrees", 0.0f);
                key.tangentIn = ReadFiniteFloat(
                    entry, "tangentIn", 0.0f);
                key.tangentOut = ReadFiniteFloat(
                    entry, "tangentOut", 0.0f);
                if (key.time < 0.0f || key.time > 1.0f ||
                    key.time <= previousTime)
                {
                    throw std::invalid_argument(
                        "Particle flipbook.frameRotationKeys times must "
                        "be in [0,1] and strictly increasing");
                }
                previousTime = key.time;
                emitter.flipbookFrameRotationKeys.push_back(key);
            }
        }

        if (flipbook.contains("frameDurations"))
        {
            const nlohmann::json& durations =
                flipbook.at("frameDurations");
            if (!durations.is_array() ||
                durations.size() > kMaxParticleFlipbookFrames)
            {
                throw std::invalid_argument(
                    "Particle flipbook frameDurations requires an array "
                    "of at most 256 entries");
            }
            emitter.flipbookFrameDurations.reserve(durations.size());
            for (const nlohmann::json& entry : durations)
            {
                const float duration = ReadFiniteNumber(
                    entry, "flipbook.frameDurations");
                if (duration <= 0.0f)
                {
                    throw std::invalid_argument(
                        "Particle flipbook frameDuration must be finite "
                        "and positive");
                }
                emitter.flipbookFrameDurations.push_back(duration);
            }
        }

        if (flipbook.contains("framePivots"))
        {
            const nlohmann::json& pivots =
                flipbook.at("framePivots");
            if (!pivots.is_array() ||
                pivots.size() > kMaxParticleFlipbookFrames)
            {
                throw std::invalid_argument(
                    "Particle flipbook framePivots requires an array "
                    "of at most 256 entries");
            }
            emitter.flipbookFramePivots.reserve(pivots.size());
            for (const nlohmann::json& entry : pivots)
            {
                if (!entry.is_array() || entry.size() != 2)
                {
                    throw std::invalid_argument(
                        "Particle flipbook framePivot requires [u, v]");
                }
                Eigen::Vector2f pivot(
                    ReadFiniteNumber(
                        entry.at(0), "flipbook.framePivots.u"),
                    ReadFiniteNumber(
                        entry.at(1), "flipbook.framePivots.v"));
                if (!std::isfinite(pivot.x()) ||
                    !std::isfinite(pivot.y()) ||
                    pivot.x() < 0.0f || pivot.x() > 1.0f ||
                    pivot.y() < 0.0f || pivot.y() > 1.0f)
                {
                    throw std::invalid_argument(
                        "Particle flipbook framePivot components must be "
                        "finite and in [0, 1]");
                }
                emitter.flipbookFramePivots.push_back(pivot);
            }
        }

        if (flipbook.contains("frameUVs"))
        {
            const nlohmann::json& uvs = flipbook.at("frameUVs");
            if (!uvs.is_array() ||
                uvs.size() > kMaxParticleFlipbookFrames)
            {
                throw std::invalid_argument(
                    "Particle flipbook frameUVs requires an array "
                    "of at most 256 entries");
            }
            emitter.flipbookFrameUVs.reserve(uvs.size());
            for (const nlohmann::json& entry : uvs)
            {
                if (!entry.is_array() || entry.size() != 4)
                {
                    throw std::invalid_argument(
                        "Particle flipbook frameUV requires "
                        "[u0, v0, u1, v1]");
                }
                Eigen::Vector4f rect(
                    ReadFiniteNumber(
                        entry.at(0), "flipbook.frameUVs.u0"),
                    ReadFiniteNumber(
                        entry.at(1), "flipbook.frameUVs.v0"),
                    ReadFiniteNumber(
                        entry.at(2), "flipbook.frameUVs.u1"),
                    ReadFiniteNumber(
                        entry.at(3), "flipbook.frameUVs.v1"));
                for (int component = 0; component < 4; ++component)
                {
                    if (!std::isfinite(rect[component]) ||
                        rect[component] < 0.0f ||
                        rect[component] > 1.0f)
                    {
                        throw std::invalid_argument(
                            "Particle flipbook frameUV components must be "
                            "finite and in [0, 1]");
                    }
                }
                if (rect.x() >= rect.z() || rect.y() >= rect.w())
                {
                    throw std::invalid_argument(
                        "Particle flipbook frameUV requires u0 < u1 "
                        "and v0 < v1");
                }
                emitter.flipbookFrameUVs.push_back(rect);
            }
        }

        if (flipbook.contains("frameRotations"))
        {
            const nlohmann::json& rotations =
                flipbook.at("frameRotations");
            if (!rotations.is_array() ||
                rotations.size() > kMaxParticleFlipbookFrames)
            {
                throw std::invalid_argument(
                    "Particle flipbook frameRotations requires an array "
                    "of at most 256 entries");
            }
            emitter.flipbookFrameRotations.reserve(rotations.size());
            for (const nlohmann::json& entry : rotations)
            {
                emitter.flipbookFrameRotations.push_back(
                    ReadFiniteNumber(
                        entry, "flipbook.frameRotations"));
            }
        }
    }
    else
    {
        const uint32_t frameCount =
            emitter.flipbookColumns * emitter.flipbookRows;
        emitter.flipbookEndFrame =
            frameCount > 0 ? frameCount - 1 : 0;
    }

    std::string blend =
        json.value("blend", std::string("additive"));
    // v2 renderer 模块暴露的是布尔 additive；v1 继续使用 blend 字符串。
    // 两者同时出现时 additive 是 v2 模块作者意图，优先于底层 blend。
    if (json.contains("additive"))
    {
        if (!json.at("additive").is_boolean())
        {
            throw std::invalid_argument(
                "Particle additive must be a boolean");
        }
        blend = json.at("additive").get<bool>()
            ? "additive"
            : "alpha";
    }
    if (blend != "additive" && blend != "alpha")
    {
        throw std::invalid_argument(
            "Particle blend must be 'additive' or 'alpha'");
    }
    emitter.additive = blend == "additive";
    return emitter;
}

} // namespace ParticleAssetDetail
} // namespace VL
