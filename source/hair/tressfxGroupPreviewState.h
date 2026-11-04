#pragma once

#include <cstdint>

namespace VL
{

// preview-only 状态：不写资产，也不改变 material/几何身份。
// flags 与 base.frag.glsl 的 `uboM.selectionData.w` 位定义保持一致；
// selected group 单独走 selectionData.z，不打包进 flags。
enum TressfxGroupPreviewFlags : uint32_t
{
    TressfxGroupPreviewColors = 1u << 0,
    TressfxGroupPreviewSolo = 1u << 1,
    // brush ghost 是 per-object 运行时状态；target group 单独走 selectionData.z，
    // 这里只记录 active/remove。
    TressfxGroupPreviewBrushActive = 1u << 2,
    TressfxGroupPreviewBrushRemove = 1u << 3
};

struct TressfxGroupPreviewState
{
    // -1 = 无选中；0 = 隐式 Default；k = 声明序第 k 个命名组。
    int32_t selectedGroup = -1;
    bool groupColors = false;
    bool solo = false;
};

} // namespace VL
