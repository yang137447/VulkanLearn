#include "particles/particleAsset.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "commonFunction.h"
#include "particles/particleFlipbook.h"
#include "particles/particleMaterial.h"
#include "particles/particleMesh.h"
#include "particles/particleMeshCollision.h"
#include "particles/particleMeshReference.h"
#include "particles/particleSdf.h"
#include "particles/particleTextureReference.h"

namespace VL
{
namespace
{

Eigen::Vector3f ReadVec3(
    const nlohmann::json& object,
    const char* field,
    const Eigen::Vector3f& fallback)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_array() || value.size() != 3)
    {
        throw std::invalid_argument(
            std::string("Particle vector requires three components: ") +
            field);
    }
    Eigen::Vector3f result;
    for (int axis = 0; axis < 3; ++axis)
    {
        const nlohmann::json& component = value.at(axis);
        if (!component.is_number() ||
            !std::isfinite(component.get<float>()))
        {
            throw std::invalid_argument(
                std::string("Particle vector is not finite: ") + field);
        }
        result[axis] = component.get<float>();
    }
    return result;
}

Eigen::Vector4f ReadVec4(
    const nlohmann::json& object,
    const char* field,
    const Eigen::Vector4f& fallback)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_array() || value.size() != 4)
    {
        throw std::invalid_argument(
            std::string("Particle color requires four components: ") +
            field);
    }
    Eigen::Vector4f result;
    for (int channel = 0; channel < 4; ++channel)
    {
        const nlohmann::json& component = value.at(channel);
        if (!component.is_number() ||
            !std::isfinite(component.get<float>()))
        {
            throw std::invalid_argument(
                std::string("Particle color is not finite: ") + field);
        }
        result[channel] = component.get<float>();
    }
    return result;
}

float ReadFiniteFloat(
    const nlohmann::json& object,
    const char* field,
    float fallback)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_number() ||
        !std::isfinite(value.get<float>()))
    {
        throw std::invalid_argument(
            std::string("Particle field is not finite: ") + field);
    }
    return value.get<float>();
}

float ReadFiniteNumber(
    const nlohmann::json& value,
    const char* field)
{
    if (!value.is_number())
    {
        throw std::invalid_argument(
            std::string("Particle field must be a number: ") + field);
    }
    const double number = value.get<double>();
    if (!std::isfinite(number))
    {
        throw std::invalid_argument(
            std::string("Particle field is not finite: ") + field);
    }
    return static_cast<float>(number);
}

// 计数字段必须是整数：直接 get<uint32_t> 会把 512.9 静默截断成 512。
uint32_t ReadCountField(
    const nlohmann::json& object,
    const char* field,
    uint32_t fallback)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_number())
    {
        throw std::invalid_argument(
            std::string("Particle count must be a number: ") + field);
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(
            std::numeric_limits<uint32_t>::max()) ||
        std::floor(number) != number)
    {
        throw std::invalid_argument(
            std::string(
                "Particle count must be a non-negative integer: ") +
            field);
    }
    return static_cast<uint32_t>(number);
}

// flipbook 帧号必须是整数；JSON Number 在编辑器里可能以 double 到达，
// 因此先检查有限性、整数性和非负范围，避免直接 get<uint32_t> 静默截断。
uint32_t ReadFrameIndex(
    const nlohmann::json& object,
    const char* field,
    uint32_t fallback)
{
    if (!object.contains(field))
    {
        return fallback;
    }
    const nlohmann::json& value = object.at(field);
    if (!value.is_number())
    {
        throw std::invalid_argument(
            std::string("Particle flipbook frame must be a number: ") +
            field);
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < 0.0 ||
        number > static_cast<double>(
            std::numeric_limits<uint32_t>::max()) ||
        std::floor(number) != number)
    {
        throw std::invalid_argument(
            std::string("Particle flipbook frame must be a non-negative integer: ") +
            field);
    }
    return static_cast<uint32_t>(number);
}

Eigen::Vector3f NormalizeParticleCollisionPlaneNormal(
    const Eigen::Vector3f& normal,
    const char* field)
{
    const float length = normal.norm();
    if (!normal.allFinite() || !std::isfinite(length) ||
        length <= 1.0e-6f)
    {
        throw std::invalid_argument(
            std::string(
                "Particle collision plane normal must be finite and non-zero: ") +
            field);
    }
    return normal / length;
}

// v2 用户参数绑定：任何以 '$' 开头的字符串在展平后的 emitter JSON 中
// 替换为同名数值参数。这样模块字段（spawnRate、gravity、radius 等）
// 与普通数值走同一条解析/校验路径，不存在“只解析不生效”的旁路。
void SubstituteParticleUserParameters(
    nlohmann::json& value,
    const std::unordered_map<std::string, float>& parameters,
    const std::string& fieldPath,
    std::unordered_set<std::string>* references = nullptr)
{
    if (value.is_object())
    {
        for (auto child = value.begin(); child != value.end(); ++child)
        {
            SubstituteParticleUserParameters(
                child.value(),
                parameters,
                fieldPath.empty()
                    ? child.key()
                    : fieldPath + "." + child.key(),
                references);
        }
        return;
    }
    if (value.is_array())
    {
        for (size_t index = 0; index < value.size(); ++index)
        {
            SubstituteParticleUserParameters(
                value.at(index),
                parameters,
                fieldPath + "[" + std::to_string(index) + "]",
                references);
        }
        return;
    }
    if (!value.is_string())
    {
        return;
    }
    const std::string reference = value.get<std::string>();
    if (reference.empty() || reference.front() != '$')
    {
        return;
    }
    const std::string name = reference.substr(1);
    if (references != nullptr)
    {
        references->insert(name);
    }
    const auto parameter = parameters.find(name);
    if (parameter == parameters.end())
    {
        throw std::invalid_argument(
            "Particle user parameter reference is unknown: " + name +
            " at " + fieldPath);
    }
    value = parameter->second;
}

