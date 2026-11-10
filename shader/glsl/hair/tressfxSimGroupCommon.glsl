#ifndef VL_TRESSFX_SIM_GROUP_COMMON_GLSL
#define VL_TRESSFX_SIM_GROUP_COMMON_GLSL

// 分组模拟参数表。C++ 侧来源：source/hair/tressfxGroupSimulation.h
// index 0 = base + placement，k = base + 第 k 组 overrides + placement。
layout(set = 0, binding = 17, std430) readonly buffer GuideGroupSimulationConfigs
{
    // 每组 8 个 float：damping, bendStiffness, globalShapeStiffness,
    // globalShapeEffectiveRange, vspCoefficient, vspAccelThresholdMax, pad, pad。
    float values[];
} groupSimulationConfigs;

// 与图形 Set 2 binding 5 是同一份 per-guide group id；compute 里重新绑定。
layout(set = 0, binding = 18, std430) readonly buffer GuideGroupIds
{
    uint ids[];
} guideGroupIds;

struct TressfxGroupSimulationValues
{
    float damping;
    float bendStiffness;
    float globalShapeStiffness;
    float globalShapeEffectiveRange;
    float vspCoefficient;
    float vspAccelThresholdMax;
};

// 同一根 guide 的所有 point 必须使用同一组；缺失/越界回退组 0。
uint TressfxGroupSimulationIndex(uint guide)
{
    uint count = groupSimulationConfigs.values.length() / 8u;
    uint id = guide < guideGroupIds.ids.length() ? guideGroupIds.ids[guide] : 0u;
    return id < count ? id : 0u;
}

TressfxGroupSimulationValues TressfxGroupSimulation(uint guide)
{
    uint base = TressfxGroupSimulationIndex(guide) * 8u;
    TressfxGroupSimulationValues values;
    values.damping = groupSimulationConfigs.values[base + 0u];
    values.bendStiffness = groupSimulationConfigs.values[base + 1u];
    values.globalShapeStiffness = groupSimulationConfigs.values[base + 2u];
    values.globalShapeEffectiveRange = groupSimulationConfigs.values[base + 3u];
    values.vspCoefficient = groupSimulationConfigs.values[base + 4u];
    values.vspAccelThresholdMax = groupSimulationConfigs.values[base + 5u];
    return values;
}

#endif // VL_TRESSFX_SIM_GROUP_COMMON_GLSL
