#pragma once

#include <cstdint>

namespace VL
{

// 编辑器、资产加载器与 GPU 打包表共用的粒子上限。放在轻量头文件里，
// 让只操作作者 JSON 的编辑器纯函数层不必拉入 Eigen 或运行时资产类型。
inline constexpr uint32_t kMaxParticleCurveKeys = 8;
inline constexpr uint32_t kMaxParticleEmitters = 16;
// 多级 LOD 档位上限；与粒子 UBO 无关，只在 CPU 侧选择发射预算。
inline constexpr uint32_t kMaxParticleLodTiers = 4;

} // namespace VL