std::string BuildParticleCollisionSourceIdentity(
    const std::string& reference,
    const std::filesystem::path& resolvedPath)
{
    std::error_code sizeError;
    const std::uintmax_t size =
        std::filesystem::file_size(resolvedPath, sizeError);
    std::error_code timeError;
    const std::filesystem::file_time_type writeTime =
        std::filesystem::last_write_time(resolvedPath, timeError);
    std::string key = reference;
    key += '|';
    key += sizeError ? std::string("missing") : std::to_string(size);
    key += '|';
    key += timeError
        ? std::string("missing")
        : std::to_string(static_cast<long long>(
            writeTime.time_since_epoch().count()));
    // 文件大小/时间粒度不足以保证失效；碰撞资产只在加载时计算一次内容
    // 摘要，避免运行时重解析再次读盘。
    std::ifstream file(resolvedPath, std::ios::binary);
    std::uint64_t contentHash = 1469598103934665603ull;
    if (file.is_open())
    {
        char buffer[8192];
        while (file.good())
        {
            file.read(buffer, sizeof(buffer));
            const std::streamsize count = file.gcount();
            for (std::streamsize index = 0; index < count; ++index)
            {
                contentHash ^= static_cast<std::uint8_t>(
                    buffer[static_cast<size_t>(index)]);
                contentHash *= 1099511628211ull;
            }
        }
    }
    key += '|';
    key += std::to_string(contentHash);
    return key;
}

std::shared_ptr<const ParticleSdfAsset> LoadParticleSdfAsset(
    const std::string& path,
    const std::filesystem::path& resourceRoot)
{
    if (path.empty())
    {
        return nullptr;
    }
    RequireValidParticleMeshPath(path, "collision.sdf.path");
    std::filesystem::path resolvedPath;
    try
    {
        resolvedPath = resourceRoot.empty()
            ? std::filesystem::path(CommonFunction::Path(path))
            : ValidateParticleMeshReference(
                resourceRoot, path, "collision.sdf.path");
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to open particle SDF asset: " + path +
            ": " + exception.what());
    }
    std::ifstream file(resolvedPath);
    if (!file.is_open())
    {
        throw std::runtime_error(
            "Failed to open particle SDF asset: " + path);
    }
    nlohmann::json json;
    try
    {
        file >> json;
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to parse particle SDF asset: " + path +
            ": " + exception.what());
    }
    ParticleSdfAsset sdf = ParseParticleSdfJson(json);
    sdf.sourceIdentity =
        BuildParticleCollisionSourceIdentity(path, resolvedPath);
    return std::make_shared<const ParticleSdfAsset>(std::move(sdf));
}

std::shared_ptr<const ParticleSdfAsset> LoadParticleCollisionMeshAsset(
    const std::string& path,
    const std::filesystem::path& resourceRoot,
    float minimumPadding)
{
    if (path.empty())
    {
        return nullptr;
    }
    RequireValidParticleMeshPath(path, "collision.mesh.path");
    std::filesystem::path resolvedPath;
    try
    {
        resolvedPath = resourceRoot.empty()
            ? std::filesystem::path(CommonFunction::Path(path))
            : ValidateParticleMeshReference(
                resourceRoot, path, "collision.mesh.path");
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to open particle collision mesh asset: " + path +
            ": " + exception.what());
    }
    const ParticleCollisionMeshAsset mesh =
        ReadParticleCollisionMeshFile(resolvedPath);
    // SDF 在 bounds 外返回“无碰撞”。如果 padding 小于粒子半径，
    // 粒子球可能已经接触网格但中心仍在 bounds 外，形成漏碰撞。
    if (std::isfinite(minimumPadding) &&
        minimumPadding > 0.0f &&
        mesh.padding + 1.0e-5f < minimumPadding)
    {
        throw std::invalid_argument(
            "Particle collision mesh padding must be >= "
            "collision.radius");
    }
    ParticleSdfAsset sdf = BuildParticleSdfFromCollisionMesh(mesh);
    sdf.sourceIdentity =
        BuildParticleCollisionSourceIdentity(path, resolvedPath);
    return std::make_shared<const ParticleSdfAsset>(std::move(sdf));
}

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
    emitter.billboardShadows = json.value(
        "billboardShadows", emitter.billboardShadows);
    emitter.billboardLighting = json.value(
        "billboardLighting", emitter.billboardLighting);
    emitter.billboardAmbient = ReadFiniteFloat(
        json, "billboardAmbient", emitter.billboardAmbient);
    if (emitter.renderMode != "billboard" &&
        emitter.renderMode != "ribbon" &&
        emitter.renderMode != "mesh")
    {
        throw std::invalid_argument(
            "Particle renderMode must be 'billboard', 'ribbon' or 'mesh'");
    }
    emitter.ribbonWidth = ReadFiniteFloat(
        json, "ribbonWidth", emitter.ribbonWidth);
    emitter.ribbonSegments = ReadCountField(
        json, "ribbonSegments", emitter.ribbonSegments);
    emitter.ribbonFade = ReadFiniteFloat(
        json, "ribbonFade", emitter.ribbonFade);
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
    // mesh 照明与几何参数同属 renderer 层；非 mesh 模式也可以作者化，
    // 但渲染端只在 renderMode=="mesh" 时消费。
    emitter.meshLighting = json.value(
        "meshLighting", emitter.meshLighting);
    emitter.meshAmbient = ReadFiniteFloat(
        json, "meshAmbient", emitter.meshAmbient);
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
    emitter.spawnRate = ReadFiniteFloat(
        json, "spawnRate", emitter.spawnRate);
    emitter.drag = ReadFiniteFloat(json, "drag", emitter.drag);
    emitter.softness = ReadFiniteFloat(
        json, "softness", emitter.softness);
    emitter.softParticles =
        json.value("softParticles", emitter.softParticles);
    emitter.softParticleDistance = ReadFiniteFloat(
        json, "softParticleDistance", emitter.softParticleDistance);
    emitter.sortParticles =
        json.value("sortParticles", emitter.sortParticles);
    emitter.opacity = ReadFiniteFloat(
        json, "opacity", emitter.opacity);
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

void RequireFinite(float value, const char* field)
{
    if (!std::isfinite(value))
    {
        throw std::invalid_argument(
            std::string("Particle field is not finite: ") + field);
    }
}

