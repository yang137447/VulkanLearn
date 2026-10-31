#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "hair/tressfxBehavior.h"
#include "mesh/meshAssetTypes.h"

namespace VL::Hair
{

// groups[].simulationOverrides 的唯一合法字段集。CPU 与 GPU 都消费同一份
// 解析结果；其他模拟字段属于资产/放置对象级参数，不允许按组覆盖。
inline constexpr std::array<std::string_view, 6> TressfxGroupSimulationOverrideFields = {
    "damping", "bendStiffness", "globalShapeStiffness",
    "globalShapeEffectiveRange", "vspCoefficient", "vspAccelThresholdMax"};

bool IsTressfxGroupSimulationOverrideField(std::string_view field);

// 组号选择规则（CPU/GPU/测试共用）：0 = 默认组；命名组 k = 声明序第 k 组。
// guideGroupId 越界、配置表为空或缺失时回退到索引 0。
uint32_t SelectTressfxGroupSimulationIndex(
    uint32_t guideGroupId, size_t configCount);

// 分层解析唯一入口：index 0 = base + placement，index k = base + 第 k 组
// overrides + placement。placement 对每个组最后应用，因此组不能压过关卡放置。
// groups 为空时只返回索引 0。返回数组长度 = groups.size() + 1。
std::vector<TressfxSimulationConfig> ResolveTressfxGroupSimulationConfigs(
    const TressfxSimulationConfig& base,
    const std::vector<TressfxGuideGroupPlan>& groups,
    const TressfxPlacementBehavior& placement,
    const std::string& context = {});

// GPU compute 侧每组的 8 个 float。调用方必须按 8 float 步长上传：
// [0]=damping [1]=bendStiffness [2]=globalShapeStiffness
// [3]=globalShapeEffectiveRange [4]=vspCoefficient
// [5]=vspAccelThresholdMax [6..7]=保留 0。
struct alignas(16) TressfxGroupSimulationGpu
{
    float values[8] = {};
};
static_assert(sizeof(TressfxGroupSimulationGpu) == 32,
    "TressFX group simulation GPU entry must stay 32 bytes");

TressfxGroupSimulationGpu MakeTressfxGroupSimulationGpu(
    const TressfxSimulationConfig& config);

std::vector<TressfxGroupSimulationGpu> BuildTressfxGroupSimulationGpuData(
    const std::vector<TressfxSimulationConfig>& configs,
    size_t capacity = 0);

// 分组 overrides 影响求解但不影响几何/绑定身份；该键只用于行为身份与测试。
// 只有声明了 groups（configs.size() > 1）时才产生非空键。
std::string MakeTressfxGroupSimulationKey(
    const std::vector<TressfxSimulationConfig>& configs);

} // namespace VL::Hair
