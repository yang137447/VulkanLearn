#ifndef VL_TRESSFX_SDF_COMMON_GLSL
#define VL_TRESSFX_SDF_COMMON_GLSL

// TressFX 5.0 逐帧碰撞距离场的共享声明。
//
// - std140 SdfGridParams 与 source/hair/tressfxSdfGrid.h 的 TressfxSdfParams
//   一一对应（64 bytes）；字段顺序/偏移变化必须同时改 C++ 的 static_assert。
// - 距离是**有符号**的，符号约定与 TressFXSimulationSDFGenConstruct.usf 的
//   SignedDistancePointToTriangle 相同：最近点距离 + dot(p-a, cross(b-a,c-a)) 的
//   符号。代理保留身体网格绕序，因此外侧为正、内侧为负。
// - TressFX 用 FloatFlip（`asuint(f) ^ 0x80000000`）写 uint 网格；但 min 会
//   选出 band 内最负的三角形而不是最近三角形。VL 用 magnitude-first key
//   （见 SdfFloatFlip）：先按 |d| 选最近表面，再带出符号。
//   0xFFFFFFFF 是 INITIAL_DISTANCE 等价哨兵，比任何有限距离的 key 都大。
// - 域是每帧从蒙皮后代理包围盒重算的立方体；分辨率为创建时固定的 48..64，
//   origin/cellSize/truncation 每帧由 Host 写进 SDF params。
// - 格子 i 的中心是 origin + cellSize*(i+0.5)，与 TressFX 的 GetSdfCellPosition
//   一致；插值取相邻两个格子中心，避免在格子后半段外插。

layout(std140, set = 0, binding = 8) uniform SdfGridParams
{
    vec4 originCell; // xyz = 网格最小角，w = cellSize（厘米，每帧）
    uvec4 dims;      // xyz = 每轴格子数，w = 总格子数
    uvec4 counts;    // x = 代理三角形数，yzw 保留
    vec4 range;      // x = collision margin，y = truncation（厘米）
} sdf;

layout(set = 0, binding = 11, std430) buffer SdfGrid
{
    uint cells[];
} sdfGrid;

const uint SdfInitialBits = 0xFFFFFFFFu;
const float SdfOutOfRangeDistance = 1.0e10;

uint SdfCellIndex(ivec3 coordinate)
{
    return uint(coordinate.x) + sdf.dims.x *
        (uint(coordinate.y) + sdf.dims.y * uint(coordinate.z));
}

// 【与 TressFX 的显式差异】TressFX 的 FloatFlip 只翻符号位；多个三角形覆盖
// 同一格时 min 会选出 band 内最负的三角形，表面附近产生负偏置。VL 用
// magnitude-first key：先按 |d| 选最近表面，再用最低位保留符号。
// 代价：尾数少 1 bit、编码与 TressFX 原始 XOR 不逐位相同。
uint SdfFloatFlip(float value)
{
    uint bits = floatBitsToUint(abs(value));
    return (bits << 1) | (value < 0.0 ? 1u : 0u);
}

float SdfIFloatFlip(uint bits)
{
    float magnitude = uintBitsToFloat((bits >> 1) & 0x7FFFFFFFu);
    return (bits & 1u) != 0u ? -magnitude : magnitude;
}

float SdfCoordinate(float grid, float last, out int base)
{
    if (grid <= 0.0)
    {
        base = 0;
        return 0.0;
    }
    if (grid >= last)
    {
        base = int(last);
        return 0.0;
    }
    base = int(grid);
    return grid - float(base);
}