void ValidateEmitter(const ParticleEmitterAsset& emitter)
{
    // ParseJson 已逐字段做有限性检查；这里再覆盖一次，保证程序化构造或
    // 未来新增写入路径也不能把 NaN/Inf 带进 GPU 缓冲。
    RequireFinite(emitter.localPosition.x(), "position.x");
    RequireFinite(emitter.localPosition.y(), "position.y");
    RequireFinite(emitter.localPosition.z(), "position.z");
    RequireFinite(emitter.fixedBoundsCenter.x(), "fixedBounds.center.x");
    RequireFinite(emitter.fixedBoundsCenter.y(), "fixedBounds.center.y");
    RequireFinite(emitter.fixedBoundsCenter.z(), "fixedBounds.center.z");
    RequireFinite(emitter.fixedBoundsExtent.x(), "fixedBounds.extent.x");
    RequireFinite(emitter.fixedBoundsExtent.y(), "fixedBounds.extent.y");
    RequireFinite(emitter.fixedBoundsExtent.z(), "fixedBounds.extent.z");
    RequireFinite(emitter.spawnRate, "spawnRate");
    RequireFinite(emitter.ribbonWidth, "ribbonWidth");
    RequireFinite(emitter.ribbonFade, "ribbonFade");
    RequireFinite(emitter.lifetimeMin, "lifetime.min");
    RequireFinite(emitter.lifetimeMax, "lifetime.max");
    RequireFinite(emitter.velocityMin.x(), "velocity.min.x");
    RequireFinite(emitter.velocityMin.y(), "velocity.min.y");
    RequireFinite(emitter.velocityMin.z(), "velocity.min.z");
    RequireFinite(emitter.velocityMax.x(), "velocity.max.x");
    RequireFinite(emitter.velocityMax.y(), "velocity.max.y");
    RequireFinite(emitter.velocityMax.z(), "velocity.max.z");
    RequireFinite(emitter.gravity.x(), "gravity.x");
    RequireFinite(emitter.gravity.y(), "gravity.y");
    RequireFinite(emitter.gravity.z(), "gravity.z");
    RequireFinite(emitter.drag, "drag");
    RequireFinite(emitter.curlNoiseStrength, "curlNoise.strength");
    RequireFinite(emitter.curlNoiseFrequency, "curlNoise.frequency");
    RequireFinite(emitter.curlNoiseSpeed, "curlNoise.speed");
    RequireFinite(emitter.vortexStrength, "vortex.strength");
    RequireFinite(emitter.vortexRadius, "vortex.radius");
    RequireFinite(emitter.vortexAxis.x(), "vortex.axis.x");
    RequireFinite(emitter.vortexAxis.y(), "vortex.axis.y");
    RequireFinite(emitter.vortexAxis.z(), "vortex.axis.z");
    RequireFinite(emitter.attractorPosition.x(),
        "attractor.position.x");
    RequireFinite(emitter.attractorPosition.y(),
        "attractor.position.y");
    RequireFinite(emitter.attractorPosition.z(),
        "attractor.position.z");
    RequireFinite(emitter.attractorStrength, "attractor.strength");
    RequireFinite(emitter.attractorRadius, "attractor.radius");
    RequireFinite(emitter.wind.x(), "wind.x");
    RequireFinite(emitter.wind.y(), "wind.y");
    RequireFinite(emitter.wind.z(), "wind.z");
    RequireFinite(emitter.shapeRadius, "shape.radius");
    RequireFinite(emitter.shapeExtent.x(), "shape.extent.x");
    RequireFinite(emitter.shapeExtent.y(), "shape.extent.y");
    RequireFinite(emitter.shapeExtent.z(), "shape.extent.z");
    RequireFinite(emitter.shapeHeight, "shape.height");
    RequireFinite(emitter.shapeInnerRadius, "shape.innerRadius");
    RequireFinite(emitter.collisionPlaneNormal.x(),
        "collision.plane.normal.x");
    RequireFinite(emitter.collisionPlaneNormal.y(),
        "collision.plane.normal.y");
    RequireFinite(emitter.collisionPlaneNormal.z(),
        "collision.plane.normal.z");
    RequireFinite(emitter.collisionPlaneOffset,
        "collision.plane.offset");
    RequireFinite(emitter.collisionBoxMin.x(),
        "collision.box.min.x");
    RequireFinite(emitter.collisionBoxMin.y(),
        "collision.box.min.y");
    RequireFinite(emitter.collisionBoxMin.z(),
        "collision.box.min.z");
    RequireFinite(emitter.collisionBoxMax.x(),
        "collision.box.max.x");
    RequireFinite(emitter.collisionBoxMax.y(),
        "collision.box.max.y");
    RequireFinite(emitter.collisionBoxMax.z(),
        "collision.box.max.z");
    RequireFinite(emitter.collisionCapsuleA.x(),
        "collision.capsule.a.x");
    RequireFinite(emitter.collisionCapsuleA.y(),
        "collision.capsule.a.y");
    RequireFinite(emitter.collisionCapsuleA.z(),
        "collision.capsule.a.z");
    RequireFinite(emitter.collisionCapsuleB.x(),
        "collision.capsule.b.x");
    RequireFinite(emitter.collisionCapsuleB.y(),
        "collision.capsule.b.y");
    RequireFinite(emitter.collisionCapsuleB.z(),
        "collision.capsule.b.z");
    RequireFinite(emitter.collisionCapsuleRadius,
        "collision.capsule.radius");
    RequireFinite(emitter.collisionBounce, "collision.bounce");
    RequireFinite(emitter.collisionFriction, "collision.friction");
    RequireFinite(emitter.collisionRadius, "collision.radius");
    RequireFinite(emitter.sizeStart, "size.start");
    RequireFinite(emitter.sizeEnd, "size.end");
    RequireFinite(emitter.rotation, "rotation");
    RequireFinite(emitter.rotationSpeed, "rotationSpeed");
    RequireFinite(emitter.softness, "softness");
    RequireFinite(emitter.softParticleDistance, "softParticleDistance");
    RequireFinite(emitter.opacity, "opacity");
    RequireFinite(emitter.maxDrawDistance, "maxDrawDistance");
    RequireFinite(emitter.fadeDistance, "fadeDistance");
    RequireFinite(emitter.minScreenSize, "minScreenSize");
    RequireFinite(emitter.maxScreenSize, "maxScreenSize");
    RequireFinite(emitter.burstInterval, "burstInterval");
    RequireFinite(emitter.lodDistance, "lodDistance");
    RequireFinite(emitter.lodSpawnRateScale, "lodSpawnRateScale");
    RequireFinite(emitter.flipbookFramesPerSecond, "flipbook.fps");
    for (int channel = 0; channel < 4; ++channel)
    {
        RequireFinite(emitter.colorStart[channel], "colorStart");
        RequireFinite(emitter.colorEnd[channel], "colorEnd");
    }

    if (emitter.maxParticles == 0 || emitter.maxParticles > 65536)
    {
        throw std::invalid_argument(
            "Particle maxParticles must be in [1, 65536]");
    }
    if (emitter.renderMode != "billboard" &&
        emitter.renderMode != "ribbon" &&
        emitter.renderMode != "mesh")
    {
        throw std::invalid_argument(
            "Particle renderMode must be 'billboard', 'ribbon' or 'mesh'");
    }
    if (emitter.billboardAlignment != "camera" &&
        emitter.billboardAlignment != "velocity")
    {
        throw std::invalid_argument(
            "Particle billboardAlignment must be 'camera' or 'velocity'");
    }
    if (emitter.ribbonWidth <= 0.0f)
    {
        throw std::invalid_argument(
            "Particle ribbonWidth must be positive");
    }
    if (emitter.ribbonSegments < 1 || emitter.ribbonSegments > 3)
    {
        throw std::invalid_argument(
            "Particle ribbonSegments must be in [1, 3]");
    }
    if (emitter.ribbonFade < 0.0f || emitter.ribbonFade > 1.0f)
    {
        throw std::invalid_argument(
            "Particle ribbonFade must be in [0, 1]");
    }
    if (!emitter.meshPath.empty())
    {
        RequireValidParticleMeshPath(emitter.meshPath);
    }
    RequireFinite(emitter.meshScale.x(), "mesh.scale.x");
    RequireFinite(emitter.meshScale.y(), "mesh.scale.y");
    RequireFinite(emitter.meshScale.z(), "mesh.scale.z");
    if (emitter.meshScale.squaredNorm() <= 1.0e-12f)
    {
        throw std::invalid_argument(
            "Particle mesh.scale must be non-zero");
    }
    if (!IsParticleMeshAlignment(emitter.meshAlignment))
    {
        throw std::invalid_argument(
            "Particle mesh.alignment must be 'world', 'camera' or 'velocity'");
    }
    RequireFinite(emitter.meshAmbient, "meshAmbient");
    if (emitter.meshAmbient < 0.0f || emitter.meshAmbient > 1.0f)
    {
        throw std::invalid_argument(
            "Particle meshAmbient must be in [0, 1]");
    }
    RequireFinite(emitter.billboardAmbient, "billboardAmbient");
    if (emitter.billboardAmbient < 0.0f ||
        emitter.billboardAmbient > 1.0f)
    {
        throw std::invalid_argument(
            "Particle billboardAmbient must be in [0, 1]");
    }
    if (emitter.renderMode == "mesh")
    {
        if (emitter.meshPath.empty())
        {
            throw std::invalid_argument(
                "Particle mesh renderMode requires a non-empty mesh.path");
        }
        if (emitter.localSpace)
        {
            throw std::invalid_argument(
                "Particle mesh renderMode does not support localSpace=true");
        }
    }
    if (emitter.spawnRate < 0.0f || emitter.spawnRate > 100000.0f)
    {
        throw std::invalid_argument(
            "Particle spawnRate must be in [0, 100000]");
    }
    if (emitter.burstInterval < 0.0f || emitter.burstInterval > 60.0f)
    {
        throw std::invalid_argument(
            "Particle burstInterval must be in [0, 60]");
    }
    if (emitter.lodDistance < 0.0f)
    {
        throw std::invalid_argument(
            "Particle lodDistance must be non-negative");
    }
    if (emitter.lodSpawnRateScale < 0.0f ||
        emitter.lodSpawnRateScale > 1.0f)
    {
        throw std::invalid_argument(
            "Particle lodSpawnRateScale must be in [0, 1]");
    }
    if (emitter.lodTiers.size() > kMaxParticleLodTiers)
    {
        throw std::invalid_argument(
            "Particle lod.tiers must contain at most 4 entries");
    }
    for (size_t index = 0; index < emitter.lodTiers.size(); ++index)
    {
        const ParticleLodTier& tier = emitter.lodTiers[index];
        if (!std::isfinite(tier.distance) ||
            !std::isfinite(tier.spawnRateScale))
        {
            throw std::invalid_argument(
                "Particle lod tier values must be finite");
        }
        if (tier.distance < 0.0f)
        {
            throw std::invalid_argument(
                "Particle lod tier distance must be non-negative");
        }
        if (tier.spawnRateScale < 0.0f ||
            tier.spawnRateScale > 1.0f)
        {
            throw std::invalid_argument(
                "Particle lod tier spawnRateScale must be in [0, 1]");
        }
        // 选择规则是“已越过的最远档位”，乱序或重复距离会让档位含义
        // 依赖数组顺序；要求严格递增才能保证结果唯一。
        if (index > 0 &&
            tier.distance <= emitter.lodTiers[index - 1].distance)
        {
            throw std::invalid_argument(
                "Particle lod tier distances must be strictly "
                "increasing");
        }
    }
    if (emitter.lifetimeMin <= 0.0f ||
        emitter.lifetimeMax < emitter.lifetimeMin ||
        emitter.lifetimeMax > 60.0f)
    {
        throw std::invalid_argument(
            "Particle lifetime requires 0 < min <= max <= 60");
    }
    if (emitter.drag < 0.0f || emitter.drag > 20.0f)
    {
        throw std::invalid_argument(
            "Particle drag must be in [0, 20]");
    }
    if (emitter.curlNoiseStrength < 0.0f)
    {
        throw std::invalid_argument(
            "Particle curlNoise.strength must be non-negative");
    }
    if (emitter.curlNoiseFrequency < 0.0f)
    {
        throw std::invalid_argument(
            "Particle curlNoise.frequency must be non-negative");
    }
    if (emitter.vortexStrength < 0.0f)
    {
        throw std::invalid_argument(
            "Particle vortex.strength must be non-negative");
    }
    if (emitter.vortexRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle vortex.radius must be non-negative");
    }
    if (emitter.vortexStrength > 0.0f &&
        emitter.vortexAxis.squaredNorm() <= 1.0e-12f)
    {
        throw std::invalid_argument(
            "Particle vortex.axis must be non-zero when "
            "vortex.strength is positive");
    }
    if (emitter.attractorStrength < 0.0f)
    {
        throw std::invalid_argument(
            "Particle attractor.strength must be non-negative");
    }
    if (emitter.attractorRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle attractor.radius must be non-negative");
    }
    if (emitter.attractorStrength > 0.0f &&
        emitter.attractorRadius <= 0.0f)
    {
        throw std::invalid_argument(
            "Particle attractor.radius must be positive when "
            "attractor.strength is positive");
    }
    if (emitter.shapeRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle shape radius must be non-negative");
    }
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
    // 未使用字段仍要求非负，保证切换 shape.type 时不会把资产里的负值
    // 或退化尺寸带进运行时采样。
    for (int axis = 0; axis < 3; ++axis)
    {
        if (emitter.shapeExtent[axis] < 0.0f)
        {
            throw std::invalid_argument(
                "Particle shape extent must be non-negative");
        }
    }
    if (emitter.shapeHeight < 0.0f)
    {
        throw std::invalid_argument(
            "Particle shape height must be non-negative");
    }
    if (emitter.shapeInnerRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle shape innerRadius must be non-negative");
    }
    if (emitter.shapeType == "box" &&
        (emitter.shapeExtent.x() <= 0.0f ||
         emitter.shapeExtent.y() <= 0.0f ||
         emitter.shapeExtent.z() <= 0.0f))
    {
        throw std::invalid_argument(
            "Particle box shape extent must be positive on all axes");
    }
    if ((emitter.shapeType == "cone" ||
         emitter.shapeType == "cylinder") &&
        emitter.shapeHeight <= 0.0f)
    {
        throw std::invalid_argument(
            "Particle cone/cylinder shape height must be positive");
    }
    if (emitter.shapeType == "ring" &&
        (emitter.shapeRadius <= 0.0f ||
         emitter.shapeInnerRadius >= emitter.shapeRadius))
    {
        throw std::invalid_argument(
            "Particle ring shape requires radius > 0 and "
            "0 <= innerRadius < radius");
    }
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
    // localSpace 与碰撞现在允许组合：localSpace=true 时碰撞体（平面/
    // 盒体/胶囊/SDF/三角网格）按发射器局部空间解释，与局部粒子坐标
    // 同一空间。
    if (emitter.collisionType == "sdf" &&
        (!emitter.collisionSdf || emitter.collisionSdf->Empty()))
    {
        throw std::invalid_argument(
            "Particle collision type 'sdf' requires loaded SDF data");
    }
    if (emitter.collisionType == "mesh")
    {
        if (emitter.collisionMeshPath.empty())
        {
            throw std::invalid_argument(
                "Particle collision type 'mesh' requires "
                "collision.mesh.path");
        }
        RequireValidParticleMeshPath(
            emitter.collisionMeshPath, "collision.mesh.path");
        if (!emitter.collisionSdf || emitter.collisionSdf->Empty())
        {
            throw std::invalid_argument(
                "Particle collision type 'mesh' requires loaded SDF data");
        }
        if (emitter.collisionSdf->sourcePadding >= 0.0f &&
            emitter.collisionRadius >
                emitter.collisionSdf->sourcePadding + 1.0e-5f)
        {
            throw std::invalid_argument(
                "Particle collision mesh padding must be >= "
                "collision.radius");
        }
    }
    else if (!emitter.collisionMeshPath.empty())
    {
        // 非激活类型的路径仍要满足资源根相对契约，避免切换类型时
        // 才暴露拼写错误。
        RequireValidParticleMeshPath(
            emitter.collisionMeshPath, "collision.mesh.path");
    }
    if (!emitter.materialInstancePath.empty() &&
        !IsValidParticleMeshRelativePath(emitter.materialInstancePath))
    {
        throw std::invalid_argument(
            "Particle materialInstancePath must be a "
            "resource-root-relative .json path: " +
            emitter.materialInstancePath);
    }
    if (emitter.fixedTimeStep < 0.0f ||
        emitter.fixedTimeStep > 0.25f)
    {
        throw std::invalid_argument(
            "Particle fixedTimeStep must be in [0, 0.25] seconds");
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (emitter.fixedBoundsExtent[axis] < 0.0f)
        {
            throw std::invalid_argument(
                "Particle fixedBounds extent must be non-negative");
        }
    }
    if (emitter.useFixedBounds &&
        emitter.fixedBoundsExtent.squaredNorm() <= 0.0f)
    {
        throw std::invalid_argument(
            "Particle fixedBounds extent must not be all zero");
    }
    if (!emitter.collisionPlaneNormal.allFinite() ||
        emitter.collisionPlaneNormal.norm() <= 1.0e-6f)
    {
        throw std::invalid_argument(
            "Particle collision plane normal must be finite and non-zero");
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (emitter.collisionBoxMin[axis] > emitter.collisionBoxMax[axis])
        {
            throw std::invalid_argument(
                "Particle collision box min must not exceed max");
        }
    }
    if (emitter.collisionBounce < 0.0f ||
        emitter.collisionBounce > 1.0f ||
        emitter.collisionFriction < 0.0f ||
        emitter.collisionFriction > 1.0f)
    {
        throw std::invalid_argument(
            "Particle collision bounce/friction must be in [0, 1]");
    }
    if (emitter.collisionRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle collision radius must be non-negative");
    }
    if (emitter.collisionCapsuleRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle collision capsule radius must be non-negative");
    }
    if (emitter.sizeStart < 0.0f || emitter.sizeEnd < 0.0f)
    {
        throw std::invalid_argument(
            "Particle size must be non-negative");
    }
    if (emitter.rotationSpeed < -3600.0f ||
        emitter.rotationSpeed > 3600.0f)
    {
        throw std::invalid_argument(
            "Particle rotationSpeed must be in [-3600, 3600]");
    }
    if (emitter.softness < 0.0f || emitter.softness > 1.0f)
    {
        throw std::invalid_argument(
            "Particle softness must be in [0, 1]");
    }
    if (emitter.softParticleDistance < 0.0f ||
        emitter.softParticleDistance > 10000.0f)
    {
        throw std::invalid_argument(
            "Particle softParticleDistance must be in [0, 10000]");
    }
    // 当前 bitonic sort 每个 pass 一次 dispatch；4096 已需要 78 个 pass。
    // 排序池必须覆盖整个 particle pool，因此不能在排序开启时静默截断。
    if (emitter.sortParticles && emitter.maxParticles > 4096)
    {
        throw std::invalid_argument(
            "Particle sortParticles requires maxParticles <= 4096");
    }
    // alpha billboard 的绘制顺序直接影响结果；作者忘了开启排序时直接拒绝，
    // 而不是静默按池索引顺序画出穿插。additive 与顺序无关，保持自由。
    if (!emitter.additive && !emitter.sortParticles)
    {
        throw std::invalid_argument(
            "Particle alpha blend requires sortParticles=true");
    }
    if (emitter.opacity < 0.0f || emitter.opacity > 1.0f)
    {
        throw std::invalid_argument(
            "Particle opacity must be in [0, 1]");
    }
    if (emitter.maxDrawDistance < 0.0f ||
        emitter.fadeDistance < 0.0f ||
        emitter.minScreenSize < 0.0f ||
        emitter.maxScreenSize < 0.0f)
    {
        throw std::invalid_argument(
            "Particle visibility fields must be non-negative");
    }
    if (emitter.maxDrawDistance == 0.0f)
    {
        if (emitter.fadeDistance != 0.0f)
        {
            throw std::invalid_argument(
                "Particle fadeDistance requires maxDrawDistance > 0");
        }
    }
    else if (emitter.fadeDistance > emitter.maxDrawDistance)
    {
        throw std::invalid_argument(
            "Particle fadeDistance must not exceed maxDrawDistance");
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (emitter.velocityMin[axis] > emitter.velocityMax[axis])
        {
            throw std::invalid_argument(
                "Particle velocity min must not exceed max");
        }
    }
    if (emitter.colorKeys.size() > kMaxParticleCurveKeys ||
        emitter.sizeKeys.size() > kMaxParticleCurveKeys ||
        emitter.lifetimeScaleKeys.size() > kMaxParticleCurveKeys)
    {
        throw std::invalid_argument(
            "Particle curve key count exceeds GPU capacity");
    }
    float previousColorTime = -1.0f;
    for (const ParticleColorKey& key : emitter.colorKeys)
    {
        RequireFinite(key.time, "colorKeys.time");
        for (int channel = 0; channel < 4; ++channel)
        {
            RequireFinite(key.color[channel], "colorKeys.color");
        }
        if (key.time < 0.0f || key.time > 1.0f ||
            key.time <= previousColorTime)
        {
            throw std::invalid_argument(
                "Particle color key times must be strictly increasing in [0,1]");
        }
        previousColorTime = key.time;
    }
    float previousSizeTime = -1.0f;
    for (const ParticleSizeKey& key : emitter.sizeKeys)
    {
        RequireFinite(key.time, "sizeKeys.time");
        RequireFinite(key.size, "sizeKeys.size");
        if (key.time < 0.0f || key.time > 1.0f ||
            key.time <= previousSizeTime)
        {
            throw std::invalid_argument(
                "Particle size key times must be strictly increasing in [0,1]");
        }
        if (key.size < 0.0f)
        {
            throw std::invalid_argument(
                "Particle size key value must be non-negative");
        }
        previousSizeTime = key.time;
    }
    float previousLifetimeScaleTime = -1.0f;
    for (const ParticleLifetimeScaleKey& key :
         emitter.lifetimeScaleKeys)
    {
        RequireFinite(key.time, "lifetimeScaleKeys.time");
        RequireFinite(key.scale, "lifetimeScaleKeys.scale");
        if (key.time < 0.0f || key.time > 1.0f ||
            key.time < previousLifetimeScaleTime)
        {
            throw std::invalid_argument(
                "Particle lifetime scale key times must be in [0,1] "
                "and non-decreasing");
        }
        if (key.scale < 0.0f || key.scale > 4.0f)
        {
            throw std::invalid_argument(
                "Particle lifetime scale key value must be in [0,4]");
        }
        previousLifetimeScaleTime = key.time;
    }
    if (emitter.flipbookColumns == 0 || emitter.flipbookColumns > 16 ||
        emitter.flipbookRows == 0 || emitter.flipbookRows > 16)
    {
        throw std::invalid_argument(
            "Particle flipbook columns/rows must be in [1, 16]");
    }
    if (emitter.flipbookFramesPerSecond < 0.0f ||
        emitter.flipbookFramesPerSecond > 240.0f)
    {
        throw std::invalid_argument(
            "Particle flipbook fps must be in [0, 240]");
    }
    const uint32_t frameCount =
        emitter.flipbookColumns * emitter.flipbookRows;
    if (emitter.flipbookStartFrame >= frameCount)
    {
        throw std::invalid_argument(
            "Particle flipbook startFrame must be within the atlas");
    }
    if (emitter.flipbookEndFrame < emitter.flipbookStartFrame ||
        emitter.flipbookEndFrame >= frameCount)
    {
        throw std::invalid_argument(
            "Particle flipbook endFrame must be >= startFrame and within the atlas");
    }
    const uint32_t frameSpan =
        emitter.flipbookEndFrame - emitter.flipbookStartFrame + 1;
    if (frameSpan > kMaxParticleFlipbookFrames)
    {
        throw std::invalid_argument(
            "Particle flipbook frame span must be at most 256 frames");
    }
    if (!emitter.flipbookFrameDurations.empty())
    {
        if (emitter.flipbookFrameDurations.size() != frameSpan)
        {
            throw std::invalid_argument(
                "Particle flipbook frameDurations length must match "
                "endFrame-startFrame+1");
        }
        for (float duration : emitter.flipbookFrameDurations)
        {
            if (!std::isfinite(duration) || duration <= 0.0f)
            {
                throw std::invalid_argument(
                    "Particle flipbook frameDurations must all be finite "
                    "and positive");
            }
        }
    }
    if (!emitter.flipbookFramePivots.empty())
    {
        if (emitter.flipbookFramePivots.size() != frameSpan)
        {
            throw std::invalid_argument(
                "Particle flipbook framePivots length must match "
                "endFrame-startFrame+1");
        }
        for (const Eigen::Vector2f& pivot : emitter.flipbookFramePivots)
        {
            if (!std::isfinite(pivot.x()) || !std::isfinite(pivot.y()) ||
                pivot.x() < 0.0f || pivot.x() > 1.0f ||
                pivot.y() < 0.0f || pivot.y() > 1.0f)
            {
                throw std::invalid_argument(
                    "Particle flipbook framePivots must be finite and "
                    "within [0, 1]");
            }
        }
    }
    if (!emitter.flipbookFrameUVs.empty())
    {
        if (emitter.flipbookFrameUVs.size() != frameSpan)
        {
            throw std::invalid_argument(
                "Particle flipbook frameUVs length must match "
                "endFrame-startFrame+1");
        }
        // GPU 表是每帧一个 vec4，固定 64 项；超过时必须显式拒绝，
        // 不能把 256 帧资产的后半段静默截断成默认 rect。
        if (frameSpan > kParticleFlipbookUvVectors)
        {
            throw std::invalid_argument(
                "Particle flipbook frameUVs requires at most 64 frames");
        }
        for (const Eigen::Vector4f& rect : emitter.flipbookFrameUVs)
        {
            for (int component = 0; component < 4; ++component)
            {
                if (!std::isfinite(rect[component]) ||
                    rect[component] < 0.0f ||
                    rect[component] > 1.0f)
                {
                    throw std::invalid_argument(
                        "Particle flipbook frameUVs must be finite and "
                        "within [0, 1]");
                }
            }
            if (rect.x() >= rect.z() || rect.y() >= rect.w())
            {
                throw std::invalid_argument(
                    "Particle flipbook frameUVs require u0 < u1 "
                    "and v0 < v1");
            }
        }
    }
    if (!emitter.flipbookFrameRotations.empty())
    {
        if (emitter.flipbookFrameRotations.size() != frameSpan)
        {
            throw std::invalid_argument(
                "Particle flipbook frameRotations length must match "
                "endFrame-startFrame+1");
        }
        // GPU 表每 vec4 打包 4 帧，固定 64 帧；超过时必须拒绝，
        // 不能把后半段静默当成 0 度。
        if (frameSpan > kParticleFlipbookRotationVectors * 4)
        {
            throw std::invalid_argument(
                "Particle flipbook frameRotations requires at most "
                "64 frames");
        }
        for (float rotation : emitter.flipbookFrameRotations)
        {
            if (!std::isfinite(rotation))
            {
                throw std::invalid_argument(
                    "Particle flipbook frameRotations must all be finite");
            }
        }
    }
}

