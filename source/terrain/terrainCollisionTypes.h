#pragma once

#include <string>

#include <Eigen/Dense>

#include "terrain/terrainAssetTypes.h"

namespace VL
{

// 一次地形摆放的世界变换，字段与场景对象的 position/rotation/scale 一一对应：
// position 单位 cm、rotation 单位度（Y*X*Z，与 CommonFunction::RotationToQuat 一致）、
// scale 是无量纲乘数。Jolt 世界本身用米，换算只发生在 terrainCollision 内部。
//
// 这个头文件刻意不包含 Jolt：World 只需要携带摆放数据，物理后端只在
// ScriptPhysicsWorld / terrainCollision 里出现。
struct TerrainCollisionPlacement
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f rotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f scale = Eigen::Vector3f::Ones();
};

// World 保留的一份地形碰撞摆放。plan 携带已解码高度图，placement 是作者 TRS；
// 同一 TR 多次摆放时 placementId 是唯一身份。
struct TerrainCollisionInstance
{
    std::string placementId;
    TerrainBuildPlan plan;
    TerrainCollisionPlacement placement;
};

} // namespace VL
