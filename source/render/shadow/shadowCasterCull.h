#pragma once

#include <array>
#include <cstdint>

#include <Eigen/Dense>

namespace VL
{

// 单个阴影 pass 的剔除计数。Before = 路由为 shadow caster 的 draw packet 数，
// After = 实际提交的 draw packet 数；两者相差即本 pass 省下的绘制。
struct ShadowCasterCullStats
{
    uint32_t cascadeIndex = 0;
    uint32_t casterDraws = 0;
    uint32_t submittedDraws = 0;
};

// 该级联接收面（相机 split）在光源空间的 XY 包围盒。方向光沿光源空间 -Z
// 传播，ray 上的点到光源空间 XY 的投影保持不变，因此 caster 的 XY 与接收面
// XY 不相交时，它不可能把阴影投到该级联内。这比拟合出的正交视锥更紧，同时
// 对方向光仍是精确的保守判据。
struct ShadowCasterReceiverBounds
{
    Eigen::Matrix3f worldToLight = Eigen::Matrix3f::Identity();
    // (minX, minY, maxX, maxY)，光源空间单位。
    Eigen::Vector4f lightSpaceXyMinMax = Eigen::Vector4f::Zero();
    // false 表示该级联没有有效的接收面范围（例如场景没有方向光），调用方保留全部 draw。
    bool valid = false;
};

// 返回 true 表示 XY 范围仍有重叠（保留 draw）；false 表示可以安全跳过。
// 非有限/退化输入一律返回 true。
bool ShadowCasterBoundsTouchReceiverLightSpaceXy(
    const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax,
    const ShadowCasterReceiverBounds& receiver,
    float padding = 0.0f);

// 由某一级联自己的 light view-projection 导出的 caster 剔除体。
//
// 这里刻意不复用主相机 split 的快速测试：阴影 caster 只需要排除完全落在
// 该级联光源视锥外的对象，主相机视锥会把“在级联内但不在相机 slice 内”的
// 合法 caster 也排除掉。平面直接取自同一份 lightViewProj，因此与真正写入
// 该层 shadow map 的变换严格一致。
//
// 保守性约定（错剔是可见 bug，多画只损失时间）：
// - 矩阵无效/非有限时整级联不剔除；
// - 包围盒非有限、min > max 或退化时保留；
// - 只有 AABB 完全落在某个平面负半空间时才剔除；相切或跨平面都保留；
// - 调用方可用 padding 沿世界轴外扩，吸收浮点误差。
class ShadowCasterCullFrustum
{
public:
    // Vulkan clip space：x/y ∈ [-w, w]，z ∈ [0, w]；与 Camera::GetProjectionMatrix
    // 的 ndcMatrix 约定一致。返回 false 表示矩阵退化，调用方必须保留全部 caster。
    bool Build(const Eigen::Matrix4f& lightViewProjection);

    bool IsValid() const { return valid; }

    // bounds 是当前帧 draw packet 的世界空间 AABB。蒙皮对象在 World 动画更新时
    // 已把 localBounds 换成当前 skinPose 的边界，因此这里不需要、也不能回退到
    // bind pose。padding 单位为世界单位（当前场景为厘米）。
    // 返回 true 表示“保留”；false 表示可以安全跳过。
    bool IntersectsWorldBounds(
        const Eigen::Vector3f& boundsMin,
        const Eigen::Vector3f& boundsMax,
        float padding = 0.0f) const;

private:
    struct Plane
    {
        Eigen::Vector3f normal = Eigen::Vector3f::Zero();
        float offset = 0.0f;
    };

    std::array<Plane, 6> planes{};
    bool valid = false;
};

// 世界单位下的小幅外扩。当前场景坐标在 0..20000 cm 量级，float 在该量级的
// 分辨率约 0.002 cm；0.05 cm 足以覆盖平面/包围盒运算误差，又不会改变可见结果。
inline constexpr float kShadowCasterCullPadding = 0.05f;

} // namespace VL