// v2 模块只做作者层组织；运行时仍消费展平后的 flat emitter。
// 未知模块类型必须拒绝，避免拼写错误静默变成空模块。
const std::unordered_set<std::string>& ParticleModuleTypes()
{
    static const std::unordered_set<std::string> types = {
        "emitter",
        "renderer",
        "spawn.rate",
        "spawn.shapeSphere",
        "spawn.velocity",
        "update.lifetime",
        "update.color",
        "update.size",
        "update.rotation",
        "forces.gravity",
        "forces.drag",
        "forces.curlNoise",
        "forces.vortex",
        "forces.attractor",
        "forces.wind",
        "collision.analytic",
        "events",
        "flipbook",
        "lod",
        "curves",
        "bounds",
        "culling"};
    return types;
}

void MergeParticleModuleFields(
    nlohmann::json& target,
    const nlohmann::json& source,
    const std::string& moduleType,
    const std::string& path)
{
    for (auto field = source.begin(); field != source.end(); ++field)
    {
        const std::string fieldPath = path.empty()
            ? field.key()
            : path + "." + field.key();
        if (!target.contains(field.key()))
        {
            target[field.key()] = field.value();
            continue;
        }
        if (target.at(field.key()).is_object() &&
            field.value().is_object())
        {
            MergeParticleModuleFields(
                target[field.key()],
                field.value(),
                moduleType,
                fieldPath);
            continue;
        }
        if (target.at(field.key()) != field.value())
        {
            throw std::invalid_argument(
                "Particle v2 module field conflict: " + moduleType +
                " -> " + fieldPath);
        }
    }
}

