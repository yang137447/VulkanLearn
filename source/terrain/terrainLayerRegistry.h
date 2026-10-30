#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "terrain/terrainAssetTypes.h"

namespace VL
{

// 一个 World 级全局层的注册结果。globalId 就是它在 Layers() 里的下标，
// 也是 ID/权重图、页表、feedback 使用的层号。
struct TerrainGlobalLayerRecord
{
    std::string key;
    TerrainLayerDesc layer;
    // 引用该 key 的地形资产路径（按首次出现顺序；冲突诊断用）。
    std::vector<std::string> assetPaths;
    // 引用该全局层的地形摆放数量。
    uint32_t placementCount = 0;
};

// World 级全局层注册表，规则见
// documents/plan/rendering/terrain-global-layer-catalog-design.md §4.2/§5：
//   - 身份是显式 key，缺省 "<assetPath>#<name>"；sourceSet 不参与 key；
//   - 同 key 且参数一致 -> 复用同一个 globalId（跨 TR 去重）；
//   - 同 key 但参数不一致 -> 加载失败，错误里带两个 assetPath；
//   - 不同 key 即使内容相同也不合并（作者要共享必须写同一个 key）；
//   - 分配顺序 = 场景对象顺序 × 每个 TR 的 layers 顺序首次出现，
//     因此单 TR World 的 globalId 恰好等于局部下标，旧输出逐字节不变。
class TerrainLayerRegistry
{
public:
    // 与 TerrainLayerTable::MaxLayers 一致：ID/权重图是 RGBA8，单通道 8 bit。
    static constexpr uint32_t MaxGlobalLayers = 256;

    static std::string ResolveLayerKey(
        const TerrainLayerDesc& layer,
        const std::string& assetPath);
    // 同一 key 的两个层是否参数完全一致（name/height/slope/noise/uv/
    // sourceResolution/avgColor/sourceSet/flags）。
    static bool LayersEquivalent(
        const TerrainLayerDesc& lhs,
        const TerrainLayerDesc& rhs);

    // 注册一个地形的全部层，返回 local -> global 映射。
    // 冲突、层名重复、超过 256 层都会抛 std::runtime_error；
    // 重复注册同一 placementId 也会失败（映射必须唯一）。
    const std::vector<uint8_t>& RegisterTerrain(const TerrainBuildPlan& plan);

    const std::vector<TerrainGlobalLayerRecord>& Layers() const
    {
        return layers;
    }
    // 供 catalog/页管理器使用的全局层描述（顺序即 globalId）。
    std::vector<TerrainLayerDesc> LayerDescriptions() const;
    const std::vector<uint8_t>* FindLayerIdMap(
        const std::string& placementId) const;
    uint32_t GlobalLayerCount() const
    {
        return static_cast<uint32_t>(layers.size());
    }
    // 局部层总数 - 全局层数；0 表示没有任何跨 TR 去重。
    uint32_t DedupedLayerCount() const;
    bool Empty() const { return layers.empty(); }
    void Clear();

private:
    std::vector<TerrainGlobalLayerRecord> layers;
    std::map<std::string, uint32_t> layerIdByKey;
    std::map<std::string, std::vector<uint8_t>> layerIdMapsByPlacement;
};

} // namespace VL
