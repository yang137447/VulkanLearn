#pragma once

#include <string>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"
#include "scene/sceneAssetTypes.h"
#include "terrain/terrainCollisionTypes.h"

namespace VL
{

// 从已校验的场景计划里提取每个 terrain 摆放的碰撞数据。
//
// 这是 WorldBuilder 与测试共用的唯一来源：地形几何/高度图来自
// SceneAssetObjectPlan::terrainBuildPlan（场景预检产物），摆放 TRS 来自作者
// 场景 JSON。渲染器的 chunk 计划不在输入里，避免把“渲染派生数据”当成
// 碰撞的第二份真值。
//
// 任何 terrain 对象缺少地形计划、TRS 非法（非有限、scale <= 0）或
// placementId 重复都会返回失败；调用方在 World 加载期终止，而不是拖到
// Play 时才创建 body。
RuntimeResult<std::vector<TerrainCollisionInstance>> BuildTerrainCollisionInstances(
    const SceneAssetBuildPlan& sceneAssetPlan,
    const nlohmann::json& sceneJson);

} // namespace VL