nlohmann::json FlattenParticleEmitterV2(
    const nlohmann::json& emitter,
    size_t index)
{
    if (!emitter.is_object())
    {
        throw std::invalid_argument(
            "Particle v2 emitter must be an object: " +
            std::to_string(index));
    }
    if (!emitter.contains("modules") ||
        !emitter.at("modules").is_array())
    {
        throw std::invalid_argument(
            "Particle v2 emitter requires a modules array: " +
            std::to_string(index));
    }
    const nlohmann::json& modules = emitter.at("modules");
    if (modules.size() > 64)
    {
        throw std::invalid_argument(
            "Particle v2 emitter supports at most 64 modules: " +
            std::to_string(index));
    }

    nlohmann::json flattened = emitter;
    flattened.erase("modules");
    for (const nlohmann::json& module : modules)
    {
        if (!module.is_object())
        {
            throw std::invalid_argument(
                "Particle v2 module must be an object");
        }
        const std::string type =
            module.value("type", std::string());
        if (ParticleModuleTypes().find(type) ==
            ParticleModuleTypes().end())
        {
            throw std::invalid_argument(
                "Particle v2 module type is unknown: " + type);
        }
        if (!module.value("enabled", true))
        {
            continue;
        }
        if (!module.contains("fields") ||
            !module.at("fields").is_object())
        {
            throw std::invalid_argument(
                "Particle v2 module requires an object fields: " + type);
        }
        MergeParticleModuleFields(
            flattened,
            module.at("fields"),
            type,
            {});
    }
    return flattened;
}

