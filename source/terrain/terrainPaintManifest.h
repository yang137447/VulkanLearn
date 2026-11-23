#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "terrain/terrainAssetTypes.h"

namespace VL
{

// VLP1 `.vlpaint` 旁边的 VLP2 sidecar（`<paint>.meta.json`）。
// sidecar 只记录 tile 身份、层 key 顺序和形状，不改变 VLP1 二进制格式；
// 运行时加载 paint 后用它校验“这份权重是不是仍属于当前 TR 的层表”。
struct TerrainPaintManifest
{
    uint32_t version = 2;
    uint32_t tileX = 0;
    uint32_t tileY = 0;
    uint32_t tileGrid = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layerCount = 0;
    std::vector<std::string> layerKeys;
    float featherTexels = 0.0f;
    // 编辑器 bundle 与离线工具都会写入文件字节的 SHA-256；运行时只解析
    // 该字段，不计算也不核对哈希。
    std::string contentSha256;
};

// paintPath + ".meta.json"（追加，不替换扩展名）。
std::filesystem::path TerrainPaintManifestPath(
    const std::filesystem::path& paintPath);

// 按 TR 的层顺序给出稳定 key：显式 key 优先，否则 "<assetPath>#<name>"。
std::vector<std::string> TerrainPaintLayerKeys(const TerrainAssetDesc& desc);

// 查询失败（权限、非法路径等）返回 false 并写入 error；只有“确实不存在”
// 才返回 false 且 error 为空。
bool TerrainPaintManifestExists(
    const std::filesystem::path& paintPath,
    std::string& error);

// 读取并解析 sidecar。要求 version==2、kind=="terrainLayerWeights"；
// 字段缺失或类型错误都会失败，失败时 out 不被修改。
bool LoadTerrainPaintManifest(
    const std::filesystem::path& paintPath,
    TerrainPaintManifest& out,
    std::string& error);

// 校验形状与层身份：layerCount、width/height、layerKeys 顺序必须完全一致。
// contentSha256 不在这里校验。
bool ValidateTerrainPaintManifest(
    const TerrainPaintManifest& manifest,
    const std::vector<std::string>& expectedLayerKeys,
    uint32_t width,
    uint32_t height,
    std::string& error);

// 原子写 sidecar（indent=2、ensure_ascii=false、末尾换行）。
bool SaveTerrainPaintManifest(
    const std::filesystem::path& paintPath,
    const TerrainPaintManifest& manifest,
    std::string& error);

// 序列化 sidecar 文本（indent=2、ensure_ascii=false、末尾换行）。
// bundle 写入复用它；SaveTerrainPaintManifest 等价于它 + 原子写。
bool BuildTerrainPaintManifestBytes(
    const TerrainPaintManifest& manifest,
    std::string& out,
    std::string& error);

} // namespace VL
