#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "terrain/terrainAssetTypes.h"

namespace VL
{

// 把 TR 中的资源根相对 paintPath 解析为物理路径，并确认结果仍位于资源根内。
// 文件可以尚不存在（首次 Save Paint 的目标）；存在性由调用方决定。
bool ResolveTerrainPaintPath(
    const std::filesystem::path& resourceRoot,
    const std::string& relative,
    std::filesystem::path& out,
    std::string& error);

// 编辑器 layer paint 的持久化文件（.vlpaint v1）。格式固定为小端：
//   char magic[4] = 'V','L','P','1'
//   uint32 width, height, layerCount, flags(=0), reserved(=0)
//   float32 weights[width*height*layerCount]  // layer-major
//   uint8   opacity[width*height*layerCount]  // layer-major
// 权重与透明度都由 TR 的 layers 顺序决定；读取时必须给出同一 layerCount，
// 不允许按文件名或内容猜层身份。写回使用同目录临时文件 + 原子替换。
bool SaveTerrainLayerPaintFile(
    const std::filesystem::path& path,
    const TerrainLayerPaintData& paint,
    std::string& error);

// 读取完整快照；成功后 out 才会被替换。错误信息包含具体文件路径。
bool LoadTerrainLayerPaintFile(
    const std::filesystem::path& path,
    uint32_t expectedLayerCount,
    TerrainLayerPaintData& out,
    std::string& error);

} // namespace VL
