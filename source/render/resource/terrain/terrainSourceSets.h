#pragma once

#include <memory>

class Texture;

namespace VL
{
class RendererBackendVulkan;
struct TerrainBuildPlan;
struct TerrainStreamingState;

// S2：把一个地形 MI 绑定的三张层数组登记到它用到的每个 sourceSet。
// 同一 set 必须始终指向同一组 shared_ptr；不同 set 可以指向不同数组，但
// 仍共享同一组 atlas，因此三张图的格式必须一致。
void RegisterTerrainSourceSets(
    RendererBackendVulkan& rendererBackend,
    TerrainStreamingState& streamingState,
    const TerrainBuildPlan& plan,
    const std::shared_ptr<Texture>& layerAlbedo,
    const std::shared_ptr<Texture>& layerNormal,
    const std::shared_ptr<Texture>& layerPbr,
    bool vtRequested);

} // namespace VL
