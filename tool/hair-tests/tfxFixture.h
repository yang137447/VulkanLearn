#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace TfxFixture
{
inline void U32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (std::size_t i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8u));
}

inline void Float(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    U32(bytes, offset, bits);
}

// 按 exporter 的字节布局独立构造，不依赖生产 parser 的 struct 大小或序列化代码。
inline std::vector<std::uint8_t> Guides(std::uint32_t guides = 1, std::uint32_t points = 4)
{
    std::vector<std::uint8_t> bytes(160u + guides * points * 16u, 0);
    Float(bytes, 0, 4.0f);
    U32(bytes, 4, guides);
    U32(bytes, 8, points);
    U32(bytes, 12, 160);
    for (std::uint32_t strand = 0; strand < guides; ++strand)
    {
        for (std::uint32_t point = 0; point < points; ++point)
        {
            const auto offset = 160u + (strand * points + point) * 16u;
            Float(bytes, offset, static_cast<float>(strand));
            Float(bytes, offset + 4, static_cast<float>(point));
            Float(bytes, offset + 8, -2.0f);
            Float(bytes, offset + 12, point < 2u ? 0.0f : 1.0f);
        }
    }
    return bytes;
}

inline void Point(
    std::vector<std::uint8_t>& bytes, std::size_t index, const std::array<float, 4>& value)
{
    for (std::size_t i = 0; i < 4; ++i) Float(bytes, 160u + index * 16u + i * 4u, value[i]);
}

inline void Attribute(
    std::vector<std::uint8_t>& bytes, std::size_t headerOffset, const std::vector<float>& values)
{
    const auto offset = (bytes.size() + 7u) & ~std::size_t{7u};
    bytes.resize(offset + values.size() * 4u);
    U32(bytes, headerOffset, static_cast<std::uint32_t>(offset));
    for (std::size_t i = 0; i < values.size(); ++i) Float(bytes, offset + i * 4u, values[i]);
}
}
