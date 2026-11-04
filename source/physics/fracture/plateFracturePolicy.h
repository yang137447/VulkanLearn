#pragma once

#include <cstddef>
#include <vector>

#include "physics/fracture/destructibleProfile.h"
#include "physics/fracture/planarFracture.h"

namespace VL::Physics::Fracture
{

double ComputePlateDamageRadius(double width, double height,
    float impactSpeed, const DestructibleProfile& profile);

bool IsPlateShardAttached(double distanceFromImpact, double damageRadius,
    const DestructibleProfile& profile);

struct SubfractureDecision
{
    bool split = false;
    int crackCount = 0;
};

// projectileImpact=true 使用箭头阈值；落地/碎片互撞使用更高阈值。
SubfractureDecision EvaluateSubfracture(double worldArea, int depth,
    float impactSpeed, bool projectileImpact,
    const DestructibleProfile& profile);

// 输入 polygon 是板面局部坐标，worldArea 是世界 cm²。返回需要转为动态
// 掉落的 attached 板片下标；未锚定或总残余面积过小的连通岛都会返回。
std::vector<std::size_t> FindUnsupportedPlateIndices(
    const std::vector<Polygon2>& paneOutlines,
    const std::vector<double>& worldAreas,
    double width, double height, double scale,
    const DestructibleProfile& profile);

} // namespace VL::Physics::Fracture
