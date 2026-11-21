#pragma once

#include <cstddef>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "editor/terrain/terrainEditSession.h"
#include "terrain/terrainCompositedRefresh.h"

namespace VL
{

class World;
class RendererBackendVulkan;
namespace Editor { class TerrainCompositedSculptApply; }

namespace Editor
{
struct TerrainCompositedRefreshReceipt
{
    TerrainCompositedRefreshCommand command;
    TerrainCompositedRefreshTicket ticket;
    uint64_t sessionEpoch = 0;
    uint32_t attempts = 1;
    TerrainCompositedRefreshState state = TerrainCompositedRefreshState::Queued;
    bool terminalFailure = false;

    bool Acknowledges(const TerrainCompositedRefreshStatus& status,
        uint64_t generation, uint64_t epoch, uint64_t sourceRevision) const noexcept
    {
        return status.Acknowledges(ticket) && command.worldGeneration == generation &&
            command.sourceRevision == sourceRevision && sessionEpoch == epoch &&
            status.publishedRevision > command.expectedRevision;
    }
};
}

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

// GT 稳定边界：冻结 World 高度/Bounds，通过回调排队 RefreshPlacement。
// 不触碰 GPU/cache/pageManager；RT 每个实际 frame 调用 sculpt.ReplayForFrame。
bool ApplyCompositedTerrainSculptUpdate(
    World& world,
    Editor::TerrainCompositedSculptApply& sculpt,
    const Editor::TerrainMeshUpdate& update,
    const std::function<void(const TerrainBuildPlan&)>& refreshPlacement,
    TerrainEditApplyStats& stats,
    std::string& error);

bool ApplyCompositedTerrainSculptUpdate(
    World& world,
    Editor::TerrainCompositedSculptApply& sculpt,
    const Editor::TerrainMeshUpdate& update,
    const std::function<void(const TerrainBuildPlan&, const TerrainCompositedDirtyChange&, uint64_t)>& refreshPlacement,
    TerrainEditApplyStats& stats,
    std::string& error);

} // namespace VL
