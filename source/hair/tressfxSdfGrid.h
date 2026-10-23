#pragma once

#include "core/runtimeResult.h"
#include "hair/sdfBuilder.h"
#include "hair/tressfxProxySkinning.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <Eigen/Dense>

namespace VL::Hair
{
// 运行时逐帧碰撞距离场。纯 CPU、无 Vulkan 依赖：这里的 domain / clear / splat /
// 三线性采样 / 中心差分梯度 / 位置投影都是 GPU pass 的逐操作镜像，供模块测试在
// 没有 Vulkan 的情况下验证同一套数学。
//
// 与离线 sdfBuilder 的区别（有意）：
// - 场是**有符号**距离，符号约定与 TressFXSimulationSDFGenConstruct.usf 的
//   SignedDistancePointToTriangle 相同：取点到三角形最近点距离，再用
//   dot(p - a, cross(b-a, c-a)) 的符号决定正负（三角形绕序法线一侧为正）。
//   代理从身体网格保留绕序，正常情况下外侧为正、内侧为负。
// - 用稀疏三角形 splat 代替离线 BVH 逐体素最近点搜索：每个三角形把覆盖到的
//   格子做 atomicMin（CPU 镜像用同序的整数比较）。结果与“逐格子暴力最近点”
//   在 truncation 带内相同（取所有三角形距离的最小值）。
// - TressFX 用 FloatFlip3（`asuint(f) ^ 0x80000000`）把有符号距离映射成 uint；
//   但 min 会选出 band 内最负的三角形而不是最近三角形。VL 用 magnitude-first
//   的 key（见 TressfxSdfFloatFlip）：先按 |d| 选最近表面，再带出它的符号。
// - clear 值 0xFFFFFFFF 是 TressFX INITIAL_DISTANCE 的等价哨兵：它比任何有限
//   距离的 key 都大，不会被 atomicMin 覆盖；采样遇到它时返回 1e10（域外）。
// - 格子分辨率固定（48..64 之一，创建时由作者 cellSize 目标映射一次），域每帧
//   从**蒙皮后**的代面包围盒重建，因此固定 buffer 大小、不逐帧重分配。

// 创建时解析出的分辨率必须落在这个范围；正式代理在 1 cm 目标下解析为 48³
// （110,592 格 ≈ 432 KiB）。
inline constexpr std::uint32_t TressfxSdfMinResolution = 48;
inline constexpr std::uint32_t TressfxSdfMaxResolution = 64;
inline constexpr float TressfxSdfMinDomainExtentCm = 1.0f;
// 梯度平方长度低于该值视为退化（例如点恰好落在表面、中心差分对称抵消），
// shader 与 CPU 镜像都跳过而不是归一化出 NaN。
inline constexpr float TressfxSdfGradientEpsilonSquared = 1.0e-12f;
// 采样到 clear 哨兵/域外时返回的正距离；碰撞 pass 的 `distance > margin`
// 会直接早退。TressFX 的 GetSignedDistance 同样返回 1e10。
inline constexpr float TressfxSdfOutOfRangeDistance = 1.0e10f;
// 有符号编码后的哨兵（TressFX 的 INITIAL_DISTANCE 等价物）。
inline constexpr std::uint32_t TressfxSdfInitialBits = 0xFFFFFFFFu;

// 轴对齐立方体格子域。origin 是最小角，格子 i 的中心是
// origin + cellSize*(i+0.5)，与 TressFX 的 GetSdfCellPosition 一致。
struct TressfxSdfLayout
{
    Eigen::Vector3f boundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax = Eigen::Vector3f::Zero();
    Eigen::Vector3f origin = Eigen::Vector3f::Zero();
    float cellSize = 1.0f;
    float margin = 1.0f;
    float truncation = 0.0f;
    std::uint32_t resolution = TressfxSdfMinResolution;
    std::array<std::uint32_t, 3> dims{TressfxSdfMinResolution, TressfxSdfMinResolution,
        TressfxSdfMinResolution};

