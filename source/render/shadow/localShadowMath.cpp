#include "render/shadow/localShadowMath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace VL
{
namespace
{

struct RankedLocalShadowLight
{
    size_t lightIndex = 0;
    double score = 0.0;
    RuntimeId lightId = 0;
};

} // namespace

const std::array<LocalShadowOctant, 8>& LocalShadowOctantOrder()
{
    static const std::array<LocalShadowOctant, 8> octants = {{
        { -1.0f, -1.0f, -1.0f },
        {  1.0f, -1.0f, -1.0f },
        { -1.0f,  1.0f, -1.0f },
        {  1.0f,  1.0f, -1.0f },
        { -1.0f, -1.0f,  1.0f },
        {  1.0f, -1.0f,  1.0f },
        { -1.0f,  1.0f,  1.0f },
        {  1.0f,  1.0f,  1.0f }
    }};
    return octants;
}

Eigen::Matrix4f BuildOctahedralProjectionMatrix(
    const Eigen::Vector3f& lightPosition,
    uint32_t octantIndex)
{
    if (octantIndex >= LocalShadowOctantOrder().size())
    {
        throw std::out_of_range("Octahedral shadow octant index must be in [0,7]");
    }

    const LocalShadowOctant octant =
        LocalShadowOctantOrder()[octantIndex];
    const float sx = octant.sx;
    const float sy = octant.sy;
    const float sz = octant.sz;
    const Eigen::Vector3f light = lightPosition;

    Eigen::Matrix4f matrix = Eigen::Matrix4f::Zero();
    auto setRow = [&matrix, &light](
        int row,
        float x,
        float y,
        float z)
    {
        matrix(row, 0) = x;
        matrix(row, 1) = y;
        matrix(row, 2) = z;
        matrix(row, 3) = -(x * light.x() + y * light.y() + z * light.z());
    };

    if (sz > 0.0f)
    {
        // 标准八面体上半球：p_xy = d_xy / (|dx|+|dy|+|dz|)。
        // 八象限的 sx/sy/sz 只用于让 w 的全部系数为正；x/y 本身保留符号。
        setRow(0, 1.0f, 0.0f, 0.0f);
        setRow(1, 0.0f, 1.0f, 0.0f);
        setRow(3, sx, sy, sz);
    }
    else
    {
        // 标准八面体下半球折叠：p_x = sign(x)(1-|y|/L1)，等价于
        // (dx - sx*dz)/L1；y 同理。这样和采样端 FoldOctahedralHemisphere
        // 的 (1-|value.yx|) 规则逐像素一致。
        setRow(0, 1.0f, 0.0f, -sx);
        setRow(1, 0.0f, 1.0f, -sy);
        setRow(3, sx, sy, sz);
    }

    return matrix;
}

LocalShadowSpotMatrices BuildSpotShadowMatrices(
    const Eigen::Vector3f& lightPosition,
    const Eigen::Vector3f& lightDirection,
    float coneAngleOuterRadians,
    float nearCm,
    float farCm)
{
    Eigen::Vector3f direction = lightDirection;
    if (direction.squaredNorm() <= 1.0e-12f)
    {
        direction = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    }
    direction.normalize();

    // 必须与 localShadowSampling.glsl 的 BuildLocalSpotShadowWorldToNdc
    // 完全一致：灯方向是传播方向，相机前向取反；upHint 在接近竖直时切到 +X。
    const Eigen::Vector3f viewForward = -direction;
    const Eigen::Vector3f upHint =
        std::abs(viewForward.y()) > 0.99f
        ? Eigen::Vector3f::UnitX()
        : Eigen::Vector3f::UnitY();
    const Eigen::Vector3f right =
        upHint.cross(viewForward).normalized();
    const Eigen::Vector3f cameraUp =
        viewForward.cross(right).normalized();

    LocalShadowSpotMatrices result;
    result.view <<
        right.x(), right.y(), right.z(), -right.dot(lightPosition),
        cameraUp.x(), cameraUp.y(), cameraUp.z(), -cameraUp.dot(lightPosition),
        viewForward.x(), viewForward.y(), viewForward.z(), -viewForward.dot(lightPosition),
        0.0f, 0.0f, 0.0f, 1.0f;

    const float safeNear = std::max(nearCm, 0.001f);
    const float safeFar = std::max(farCm, safeNear + 0.1f);
    // 场景的 coneAngleOuter 是半锥角；聚光 FOV = 2 * 半锥角，
    // 因此 tan(FOV/2) 的分母直接使用 coneAngleOuter。
    const float halfFov = std::clamp(
        coneAngleOuterRadians,
        0.001f,
        1.5533431f);
    const float n = -safeNear;
    const float f = -safeFar;
    const float scale = -1.0f / std::tan(halfFov);

    Eigen::Matrix4f projection = Eigen::Matrix4f::Zero();
    projection <<
        scale, 0.0f, 0.0f, 0.0f,
        0.0f, scale, 0.0f, 0.0f,
        0.0f, 0.0f, -(n + f) / (n - f), 2.0f * n * f / (n - f),
        0.0f, 0.0f, -1.0f, 0.0f;

    // 采样端等价于 NdcMatrix * Projection：Y 翻转 + z 从 OpenGL [-1,1]
    // 映射到 Vulkan [0,1]。这里保留同一展开，避免两端各自解释矩阵。
    Eigen::Matrix4f ndc = Eigen::Matrix4f::Identity();
    ndc(1, 1) = -1.0f;
    ndc(2, 2) = -0.5f;
    ndc(2, 3) = 0.5f;
    result.projection = ndc * projection;
    return result;
}

float ComputeLocalShadowFar(float lightRadius)
{
    if (!std::isfinite(lightRadius) || lightRadius <= 0.0f)
    {
        return kLocalShadowFallbackFarCm;
    }
    return std::max(2.0f, lightRadius * 1.05f);
}

std::vector<size_t> RankLocalShadowLights(
    const std::vector<LightSnapshot>& lights,
    const Eigen::Vector3f& cameraPosition,
    uint32_t maxCount)
{
    std::vector<RankedLocalShadowLight> ranked;
    ranked.reserve(lights.size());

    for (size_t index = 0; index < lights.size(); ++index)
    {
        const LightSnapshot& light = lights[index];
        if (!light.castShadows ||
            (light.type != LightSnapshotType::Point &&
             light.type != LightSnapshotType::Spot))
        {
            continue;
        }

        const float radius = light.radius > 0.0f
            ? light.radius : kLocalShadowFallbackFarCm;
        const float distance =
            (light.position - cameraPosition).norm();
        const double normalizedDistance =
            static_cast<double>(distance) /
            static_cast<double>(std::max(radius, 1.0f));
        const double intensity =
            std::max(static_cast<double>(light.intensity), 0.0);
        RankedLocalShadowLight candidate;
        candidate.lightIndex = index;
        candidate.score =
            intensity / (1.0 + normalizedDistance * normalizedDistance);
        candidate.lightId = light.lightId;
        ranked.push_back(candidate);
    }

    std::stable_sort(
        ranked.begin(),
        ranked.end(),
        [](const RankedLocalShadowLight& lhs, const RankedLocalShadowLight& rhs)
        {
            if (std::abs(lhs.score - rhs.score) > 1.0e-9)
            {
                return lhs.score > rhs.score;
            }
            return lhs.lightId < rhs.lightId;
        });

    if (ranked.size() > maxCount)
    {
        ranked.resize(maxCount);
    }

    std::vector<size_t> result;
    result.reserve(ranked.size());
    for (const RankedLocalShadowLight& light : ranked)
    {
        result.push_back(light.lightIndex);
    }
    return result;
}

} // namespace VL
