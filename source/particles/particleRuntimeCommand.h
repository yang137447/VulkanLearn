#pragma once

#include <string>
#include <string_view>

#include <Eigen/Dense>

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

} // namespace VL
