#pragma once

#include <cstdint>
#include <vector>

#include <Eigen/Dense>

// Guide 抽稀与 follow 绑定（路线 B）：从完整 groom 中选出模拟 guide，其余发丝
// 作为 follow，按最近模拟 guide 的逐点 rest frame 存 offset。运行时用当前
// guide frame 旋转同一 offset，静止时逐点还原作者造型。
//
// GLSL 侧的 frame 构造（tressfxSimFollowInterpolation.comp）必须与本文件的
// TressfxMakeFollowFrame / TressfxChooseFollowReference 保持同一分支与同一次序。
namespace VL::Hair
{
struct TressfxFollowFrame
{
    Eigen::Vector3f x = Eigen::Vector3f::UnitX();
    Eigen::Vector3f y = Eigen::Vector3f::UnitY();
    Eigen::Vector3f z = Eigen::Vector3f::UnitZ();
};

// 选与切线最不平行的世界轴；零切线退化为 +Y。返回单位轴。
Eigen::Vector3f TressfxChooseFollowReference(const Eigen::Vector3f& tangent);

// x = normalize(tangent)，y = normalize(reference - x*dot)，z = cross(x,y)。
// reference 与切线平行时用 TressfxChooseFollowReference 重新选轴，避免 NaN。
TressfxFollowFrame TressfxMakeFollowFrame(
    const Eigen::Vector3f& tangent, const Eigen::Vector3f& reference);

struct TressfxFollowBinding
{
    // 参与模拟的原始 guide 索引，升序。
    std::vector<uint32_t> simulationGuideIndices;
    // 每根原始 guide 的模拟来源：自身（参与模拟）或最近模拟 guide 的原始索引。
    std::vector<uint32_t> followGuideIndex;
    // 逐点 offset（guideCount * pointsPerGuide，xyz 有效）；在来源 guide 的
    // rest frame 下表达。参与模拟的 guide 对应项为 0。
    std::vector<Eigen::Vector4f> offsets;
    // 每根原始 guide 使用的参考轴（xyz 有效），来自其模拟来源 guide。
    std::vector<Eigen::Vector4f> frameReferences;
};

// 绑定候选数：先按根点取最近 N 根模拟 guide，再在其中选整根 rest 形状最接近的。
// 只按根点最近会把长发绑到形状差很远的 guide；实测 16 候选把 offset p95 从
// 10.6 cm 降到 1.7 cm（40,334 根、2,000 guides）。
inline constexpr uint32_t TressfxFollowCandidateCount = 16;

// simulationGuideCount == 0 或 >= guideCount 时返回空结构（全部参与模拟）。
// 选择用最远点采样（FPS，种子为最靠近根点质心的 guide），绑定用候选内
// 整根 rest 均方距离最小者。
// 输入尺寸或点数不合法时抛 std::invalid_argument。
TressfxFollowBinding BuildTressfxFollowBinding(
    const std::vector<Eigen::Vector3f>& rootPositions,
    const std::vector<Eigen::Vector3f>& restPositions,
    uint32_t pointsPerGuide,
    uint32_t simulationGuideCount);
} // namespace VL::Hair
