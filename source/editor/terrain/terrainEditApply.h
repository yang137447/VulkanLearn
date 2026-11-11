#pragma once

#include <cstddef>
#include <string>

#include "editor/terrain/terrainEditSession.h"

namespace VL
{

class World;
class RendererBackendVulkan;

struct TerrainEditApplyStats
{
    size_t renderableObjects = 0;
    size_t worldChunks = 0;
};

// 只能在 render thread idle 的稳定帧边界调用：RenderableObject 构造会立即
// 上传/替换 GPU buffer，替换旧对象时也不能与正在录制/提交的命令缓冲重叠。
// 本函数不重建 RenderGraph/材质，也不修改 TerrainStreamingState 页表。
bool ApplyTerrainMeshUpdate(
    World& world,
    RendererBackendVulkan& backend,
    const Editor::TerrainMeshUpdate& update,
    TerrainEditApplyStats& stats,
    std::string& error);

} // namespace VL
