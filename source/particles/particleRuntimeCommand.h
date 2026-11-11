#pragma once

#include <cmath>
#include <string>
#include <string_view>

#include <Eigen/Dense>

#include "particles/particleRuntimeState.h"

namespace VL
{

// 控制台 spawn 请求在投递前完成的纯数据校验；执行端复用同一函数，
// 保证脚本入口与手输命令的拒绝条件一致。
inline bool ValidateParticleSpawnRequest(
    std::string_view name,
    std::string_view assetPath,
    const Eigen::Vector3f& position,
    std::string& outError)
{
    if (name.empty())
    {
        outError =
            "Particle spawn requires a non-empty scene object name.";
        return false;
    }
    if (assetPath.empty())
    {
        outError =
            "Particle spawn requires a non-empty particle asset path.";
        return false;
    }
    if (!position.allFinite())
    {
        outError =
            "Particle spawn position must be finite.";
        return false;
    }
    return true;
}

// 运行时 spawn 的对象只有平移摆放；旋转/缩放留给场景作者与编辑器。
inline Eigen::Matrix4f BuildParticleSpawnModel(
    const Eigen::Vector3f& position)
{
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model.block<3, 1>(0, 3) = position;
    return model;
}

// 四个运行时参数名由 Console 与执行端共用；未知名字必须被拒绝，
// 不能静默落到某个默认分支。
inline bool ValidateParticleParameterName(
    std::string_view name,
    std::string& outError)
{
    if (name == "spawnRateScale" || name == "colorTint" ||
        name == "emitter" || name == "reset")
    {
        outError.clear();
        return true;
    }
    outError =
        "Unknown particle parameter: " + std::string(name) +
        " (expected spawnRateScale, colorTint, emitter or reset).";
    return false;
}

// emitter 命令的第二个参数只接受 0/1；范围校验与 World 写入分离，
// 保证 Console 与执行端在投递前后使用同一判据。
inline bool ValidateParticleEmitterEnabledValue(
    int value,
    std::string& outError)
{
    if (value != 0 && value != 1)
    {
        outError = "Particle emitter enabled value must be 0 or 1.";
        return false;
    }
    outError.clear();
    return true;
}

// F2 粒子级命令：console 与 GT 执行端共用参数校验；GPU 池范围与
// World emitter 身份由执行端继续检查。
inline bool ValidateParticleKillRequest(
    const std::string& systemName,
    const std::string& emitterName,
    int particleIndex,
    std::string& outError)
{
    outError.clear();
    if (systemName.empty() || emitterName.empty())
    {
        outError =
            "particlekill requires <system> <emitter> <particleIndex>.";
        return false;
    }
    if (particleIndex < 0)
    {
        outError = "particlekill particleIndex must be >= 0.";
        return false;
    }
    return true;
}

inline bool ValidateParticleSpawnAtRequest(
    const std::string& systemName,
    const std::string& emitterName,
    const Eigen::Vector3f& position,
    float lifetimeSeconds,
    std::string& outError)
{
    outError.clear();
    if (systemName.empty() || emitterName.empty())
    {
        outError =
            "particlespawnat requires <system> <emitter> <x> <y> <z> "
            "[lifetime].";
        return false;
    }
    if (!position.allFinite())
    {
        outError = "particlespawnat position must be finite.";
        return false;
    }
    if (!std::isfinite(lifetimeSeconds) || lifetimeSeconds < 0.0f)
    {
        outError =
            "particlespawnat lifetime must be finite and >= 0.";
        return false;
    }
    return true;
}

} // namespace VL
