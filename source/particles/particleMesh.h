#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include <Eigen/Dense>

namespace VL
{

// mesh particle v1 的朝向模式。world 保留发射器模型旋转/缩放，camera 使用
// 相机基，velocity 让网格局部 +Z 对齐粒子速度。
inline bool IsParticleMeshAlignment(std::string_view alignment)
{
    return alignment == "world" || alignment == "camera" ||
        alignment == "velocity";
}

inline int ParticleMeshAlignmentCode(std::string_view alignment)
{
    if (alignment == "camera")
    {
        return 1;
    }
    if (alignment == "velocity")
    {
        return 2;
    }
    return 0;
}

inline bool IsValidParticleMeshRelativePath(const std::string& path)
{
    if (path.empty() || path.find('\0') != std::string::npos)
    {
        return false;
    }
    std::string portable = path;
    for (char& character : portable)
    {
        if (character == '\\')
        {
            character = '/';
        }
    }
    const std::filesystem::path parsed(portable);
    if (parsed.is_absolute() || parsed.has_root_name() ||
        parsed.has_root_directory())
    {
        return false;
    }
    for (const std::filesystem::path& component : parsed)
    {
        if (component == "..")
        {
            return false;
        }
    }
    std::string extension = parsed.extension().string();
    for (char& character : extension)
    {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return extension == ".json";
}

inline void RequireValidParticleMeshPath(
    const std::string& path,
    std::string_view fieldName = "mesh.path")
{
    if (!IsValidParticleMeshRelativePath(path))
    {
        throw std::invalid_argument(
            "Particle " + std::string(fieldName) +
            " must be a resource-root-relative .json path: " + path);
    }
}

inline Eigen::Vector3f SafeNormalizeParticleMeshAxis(
    const Eigen::Vector3f& value,
    const Eigen::Vector3f& fallback)
{
    const float length = value.norm();
    if (!value.allFinite() || !std::isfinite(length) ||
        length <= 1.0e-6f)
    {
        return fallback;
    }
    return value / length;
}

// 与 particleMesh.vert 保持同一套基向量约定。返回值三列分别是局部
// X/Y/Z 在世界空间的方向；camera 与 velocity 基始终正交且右手系。
inline Eigen::Matrix3f ComputeParticleMeshBasis(
    std::string_view alignment,
    const Eigen::Vector3f& velocity,
    const Eigen::Matrix4f& localToWorld,
    const Eigen::Vector3f& cameraRight,
    const Eigen::Vector3f& cameraUp,
    const Eigen::Vector3f& cameraForward)
{
    const Eigen::Matrix3f world =
        localToWorld.block<3, 3>(0, 0);
    if (alignment == "world")
    {
        return world;
    }

    const Eigen::Vector3f right =
        SafeNormalizeParticleMeshAxis(
            cameraRight,
            Eigen::Vector3f::UnitX());
    const Eigen::Vector3f up =
        SafeNormalizeParticleMeshAxis(
            cameraUp,
            Eigen::Vector3f::UnitY());
    const Eigen::Vector3f forward =
        SafeNormalizeParticleMeshAxis(
            cameraForward,
            -Eigen::Vector3f::UnitZ());
    // 局部 +Z 朝向相机：相机 forward 是视线方向，因此取 -forward。
    Eigen::Matrix3f cameraBasis;
    cameraBasis.col(0) = right;
    cameraBasis.col(1) = up;
    cameraBasis.col(2) = -forward;
    if (alignment != "velocity")
    {
        return cameraBasis;
    }

    const Eigen::Vector3f velocityForward =
        SafeNormalizeParticleMeshAxis(
            velocity,
            Eigen::Vector3f::Zero());
    if (velocityForward.squaredNorm() <= 1.0e-12f)
    {
        return cameraBasis;
    }
    const Eigen::Vector3f velocityRight =
        Eigen::Vector3f::UnitY().cross(velocityForward);
    if (velocityRight.squaredNorm() <= 1.0e-12f)
    {
        // 速度与世界上方平行时 right 未定义，沿用相机基避免 NaN。
        return cameraBasis;
    }
    const Eigen::Vector3f rightAxis =
        velocityRight.normalized();
    const Eigen::Vector3f upAxis =
        velocityForward.cross(rightAxis).normalized();
    Eigen::Matrix3f velocityBasis;
    velocityBasis.col(0) = rightAxis;
    velocityBasis.col(1) = upAxis;
    velocityBasis.col(2) = velocityForward;
    return velocityBasis;
}

inline uint64_t ParticleMeshDrawnVertexCount(
    uint32_t meshIndexCount,
    uint32_t instanceCount)
{
    return static_cast<uint64_t>(meshIndexCount) *
        static_cast<uint64_t>(instanceCount);
}

} // namespace VL
