#pragma once

#include <cstdint>

namespace VL
{

// DecalRenderer 的实例 SSBO 与逐槽 material descriptor 都是固定 64 槽。
// CPU particle decal runtime 必须共享同一上限，不能按粒子数扩 SSBO。
inline constexpr uint32_t kMaxDecalInstances = 64;

} // namespace VL
