#pragma once

#include "core/runtimeResult.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace VL::Hair
{
inline constexpr std::size_t MaxTfxFileBytes = 512u * 1024u * 1024u;
inline constexpr std::size_t MaxTfxVertexCount = 4u * 1024u * 1024u;

// Maya exporter 的 guide 数据，归导入结果所有，供 CPU 预处理与后续 GPU 上传消费。
// xyz/UV 保留文件坐标；position.w 是逆质量，不是齐次坐标。没有 UE 对象或 GPU 资源。
struct TfxAsset
{
    float version = 0;
    std::uint32_t guideCount = 0;
    std::uint32_t pointsPerStrand = 0;
    std::vector<std::array<float, 4>> positions;
    // offset==0 对应空数组，不伪造 UV、颜色或宽度；调用者自行决定缺省材质输入。
    std::vector<std::array<float, 2>> strandUVs;
    std::vector<std::array<float, 2>> vertexUVs;
    std::vector<float> strandThickness;
    std::vector<std::array<float, 4>> vertexColors;
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
};

// 按已知的 160-byte little-endian header/绝对 offset 解析 version>=4 的布局。
// 所有外部数据在此校验；不补齐 compute 线程组、不生成 follow、不转换单位/坐标轴。
RuntimeResult<TfxAsset> ParseTfx(
    const std::vector<std::uint8_t>& bytes, const std::string& sourcePath = {});
RuntimeResult<TfxAsset> LoadTfx(const std::filesystem::path& path);
}