// 有符号三线性采样。域外或任一角点为 clear 哨兵时返回 +1e10（碰撞 pass 的
// early-out；TressFX GetSignedDistance 同样返回 1e10）。
float SdfTrilinear(vec3 position)
{
    vec3 coordinate = (position - sdf.originCell.xyz) / sdf.originCell.w;
    if (any(lessThan(coordinate, vec3(0.0))) ||
        any(greaterThan(coordinate, vec3(sdf.dims.xyz))))
        return SdfOutOfRangeDistance;

    ivec3 base;
    vec3 fraction;
    fraction.x = SdfCoordinate(coordinate.x - 0.5, float(sdf.dims.x - 1u), base.x);
    fraction.y = SdfCoordinate(coordinate.y - 0.5, float(sdf.dims.y - 1u), base.y);
    fraction.z = SdfCoordinate(coordinate.z - 0.5, float(sdf.dims.z - 1u), base.z);
    ivec3 high = min(base + ivec3(1), ivec3(sdf.dims.xyz) - ivec3(1));

    const uint b000 = sdfGrid.cells[SdfCellIndex(ivec3(base.x, base.y, base.z))];
    const uint b100 = sdfGrid.cells[SdfCellIndex(ivec3(high.x, base.y, base.z))];
    const uint b010 = sdfGrid.cells[SdfCellIndex(ivec3(base.x, high.y, base.z))];
    const uint b110 = sdfGrid.cells[SdfCellIndex(ivec3(high.x, high.y, base.z))];
    const uint b001 = sdfGrid.cells[SdfCellIndex(ivec3(base.x, base.y, high.z))];
    const uint b101 = sdfGrid.cells[SdfCellIndex(ivec3(high.x, base.y, high.z))];
    const uint b011 = sdfGrid.cells[SdfCellIndex(ivec3(base.x, high.y, high.z))];
    const uint b111 = sdfGrid.cells[SdfCellIndex(ivec3(high.x, high.y, high.z))];
    if (b000 == SdfInitialBits || b100 == SdfInitialBits || b010 == SdfInitialBits ||
        b110 == SdfInitialBits || b001 == SdfInitialBits || b101 == SdfInitialBits ||
        b011 == SdfInitialBits || b111 == SdfInitialBits)
        return SdfOutOfRangeDistance;

    const float d000 = SdfIFloatFlip(b000);
    const float d100 = SdfIFloatFlip(b100);
    const float d010 = SdfIFloatFlip(b010);
    const float d110 = SdfIFloatFlip(b110);
    const float d001 = SdfIFloatFlip(b001);
    const float d101 = SdfIFloatFlip(b101);
    const float d011 = SdfIFloatFlip(b011);
    const float d111 = SdfIFloatFlip(b111);
    const float lower = mix(mix(d000, d100, fraction.x), mix(d010, d110, fraction.x),
        fraction.y);
    const float upper = mix(mix(d001, d101, fraction.x), mix(d011, d111, fraction.x),
        fraction.y);
    return mix(lower, upper, fraction.z);
}

// 同一有符号三线性场的中心差分梯度，半步长 = 0.5*cellSize；除以 2h = cellSize。
// 参考 shader 用 0.1*cellSize 的前向差分，这里改用中心差分以减少单侧偏置。
vec3 SdfGradient(vec3 position)
{
    const float halfStep = 0.5 * sdf.originCell.w;
    const float dx = SdfTrilinear(position + vec3(halfStep, 0.0, 0.0)) -
        SdfTrilinear(position - vec3(halfStep, 0.0, 0.0));
    const float dy = SdfTrilinear(position + vec3(0.0, halfStep, 0.0)) -
        SdfTrilinear(position - vec3(0.0, halfStep, 0.0));
    const float dz = SdfTrilinear(position + vec3(0.0, 0.0, halfStep)) -
        SdfTrilinear(position - vec3(0.0, 0.0, halfStep));
    return vec3(dx, dy, dz) / sdf.originCell.w;
}

// Ericson《Real-Time Collision Detection》5.1.5，与 sdfBuilder 的
// ClosestPointOnTriangleSquared 逐分支对应（这里直接返回无符号距离）。退化
// 三角形的面内分支回退到最近顶点，保证结果有限而不是 0/0。
float SdfDistanceToTriangle(vec3 point, vec3 a, vec3 b, vec3 c)
{
    vec3 ab = b - a;
    vec3 ac = c - a;
    vec3 ap = point - a;
    float d1 = dot(ab, ap);
    float d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0)
        return length(ap);

    vec3 bp = point - b;
    float d3 = dot(ab, bp);
    float d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3)
        return length(bp);

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
        return length(point - (a + ab * (d1 / (d1 - d3))));

    vec3 cp = point - c;
    float d5 = dot(ab, cp);
    float d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6)
        return length(cp);

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
        return length(point - (a + ac * (d2 / (d2 - d6))));

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
        return length(point - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));

    float denominator = va + vb + vc;
    if (!(denominator > 0.0))
        return min(min(length(point - a), length(point - b)), length(point - c));
    float v = vb / denominator;
    float w = vc / denominator;
    return length(point - (a + ab * v + ac * w));
}

// TressFX SignedDistancePointToTriangle 的符号：绕序法线一侧为正。
float SdfSignedDistanceToTriangle(vec3 point, vec3 a, vec3 b, vec3 c)
{
    float distance = SdfDistanceToTriangle(point, a, b, c);
    vec3 normal = cross(b - a, c - a);
    return dot(point - a, normal) < 0.0 ? -distance : distance;
}

#endif // VL_TRESSFX_SDF_COMMON_GLSL
