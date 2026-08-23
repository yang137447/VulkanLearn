#pragma once

namespace VL::Hair
{

// 角度常量与 wrap 规则是 LUT 坐标和 CPU oracle 的共同基础合同；独立出来避免
// 轻量资源工具为了一个角度 helper 链接完整的 reference evaluator。
inline constexpr float HairPi = 3.14159265358979323846f;
inline constexpr float HairHalfPi = HairPi * 0.5f;

float WrapAngle(float angle) noexcept;

} // namespace VL::Hair
