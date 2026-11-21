#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace VL
{

// mode 1 的单块地形权重纹理守卫（纯 CPU，不依赖渲染资源）。
// loader 传最终消费的 layerWeightMaps.get()；作者/overlay 跨 placement 共享非法，
// 已由 Service 验证闭包与映射的不可变 publication 可以共享真实 Texture。
class TerrainWeightMapBindingTracker
{
public:
    enum class BindingPolicy
    {
        PlacementOwned,
        VerifiedImmutablePublication
    };

    void Record(
        const void* weightsIdentity,
        std::string materialPath,
        std::string placementId,
        BindingPolicy policy = BindingPolicy::PlacementOwned);

    void RecordFrozenOverlay(
        const void* importedWeightsIdentity,
        const void* frozenWeightsIdentity,
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
        bool immutablePublicationOnly = true;
    };

    std::vector<Binding> bindings;
};

} // namespace VL
