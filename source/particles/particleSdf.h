#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

namespace VL
{

// 有限体素 SDF 资产（碰撞用）。分辨率上限 64^3，距离单位 cm，
// 负值表示实体内。数组按 x 最快、z 最慢排列：index =
// x + resolution.x * (y + resolution.y * z)。
struct ParticleSdfAsset
{
    Eigen::Vector3i resolution = Eigen::Vector3i(8, 8, 8);
    Eigen::Vector3f boundsMin = Eigen::Vector3f(-50.0f, -50.0f, -50.0f);
    Eigen::Vector3f boundsMax = Eigen::Vector3f(50.0f, 50.0f, 50.0f);
    std::vector<float> distances;

    bool Empty() const noexcept { return distances.empty(); }

    Eigen::Vector3f CellSize() const noexcept
    {
        Eigen::Vector3f size =
            (boundsMax - boundsMin).array() /
            (resolution.array() - 1).cast<float>();
        return size;
    }

    // 返回网格坐标；调用方已保证在 bounds 内。
    Eigen::Vector3f GridPosition(const Eigen::Vector3f& position) const
    {
        Eigen::Vector3f normalized =
            (position - boundsMin).array() /
            (boundsMax - boundsMin).array();
        return normalized.array() *
            (resolution.array() - 1).cast<float>();
    }

    float Voxel(int x, int y, int z) const noexcept
    {
        const int index =
            x +
            resolution.x() * (y + resolution.y() * z);
        return distances[static_cast<size_t>(index)];
    }

    // 三线性采样。bounds 外返回 +infinity，表示不与实体相交。
    float SampleDistance(const Eigen::Vector3f& position) const
    {
        if (distances.empty() || !position.allFinite())
        {
            return std::numeric_limits<float>::infinity();
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (position[axis] < boundsMin[axis] ||
                position[axis] > boundsMax[axis])
            {
                return std::numeric_limits<float>::infinity();
            }
        }

        const Eigen::Vector3f grid = GridPosition(position);
        Eigen::Vector3i base;
        Eigen::Vector3f fraction;
        for (int axis = 0; axis < 3; ++axis)
        {
            base[axis] = std::clamp(
                static_cast<int>(std::floor(grid[axis])),
                0,
                resolution[axis] - 2);
            fraction[axis] = std::clamp(
                grid[axis] - static_cast<float>(base[axis]),
                0.0f,
                1.0f);
        }

        float result = 0.0f;
        for (int z = 0; z < 2; ++z)
        {
            for (int y = 0; y < 2; ++y)
            {
                for (int x = 0; x < 2; ++x)
                {
                    const float weight =
                        (x == 0 ? 1.0f - fraction.x() : fraction.x()) *
                        (y == 0 ? 1.0f - fraction.y() : fraction.y()) *
                        (z == 0 ? 1.0f - fraction.z() : fraction.z());
                    result += weight * Voxel(
                        base.x() + x,
                        base.y() + y,
                        base.z() + z);
                }
            }
        }
        return result;
    }

    // 中心差分梯度；越界处退化为 0，碰撞求解会保持位置不变。
    Eigen::Vector3f SampleGradient(const Eigen::Vector3f& position) const
    {
        const Eigen::Vector3f cell = CellSize();
        Eigen::Vector3f gradient = Eigen::Vector3f::Zero();
        for (int axis = 0; axis < 3; ++axis)
        {
            Eigen::Vector3f offset = Eigen::Vector3f::Zero();
            offset[axis] = cell[axis];
            // 边界处把采样点 clamp 回 bounds，避免中心差分在网格外
            // 退化成 inf 而丢失梯度方向。
            Eigen::Vector3f beforePosition =
                (position - offset).array().max(boundsMin.array()).min(
                    boundsMax.array());
            Eigen::Vector3f afterPosition =
                (position + offset).array().max(boundsMin.array()).min(
                    boundsMax.array());
            const float before = SampleDistance(beforePosition);
            const float after = SampleDistance(afterPosition);
            if (std::isfinite(before) && std::isfinite(after))
            {
                // clamp 会缩短实际差分跨度，用真实跨度作分母，避免
                // 边界/中心处的梯度被系统性缩小。
                const float span =
                    afterPosition[axis] - beforePosition[axis];
                if (span > 1.0e-6f)
                {
                    gradient[axis] = (after - before) / span;
                }
            }
        }
        return gradient;
    }
};

inline ParticleSdfAsset ParseParticleSdfJson(
    const nlohmann::json& json)
{
    if (!json.is_object() ||
        json.value("type", std::string()) != "particleSdf")
    {
        throw std::invalid_argument(
            "Particle SDF asset requires type=particleSdf");
    }

    ParticleSdfAsset sdf;
    if (!json.contains("resolution") ||
        !json.at("resolution").is_array() ||
        json.at("resolution").size() != 3)
    {
        throw std::invalid_argument(
            "Particle SDF requires resolution [x,y,z]");
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        const nlohmann::json& value = json.at("resolution").at(axis);
        if (!value.is_number_integer())
        {
            throw std::invalid_argument(
                "Particle SDF resolution must be integers");
        }
        sdf.resolution[axis] = value.get<int>();
        if (sdf.resolution[axis] < 2 || sdf.resolution[axis] > 64)
        {
            throw std::invalid_argument(
                "Particle SDF resolution must be in [2, 64]");
        }
    }

    if (!json.contains("boundsMin") || !json.contains("boundsMax") ||
        !json.at("boundsMin").is_array() ||
        !json.at("boundsMax").is_array() ||
        json.at("boundsMin").size() != 3 ||
        json.at("boundsMax").size() != 3)
    {
        throw std::invalid_argument(
            "Particle SDF requires boundsMin/boundsMax arrays");
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        const nlohmann::json& minimum = json.at("boundsMin").at(axis);
        const nlohmann::json& maximum = json.at("boundsMax").at(axis);
        if (!minimum.is_number() || !maximum.is_number() ||
            !std::isfinite(minimum.get<float>()) ||
            !std::isfinite(maximum.get<float>()))
        {
            throw std::invalid_argument(
                "Particle SDF bounds must be finite");
        }
        sdf.boundsMin[axis] = minimum.get<float>();
        sdf.boundsMax[axis] = maximum.get<float>();
        if (sdf.boundsMax[axis] <= sdf.boundsMin[axis])
        {
            throw std::invalid_argument(
                "Particle SDF boundsMax must be greater than boundsMin");
        }
    }

    if (!json.contains("distances") ||
        !json.at("distances").is_array())
    {
        throw std::invalid_argument(
            "Particle SDF requires a distances array");
    }
    const size_t expected =
        static_cast<size_t>(sdf.resolution.x()) *
        static_cast<size_t>(sdf.resolution.y()) *
        static_cast<size_t>(sdf.resolution.z());
    const nlohmann::json& distances = json.at("distances");
    if (distances.size() != expected)
    {
        throw std::invalid_argument(
            "Particle SDF distances count does not match resolution");
    }
    sdf.distances.resize(expected);
    for (size_t index = 0; index < expected; ++index)
    {
        const nlohmann::json& value = distances.at(index);
        if (!value.is_number() ||
            !std::isfinite(value.get<float>()))
        {
            throw std::invalid_argument(
                "Particle SDF distance must be finite");
        }
        sdf.distances[index] = value.get<float>();
    }
    return sdf;
}

} // namespace VL