    std::uint32_t CellCount() const
    {
        return dims[0] * dims[1] * dims[2];
    }
    Eigen::Vector3f CellCenter(std::uint32_t x, std::uint32_t y, std::uint32_t z) const
    {
        return origin + cellSize * Eigen::Vector3f(
            static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
            static_cast<float>(z) + 0.5f);
    }
};

// 与 shader/glsl/hair/tressfxSdfCommon.glsl 的 std140 SdfGridParams 一一对应。
// originCell.w = cellSize；dims.w = cellCount；counts.x = 代理三角形数，
// counts.y = 1 表示代理走逐顶点多骨 LBS（0 回退到单骨根矩阵）；
// range.x = margin；range.y = truncation。任何字段顺序/大小变化都必须同时改
// shader 声明与下面的 static_assert。
struct TressfxSdfParams
{
    Eigen::Vector4f originCell = Eigen::Vector4f::Zero();
    std::array<std::uint32_t, 4> dims{0, 0, 0, 0};
    std::array<std::uint32_t, 4> counts{0, 0, 0, 0};
    Eigen::Vector4f range = Eigen::Vector4f::Zero();
};
static_assert(sizeof(TressfxSdfParams) == 64,
    "std140 SDF grid params must stay 64 bytes");
static_assert(alignof(TressfxSdfParams) == 16,
    "std140 SDF grid params require vec4 alignment");
static_assert(offsetof(TressfxSdfParams, dims) == 16,
    "std140 SDF dims must start at offset 16");
static_assert(offsetof(TressfxSdfParams, counts) == 32,
    "std140 SDF counts must start at offset 32");
static_assert(offsetof(TressfxSdfParams, range) == 48,
    "std140 SDF range must start at offset 48");

// 作者 cellSize 是**绑定姿态下的目标格距**：分辨率 = clamp(round(maxExtent /
// cellSize), 48, 64)，之后固定不变。实际格距 = 每帧域边长 / 分辨率，因此会随
// 代理包围盒变化；作者值仍进入模型缓存身份。
std::uint32_t TressfxSdfResolutionForCellSize(float cellSize, float bindMaxExtent);

// 每帧域：以蒙皮后代理包围盒的中心为立方体中心，边长同时覆盖包围盒和
// truncation = margin + 2*cellSize。固定分辨率，返回的 dims 恒为
// {resolution,resolution,resolution}；退化包围盒（零/负 extent）按
// TressfxSdfMinDomainExtentCm 处理，避免除零。非有限包围盒返回失败。
RuntimeResult<TressfxSdfLayout> MakeTressfxSdfDomain(const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax, float margin, std::uint32_t resolution);

// 绑定姿态入口：校验代理三角形汤、求包围盒、按作者 cellSize 解析分辨率，
// 再调用 MakeTressfxSdfDomain。用于创建 buffer 与测试；每帧域由
// MakeTressfxSdfDomain 从蒙皮后的包围盒重算。
RuntimeResult<TressfxSdfLayout> MakeTressfxSdfLayout(const SdfTriangleMesh& proxy,
    float cellSize, float margin);

TressfxSdfParams MakeTressfxSdfParams(
    const TressfxSdfLayout& layout, std::uint32_t triangleCount,
    bool proxySkinning = false);

// 【与 TressFX 的显式差异】TressFX 的 FloatFlip（`asuint(f) ^ 0x80000000`）只
// 翻符号位，同号段内按 |d| 排序；但一个格子被多个三角形覆盖时，min 会把
// “band 内最负”的三角形（而不是最近三角形）选出来，表面附近产生可达一个
// truncation 的负偏置（实测球面质心 -1.2 cm）。这里改用 magnitude-first 的
// 打包 key：`(floatBitsToUint(|d|) << 1) | (d < 0 ? 1 : 0)`。无符号 min 先按
// |d| 选最近三角形，再用最低位保留其符号；采样时右移并还原符号。
// 代价：float 尾数少 1 bit（碰撞尺度下误差远小于格距），且不再与 TressFX 的
// 原始 XOR 编码逐位相同；行为上更接近“到网格的有符号距离”定义。
inline std::uint32_t TressfxSdfFloatFlip(float value)
{
    std::uint32_t bits = 0;
    const float magnitude = std::abs(value);
    std::memcpy(&bits, &magnitude, sizeof(bits));
    return (bits << 1) | (value < 0.0f ? 1u : 0u);
}
inline float TressfxSdfIFloatFlip(std::uint32_t bits)
{
    const std::uint32_t magnitudeBits = (bits >> 1) & 0x7FFFFFFFu;
    float magnitude = 0.0f;
    std::memcpy(&magnitude, &magnitudeBits, sizeof(magnitude));
    return (bits & 1u) != 0u ? -magnitude : magnitude;
}

// 有符号点到三角形距离。最近点用与 sdfBuilder 相同的 Ericson 区域分支；
// 符号用三角形绕序法线（TressFX SignedDistancePointToTriangle 的同一约定）。
// 退化三角形在面内分支回退到最近顶点距离，符号仍由（可能为零的）面法线决定。
float SignedDistancePointToTriangle(const Eigen::Vector3f& point, const Eigen::Vector3f& a,
    const Eigen::Vector3f& b, const Eigen::Vector3f& c);

// clear 等价：所有格子写成 TressfxSdfInitialBits。
void ClearTressfxSdfGrid(const TressfxSdfLayout& layout, std::vector<std::uint32_t>& cells);

// splat 等价：每个三角形先把三个顶点按 skinning 变换（根点用同一矩阵），再对
// “三角形 AABB 每侧外扩 truncation”覆盖的格子写有符号距离编码的最小值
// （CPU 用同序的 uint 比较，GPU 用 atomicMin）。
// 传入 proxySkin + jointMatrices 时改用逐顶点多骨 LBS（与 GPU splat 的
// counts.y=1 路径同一数学）；否则保持单骨刚性矩阵。
void SplatTressfxSdfGrid(const TressfxSdfLayout& layout, const SdfTriangleMesh& proxy,
    const Eigen::Matrix4f& skinning, std::vector<std::uint32_t>& cells,
    const SdfProxySkinTransfer* proxySkin = nullptr,
    const std::vector<Eigen::Matrix4f>* jointMatrices = nullptr);

// clear + splat。返回的 cells 长度等于 layout.CellCount()。
std::vector<std::uint32_t> BuildTressfxSdfGrid(const TressfxSdfLayout& layout,
    const SdfTriangleMesh& proxy,
    const Eigen::Matrix4f& skinning = Eigen::Matrix4f::Identity(),
    const SdfProxySkinTransfer* proxySkin = nullptr,
    const std::vector<Eigen::Matrix4f>* jointMatrices = nullptr);

// 三线性采样（有符号）。域外或 8 角点含 clear 哨兵时返回
// TressfxSdfOutOfRangeDistance。采样区间取相邻两个格子中心，插值不外插。
float SampleTressfxSdfValue(const TressfxSdfLayout& layout,
    const std::vector<std::uint32_t>& cells, const Eigen::Vector3f& position);

// 中心差分梯度（半步长 = 0.5*cellSize，除以 2h = cellSize）。域外采样按
// TressfxSdfOutOfRangeDistance 参与差分，与 shader 完全一致；退化（平方长度
// <= epsilon）返回零向量。
Eigen::Vector3f TressfxSdfGradient(const TressfxSdfLayout& layout,
    const std::vector<std::uint32_t>& cells, const Eigen::Vector3f& position);

// 碰撞投影的 CPU 镜像：`distance > margin` 直接返回 false（有符号距离下内部点
// 是负值，总会被处理）；梯度退化也返回 false；否则
// position += normalize(gradient) * (margin - distance)。只改位置，不写速度/历史。
bool ResolveTressfxSdfCollision(const TressfxSdfLayout& layout,
    const std::vector<std::uint32_t>& cells, Eigen::Vector3f& position);
}
