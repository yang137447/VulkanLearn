#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace VL
{

// mode 1 的单块地形权重纹理守卫（纯 CPU，不依赖渲染资源）。
// 同一个 weightsIdentity（loader 传 layerWeightMaps.get()）被多个 placement
// 使用时，所有 tile 会采样同一份掩码，必须在 World 加载期直接失败。
class TerrainWeightMapBindingTracker
{
public:
    void Record(
        const void* weightsIdentity,
        std::string materialPath,
        std::string placementId);

    // 空串表示合法；否则返回按插入顺序找到的第一个共享组。
    std::string SharedBindingError() const;

    // 重置跟踪状态，供新的 World 候选复用宿主状态。
    void Clear();

    // 去重后的 (weightsIdentity, placementId) 记录总数。
    size_t PlacementCount() const;

private:
    struct Binding
    {
        const void* weightsIdentity = nullptr;
        std::string materialPath;
        std::vector<std::string> placements;
    };

    std::vector<Binding> bindings;
};

} // namespace VL
