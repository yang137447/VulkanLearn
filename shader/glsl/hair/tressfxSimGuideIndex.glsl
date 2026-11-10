#ifndef VL_TRESSFX_SIM_GUIDE_INDEX_GLSL
#define VL_TRESSFX_SIM_GUIDE_INDEX_GLSL

// 路线 B：实际参与模拟的 guide 原始索引。Host 在 simulationGuideCount=0 时
// 上传 0..guideCount-1 的恒等表，因此所有模拟 pass 可以无条件走 slot 映射。
// 只被需要按 guide 调度的 pass 包含；把 binding 19 留给这些 pass 的 layout。
layout(set = 0, binding = 19, std430) readonly buffer TressfxSimulationGuideIndices
{
    uint indices[];
} tressfxSimulationGuides;

uint TressfxSimulationGuideCount()
{
    return uint(params.windParams.y);
}

uint TressfxSimulationGuide(uint slot)
{
    return tressfxSimulationGuides.indices[slot];
}

// 点级 pass：slotPoint 是紧凑 dispatch 的线性点下标，返回原始 guide 内点下标。
uint TressfxSimulationPoint(uint slotPoint, uint pointsPerGuide, out uint guide)
{
    guide = TressfxSimulationGuide(slotPoint / pointsPerGuide);
    return guide * pointsPerGuide + slotPoint % pointsPerGuide;
}
#endif