std::unordered_map<std::string, float> ParseParticleUserParameters(
    const nlohmann::json& json)
{
    if (!json.contains("userParameters"))
    {
        return {};
    }
    const nlohmann::json& parameters = json.at("userParameters");
    if (!parameters.is_object() || parameters.size() > 64)
    {
        throw std::invalid_argument(
            "Particle userParameters requires an object with at most "
            "64 entries");
    }
    std::unordered_map<std::string, float> result;
    result.reserve(parameters.size());
    for (auto parameter = parameters.begin();
         parameter != parameters.end(); ++parameter)
    {
        if (parameter.key().empty())
        {
            throw std::invalid_argument(
                "Particle user parameter name must be non-empty");
        }
        if (!parameter.value().is_number() ||
            !std::isfinite(parameter.value().get<float>()))
        {
            throw std::invalid_argument(
                "Particle user parameter must be finite: " +
                parameter.key());
        }
        result.emplace(
            parameter.key(),
            parameter.value().get<float>());
    }
    return result;
}

} // namespace

ParticleSystemAsset ParticleAssetLoader::Prepare(const std::string& path)
{
    std::string resolvedPath;
    try
    {
        resolvedPath = CommonFunction::Path(path);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to open particle system asset: " + path +
            ": " + exception.what());
    }

    std::ifstream file(resolvedPath);
    if (!file.is_open())
    {
        throw std::runtime_error(
            "Failed to open particle system asset: " + path);
    }

    const std::string source{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()};
    if (file.bad())
    {
        throw std::runtime_error(
            "Failed to read particle system asset: " + path);
    }

    nlohmann::json json;
    try
    {
        json = nlohmann::json::parse(source);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to parse particle system asset: " + path +
            ": " + exception.what());
    }
    return ParseJson(
        json,
        path,
        std::filesystem::path(CommonFunction::GetResourcePath()));
}

