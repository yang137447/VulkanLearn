#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace VL
{

// 纯 C++ SHA-256（FIPS 180-4），返回 64 个小写十六进制字符。
// 只用于 VLP2 sidecar 的 contentSha256，避免为一次哈希引入第三方依赖。
std::string TerrainPaintSha256Hex(const std::vector<uint8_t>& bytes);

} // namespace VL
