#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "resource/image/hostImage.h"

namespace VL::TerrainHeightmapIO
{

// 地形高度图专用加载入口，不改变 TextureIO 的通用贴图语义。
// 16-bit grayscale PNG 会解码为 R32_FLOAT 并把 uint16 样本除以 65535，
// 保留全部 16 bit；8-bit PNG/BMP/TGA 与 EXR/HDR 仍完全走 TextureIO
// 原有的 linear、ForceOff、forceChannels=4 路径，输出格式不变。
std::optional<HostImage> Load(
    const std::filesystem::path& path,
    std::string& error);

} // namespace VL::TerrainHeightmapIO