ParticleSystemAsset ParticleAssetLoader::ParseJson(
    const nlohmann::json& json,
    std::string_view sourcePath,
    const std::filesystem::path& resourceRoot,
    const ParticleCollisionResourceCache* collisionCache)
{
    const std::string path(sourcePath);
    if (!json.is_object())
    {
        throw std::invalid_argument(
            "Particle system asset must be a JSON object: " + path);
    }
    if (json.value("type", std::string()) != "particleSystem")
    {
        throw std::invalid_argument(
            "Particle system asset requires type=particleSystem: " + path);
    }

    ParticleSystemAsset asset;
    asset.sourcePath =
        std::filesystem::path(path).lexically_normal().generic_string();
    asset.name = json.value("name", asset.sourcePath);
    if (json.contains("version"))
    {
        const nlohmann::json& version = json.at("version");
        if (!version.is_number_integer() &&
            !version.is_number_unsigned())
        {
            throw std::invalid_argument(
                "Particle version must be an integer: " + path);
        }
        const int value = version.get<int>();
        if (value != 1 && value != 2)
        {
            throw std::invalid_argument(
                "Particle version must be 1 or 2: " + path);
        }
        asset.version = static_cast<uint32_t>(value);
    }
    if (asset.version == 2)
        asset.userParameters = ParseParticleUserParameters(json);
    std::unordered_set<std::string> userParameterReferences;
    if (json.contains("emitters"))
    {
        const nlohmann::json& emitters = json.at("emitters");
        if (!emitters.is_array() || emitters.empty())
        {
            throw std::invalid_argument(
                "Particle emitters requires a non-empty array");
        }
        if (emitters.size() > kMaxParticleEmitters)
        {
            throw std::invalid_argument(
                "Particle emitters supports at most 16 entries");
        }
        asset.emitters.reserve(emitters.size());
        for (size_t index = 0; index < emitters.size(); ++index)
        {
            nlohmann::json flattened = asset.version == 2
                ? FlattenParticleEmitterV2(emitters.at(index), index)
                : emitters.at(index);
            if (asset.version == 2)
            {
                SubstituteParticleUserParameters(
                    flattened,
                    asset.userParameters,
                    "emitters[" + std::to_string(index) + "]",
                    &userParameterReferences);
            }
            asset.emitters.push_back(
                ParseEmitter(
                    flattened,
                    index,
                    false,
                    resourceRoot,
                    collisionCache));
        }
    }
    else if (asset.version == 2 && json.contains("modules"))
    {
        // v2 也允许根对象直接携带一个隐式 emitter 的 modules。
        nlohmann::json flattened = FlattenParticleEmitterV2(json, 0);
        SubstituteParticleUserParameters(
            flattened,
            asset.userParameters,
            "modules",
            &userParameterReferences);
        asset.emitters.push_back(
            ParseEmitter(
                flattened,
                0,
                true,
                resourceRoot,
                collisionCache));
    }
    else
    {
        // 旧 PS 资产没有 emitters：根对象就是一个隐式发射器。
        asset.emitters.push_back(
            ParseEmitter(
                json,
                0,
                true,
                resourceRoot,
                collisionCache));
    }

    Validate(asset);
    asset.userParameterReferences =
        std::move(userParameterReferences);
    for (const ParticleEmitterAsset& emitter : asset.emitters)
    {
        if (emitter.texturePath.empty()) continue;
        // 在场景候选提交前解析一次资源根，避免渲染线程首次创建发射器时
        // 才发现 flipbook 纹理缺失。GPU 纹理本身仍由 ParticleRenderer 创建。
        if (resourceRoot.empty())
        {
            (void)CommonFunction::Path(emitter.texturePath);
        }
        else
        {
            (void)ValidateParticleTextureReference(
                resourceRoot, emitter.texturePath, "texture.path");
        }
    }
    // 只有 v2 + 用户参数才需要运行时覆盖重解析；冻结原始 JSON
    // （而不是展平后的 emitter），保证模块结构/字段归属不丢失。
    if (asset.version == 2 && !asset.userParameters.empty())
    {
        asset.authoringJson =
            std::make_shared<const nlohmann::json>(json);
    }
    return asset;
}

void ParticleAssetLoader::Validate(const ParticleSystemAsset& asset)
{
    if (asset.name.empty())
    {
        throw std::invalid_argument("Particle system requires a name");
    }
    if (asset.emitters.empty() ||
        asset.emitters.size() > kMaxParticleEmitters)
    {
        throw std::invalid_argument(
            "Particle system requires 1..16 emitters");
    }
    std::unordered_set<std::string> names;
    names.reserve(asset.emitters.size());
    for (const ParticleEmitterAsset& emitter : asset.emitters)
    {
        if (emitter.name.empty())
        {
            throw std::invalid_argument(
                "Particle emitter requires a non-empty name");
        }
        if (!names.insert(emitter.name).second)
        {
            throw std::invalid_argument(
                "Particle emitter names must be unique: " +
                emitter.name);
        }
        ValidateEmitter(emitter);
    }
}

} // namespace VL
