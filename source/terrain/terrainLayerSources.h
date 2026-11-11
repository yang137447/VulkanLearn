#pragma once

#include <cstdint>
#include <map>
#include <memory>

class Texture;

namespace VL
{

// 一组层源数组。TR 的 layer.sourceSet 指向其中一个记录；不同 sourceSet
// 允许绑定不同纹理，但同一 World 的层 atlas 仍只有一组，因此三张图的
// 格式必须跨 set 一致（多格式组留给 S5）。
//
// layerSlices 是 global layerId -> 该组源数组的 slice 下标。全局 ID 只在
// 这个 World 内稳定，源数组本身仍按 MI 的局部顺序排列，所以上传时必须
// 用这张映射而不是直接把 globalId 当 slice。
struct TerrainSourceSetRecord
{
    uint32_t sourceSet = 0;
    std::shared_ptr<Texture> albedo;
    std::shared_ptr<Texture> normal;
    std::shared_ptr<Texture> pbr;
    uint32_t arrayLayers = 0;
    uint32_t albedoMipLevels = 0;
    uint32_t normalMipLevels = 0;
    uint32_t pbrMipLevels = 0;
    uint32_t albedoWidth = 0;
    uint32_t albedoHeight = 0;
    uint32_t normalWidth = 0;
    uint32_t normalHeight = 0;
    uint32_t pbrWidth = 0;
    uint32_t pbrHeight = 0;
    // 三张源图的实际 vk::Format，存整数避免 terrain 层依赖 Vulkan 头。
    // 单一 atlas 格式组要求所有 sourceSet 的对应格式相同（S5 才分组）。
    int32_t albedoFormat = 0;
    int32_t normalFormat = 0;
    int32_t pbrFormat = 0;
    // catalog 的 sourceResolution 必须等于真实源图边长；不一致时 mode 0
    // 只警告，mode 1（VT）必须加载期失败。
    bool catalogResolutionMatches = true;
    std::map<uint32_t, uint32_t> layerSlices;

    // 三张源图都能 copy 的 mip 级数（取最小值）。
    uint32_t MinMipLevels() const;
    bool HasTextures() const;
    bool TryGetLayerSlice(uint32_t globalLayerId, uint32_t& slice) const;
};

using TerrainLayerSourceSets = std::map<uint32_t, TerrainSourceSetRecord>;

const TerrainSourceSetRecord* FindTerrainSourceSet(
    const TerrainLayerSourceSets& sources,
    uint32_t sourceSet);

// 所有 set 的公共 mip 上限（0 表示没有可用源）。
uint32_t TerrainSourceMinMipLevels(const TerrainLayerSourceSets& sources);

// 指定层所在 set 的 mip 上限；set 缺失时返回 0（调用方应视为不可上传）。
uint32_t TerrainSourceMipLevelsForLayer(
    const TerrainLayerSourceSets& sources,
    uint32_t sourceSet);

} // namespace VL
