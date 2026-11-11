#pragma once

#include <string_view>

#include "terrain/terrainAssetTypes.h"

namespace VL
{

// TR_*.json 的唯一解析/预检入口。它在场景校验阶段读取并解码高度图，
// 让 World 候选在提交前拿到完整 CPU 地形计划；不创建 GPU 资源。
class TerrainAssetLoader
{
public:
    static TerrainBuildPlan Prepare(std::string_view terrainAssetPath);
};

} // namespace VL
