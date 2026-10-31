#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "hair/tressfxGroupBinding.h"
#include "hair/tressfxGroupPalette.h"

namespace VL::Hair
{

// Object Set 2 binding 6 的 std430 vec4 元素；a 恒为 1。
struct alignas(16) TressfxGroupColorGpu
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct TressfxGroupGpuData
{
    // 每 guide 一个 group id；0 = 默认组，k = 声明序第 k 组。
    std::vector<uint32_t> guideGroupIds;
    // index 0 = 默认灰，k = 第 k 个命名组的调色板颜色。
    std::vector<TressfxGroupColorGpu> groupColors;
};

// 导入后的 section 数据 + 已校验的 groups[] 名称 -> GPU 顶点阶段需要的两份数据。
// 纯函数，不接触 Vulkan，便于用模块测试固定“默认组 0 / 命名组 k”的映射。
inline TressfxGroupGpuData BuildTressfxGroupGpuData(
    uint32_t guideCount,
    const std::vector<uint32_t>& importedGuideGroupIds,
    const std::vector<std::string>& groupNames,
    uint32_t colorCapacity = 0)
{
    TressfxGroupGpuData data;
    data.guideGroupIds.assign(guideCount, 0u);
    if (importedGuideGroupIds.size() == guideCount)
        data.guideGroupIds = importedGuideGroupIds;

    // descriptor 捕获的是固定容量 buffer；尾部用默认灰色填满，避免组数减少后
    // 旧组号短暂读到未初始化颜色。着色器仍只按真实 guide id 索引。
    const size_t activeColorCount = groupNames.size() + 1;
    const size_t tableSize = colorCapacity == 0
        ? activeColorCount : std::max<size_t>(activeColorCount, colorCapacity);
    data.groupColors.resize(tableSize);
    const TressfxGroupColor defaultColor = TressfxDefaultGroupColor();
    const TressfxGroupColorGpu defaultGpu = {static_cast<float>(defaultColor.r) / 255.0f,
        static_cast<float>(defaultColor.g) / 255.0f,
        static_cast<float>(defaultColor.b) / 255.0f, 1.0f};
    for (TressfxGroupColorGpu& color : data.groupColors)
        color = defaultGpu;
    for (size_t index = 0; index < groupNames.size(); ++index)
    {
        const TressfxGroupColor color = TressfxGroupColorForName(groupNames[index]);
        data.groupColors[index + 1] = {static_cast<float>(color.r) / 255.0f,
            static_cast<float>(color.g) / 255.0f,
            static_cast<float>(color.b) / 255.0f, 1.0f};
    }
    return data;
}

} // namespace VL::Hair
