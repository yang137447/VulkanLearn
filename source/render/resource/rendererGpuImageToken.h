#pragma once

#include <cstdint>

namespace VL
{

// UI/资产快照只保留不可变身份和尺寸；renderer 根据 token 解析并保活成图，
// token 不包含 Vulkan 对象，也不在析构时操作 GPU。
struct RendererGpuImageToken
{
    RendererGpuImageToken(uint64_t id, uint32_t width, uint32_t height)
        : id(id), width(width), height(height) {}

    const uint64_t id;
    const uint32_t width;
    const uint32_t height;
};

} // namespace VL
