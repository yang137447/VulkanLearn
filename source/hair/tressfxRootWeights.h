#pragma once

#include <cstdint>
#include <vector>

#include <Eigen/Dense>

#include "core/runtimeResult.h"
#include "hair/tressfxProxySkinning.h"
#include "hair/tressfxSkinning.h"

namespace VL::Hair
{
// 从绑定姿态身体网格派生逐 guide 的根点权重。纯 CPU、只依赖值/视图，
// 不读文件、不碰 World/Vulkan：加载期的 scene/component 绑定与运行时 spawn
// 的 per-instance 绑定共用同一入口，避免两条路径各写一份转移逻辑。
struct TressfxRootWeightInput
{
    // 每根 guide 的根点，model（渲染绑定）空间；必须与 body 的位置同一空间。
    const std::vector<Eigen::Vector3f>* roots = nullptr;
    // 身体顶点位置 + CSR 影响视图；jointCount > 0 时校验关节下标上界。
    const SdfProxySkinSource* body = nullptr;
    // 最近顶点匹配上限（厘米）。只作为“毛发与身体不在同一绑定空间”的检测器，
    // 不承担“头皮顶点稀疏”的判断（稀疏只影响量化误差，不影响可匹配性）。
    float maxNearestDistanceCm = 2.0f;
};

struct TressfxRootWeightDerivation
{
    // 每个 guide 一项，顺序与 roots 一致；未匹配项 joints/weights 为空，
    // 由调用方按其策略回退 rootBone（这里不静默补权，也不改 strand 数量）。
    std::vector<TressfxStrandBoneWeights> strandBones;
    float maxNearestDistanceCm = 0.0f;
    uint32_t unmatchedCount = 0;
    uint32_t matchedCount = 0;
    // 转移表引用到的关节上界（骨架关节数）；供 GPU palette 容量校验使用。
    uint32_t jointCount = 0;
};

// 语义与加载期原实现逐条一致：每个根点匹配最近的绑定姿态身体顶点，复制其
// **完整**影响列表并归一化；不插值、不做 k 近邻、不平滑。个别根点未匹配时
// 该项留空并计入 unmatchedCount；全部未匹配才判为失败。
//
// 失败只有两类，调用方据 code 决定文案：
//   TressFXRootWeights.InvalidInput —— 根点/身体视图为空、距离非法，或转移
//                                      输入本身非法（非有限坐标等）；
//   TressFXRootWeights.NoMatch      —— 所有根点都未匹配（错空间/错角色）。
RuntimeResult<TressfxRootWeightDerivation> BuildTressfxRootWeights(
    const TressfxRootWeightInput& input);
} // namespace VL::Hair
