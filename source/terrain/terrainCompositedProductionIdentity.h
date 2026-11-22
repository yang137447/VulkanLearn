#pragma once

// File responsibility: 定义 terrain composited 生成器生产身份的纯 CPU 比较。
// 只消费 publication 的 Production 闭包与当前 shader build manifest 快照，
// 不读取 package payload，也不做任何编译器/Vulkan 工作。

#include "shader/build/shaderBuildManifest.h"
#include "terrain/terrainCompositedProduction.h"

namespace VL
{

// 生产身份 = publication 记录的 compute 源码/include 摘要 + 当前编译器 manifest
// 中 runtime 角色输出的 SPIR-V 摘要。
//
// publication 会同时归档 runtime 与 debug reflection 两个输出（每个 compute kernel
// 各一份），但 debug 副本包含 -g 调试信息，源码空白/行号变化就会重编译出不同字节，
// 而优化后的 runtime SPIR-V 可以完全不变。debug 摘要因此不是运行时 compute 闭包
// 的一部分，不能参与身份比较；否则一次纯反射产物重编译就会误拒绝整份 publication。
bool MatchesTerrainCompositedGeneratorClosure(
    const TerrainCompositedProductionClosure& expected,
    const ShaderBuildManifestSnapshot& actual);

}
