#ifndef VL_ROD_KKT10_BAND_SOLVE
#define VL_ROD_KKT10_BAND_SOLVE

#include "rodV3Kkt10.glsl"

// ===========================================================================
// CosseratImplicitV3 的 10 分量活跃集 KKT 带状求解。
//
// 每个 point 的未知量（共 32 组，一组 per lane）：
//   0..2  位置增量（缩放后，物理增量 = v2LengthScale * 分量）
//   3..5  材料帧 body 右扰动（q <- q Exp(分量)）
//   6..8  三个 contact multiplier（物理单位 = 力 N）
//   9     incoming axial multiplier（该点左侧那条 1% 上拉伸约束，力 N）
//
// 与 CPU 参考实现的逐项对应：
//   source/hair/rod/rodContactImplicit.cpp  SelectContactActiveSet / AssembleContact
//   source/hair/rod/rodAxialKkt.cpp         AssembleAxialContactSystem / SolveAxialContactSystem
//   source/hair/rod/rodAxialLimit.cpp       EvaluateAxialLimit
//   shader/glsl/hair/rodV3Kkt10.glsl        RodKkt10AssembleRow（10x10 块装配）
//
// 明确的 GPU 侧差异（不能当作 CPU 等价证明）：
//   * fp32：收敛判据沿用 kernel 的 implicitControls.originTolerance.w，达不到 CPU
//     的 1e-8；gap / 互补条件只到 1e-4 * lengthScale 量级。
//   * workgroup 只有一个 warp（32 lane = 32 point）。装配由 lane == point 单次完成
//     并写入双槽共享缓冲；顺序块 LU 由所有 lane 冗余执行，因此矩阵 / 右端 / 主元
//     工作区放 shared（放 private 会按 lane 复制 32 份并溢出 L1）。
//   * SDF contact 的 Hessian 先用 0（Gauss-Newton）；胶囊用解析 Hessian。
//     CPU 的 MakeRawSdfContact 是中心差分 Hessian，两者不等价。
//   * CPU 的 original linear residual 复核默认关闭（VL_ROD_KKT10_RESIDUAL_CHECK），
//     由牛顿 merit 单调下降 + 每步 r10 范数检查替代。
// ===========================================================================

struct RodKkt10Band
{
    RodKkt10Matrix lower;
    RodKkt10Matrix diagonal;
    RodKkt10Matrix upper;
    RodKkt10Vector residual;
};

// 装配双槽：point 的块 + supernode 需要的次点块。
shared RodKkt10Band rodKktBand[2];
// 块三对角因子：上块（10x10）与右端（10）。按 point 索引。
shared RodKkt10Matrix rodKktFactorUpper[32];
shared RodKkt10Vector rodKktFactorRight[32];
shared RodKkt10Vector rodKktDirection[32];
shared uint rodKktNextPoint[32];
// 主元工作区。20x20 联立块 + 20x11 右端 + 回代解 + 行列缩放 / 置换。
shared float rodKktWorkMatrix[400];
shared float rodKktWorkRight[220];
shared float rodKktSolution[220];
shared float rodKktColumnScale[20];
shared uint rodKktPermutation[20];
// 当前 substep 的胶囊场采样维（rod.capsules 的第一维 = substep 或 substep+1）。
shared uint rodKktSubstep;
// 每个点的 incoming axial 约束共享副本：(multiplier, gap, active, 保留位)。
// 点 p 的 outgoing 边就是点 p+1 的 incoming 边，因此只需要一份。
shared vec4 rodKktAxial[32];
// 本轮装配的可行性：0 表示存在负乘子，牛顿不能提前收敛。
shared uint rodKktFeasible;
// 诊断通道（VL_ROD_KKT10_DEBUG）：最近一次测量的活跃集规模与分量残差。
// x = 活跃 contact 槽数，y = max |contact 残差|，z = max |位置残差|，
// w = max |multiplier| / forceScale。kernel 结尾写进 V3 未使用的
// rodState.guides[].residuals，随 snapshot 的 rod-diagnostics 侧车读回。
shared uvec4 rodKktDebug;
// 重叠修复统计（也只在 DEBUG 下写回）：触发次数与最深重叠（bits）。
shared uint rodKktRepairCount;
shared uint rodKktRepairDepthBits;

// 轴向上拉伸上限：与 CPU rodAxialLimit.h 的 MaximumTensileStrain 相同。
const float RodKktMaximumTensileStrain = 0.01;
const uint RodKktMaxContactCandidates = 9u;

// 活跃集与可行性共用的归一化 gap 容差。
// CPU 用 1e-9 * totalLength（double）；GPU 是 fp32 且收敛判据已经是
// implicitControls.originTolerance.w（1e-4）。沿用 1e-9 会让「穿透」判定
// 永远为真、互补条件永远不满足，从而任何活跃接触都无法宣告收敛。
// 这里统一取同一个归一化尺度，使活跃集阈值、穿透判据与 merit 阈值彼此自洽。
float RodKkt10GapTolerance()
{
    return implicitControls.originTolerance.w * v2LengthScale;
}

// ---- 每 invocation（= 每 point）私有活跃集状态 -----------------------------
// (id, gap, multiplier, enabled)。id 在 [0,7] 是胶囊下标，8 是 SDF 代理场。
vec4 rodKktContacts[3];
uint rodKktExcludedContacts;
bool rodKktAxialExcluded;
// 候选与选择过程的临时缓冲（固定 9 项，便于展开且不进 local memory）。
uint rodKktCandidateId[9];
float rodKktCandidateGap[9];
vec3 rodKktCandidateNormal[9];
vec4 rodKktSelected[3];
vec3 rodKktBasis[3];
uint rodKktSelectedCount;

// ---------------------------------------------------------------------------
// 几何
// ---------------------------------------------------------------------------

// point 的物理位置。V3 的未知量 v2Positions 是「相对固定作者世界 rest 的位移」，
// 物理位置 = 位移 + 补偿低位 + 参考 rest 位置 + 作者原点偏移，与 kernel 结尾发布到
// currentPositions 的 publishedPosition 逐项一致（发布处再乘 100 转厘米）。
// 接触与轴限必须在物理位置里判定：否则会退化到位移空间，胶囊 gap 变成
// 「到原点距离 - 半径」、轴限 gap 恒为正，两者的活跃集永远为空。
// 参考位置与原点偏移都是常量，对缩放未知量的 Jacobian 仍是单位法向。
vec3 RodKkt10PointPosition(uint point)
{
    vec3 position = v2Positions[point].xyz;
#ifdef VL_ROD_V3_COMPENSATED_STATE
    position += v3CandidateLow[point];
#endif
#ifdef VL_ROD_V3_DISPLACEMENT
    position += displacementReferences.guides[gl_WorkGroupID.x].positions[point].xyz;
#endif
    position += implicitControls.originTolerance.xyz;
    return position;
}

// 胶囊有符号距离（点到线段，减半径）。normal 是单位法向。
// 分支与 CPU MakeCapsuleContact / SphereGeometry 相同，包括退化轴的回退法向。
float RodKkt10CapsuleDistance(uint collider, vec3 position, out vec3 normal)
{
    vec3 first = rod.capsules[rodKktSubstep + 1u][collider].a.xyz;
    vec3 last = rod.capsules[rodKktSubstep + 1u][collider].b.xyz;
    float radius = rod.capsules[rodKktSubstep + 1u][collider].a.w;
    vec3 axis = last - first;
    float denominator = dot(axis, axis);
    float fraction = denominator > 0.0 ?
        clamp(dot(position - first, axis) / denominator, 0.0, 1.0) : 0.0;
    vec3 delta = position - first - fraction * axis;
    float distanceValue = length(delta);
    normal = vec3(0, 1, 0);
    if (distanceValue > 1.0e-12)
        normal = delta / distanceValue;
    else if (denominator > 1.0e-24)
    {
        vec3 unitAxis = normalize(axis);
        normal = normalize(cross(unitAxis, abs(unitAxis.y) < 0.9 ?
            vec3(0, 1, 0) : vec3(1, 0, 0)));
    }
    return distanceValue - radius;
}

// 胶囊解析 Hessian：(I - nn^T)/rho，其中 rho = |p - center|（不是 gap）；
// 内段（fraction 在 (0,1)）再减 tt^T/rho。与 CPU SphereGeometry + 胶囊内段修正一致。
mat3 RodKkt10CapsuleHessian(uint collider, vec3 position, vec3 normal)
{
    vec3 first = rod.capsules[rodKktSubstep + 1u][collider].a.xyz;
    vec3 last = rod.capsules[rodKktSubstep + 1u][collider].b.xyz;
    vec3 axis = last - first;
    float denominator = dot(axis, axis);
    float fraction = denominator > 0.0 ?
        clamp(dot(position - first, axis) / denominator, 0.0, 1.0) : 0.0;
    float radius = length(position - first - fraction * axis);
    if (!(radius > 1.0e-12))
        return mat3(0);
    mat3 hessian = (mat3(1) - outerProduct(normal, normal)) / radius;
    if (fraction > 0.0 && fraction < 1.0)
        hessian -= outerProduct(normalize(axis), normalize(axis)) / radius;
    return hessian;
}

#ifdef VL_ROD_SDF
// SDF 代理场：局部厘米场 -> 世界米。gap 与梯度都换算到米，
// 与 rodKernel.glsl 的 V1 SDF 分支（(d - range.x) * 0.01）保持一致。
bool RodKkt10SdfQuery(vec3 position, out float gap, out vec3 gradient)
{
    gap = 0.0;
    gradient = vec3(0);
    if ((rod.flags.w & 1u) == 0u)
        return false;
    vec3 local = (rod.worldToHair * vec4(position * 100.0, 1)).xyz;
    float distanceValue = SdfTrilinear(local);
    if (distanceValue >= SdfOutOfRangeDistance)
        return false;
    vec3 localGradient = SdfGradient(local);
    if (!RodFinite(localGradient) || dot(localGradient, localGradient) <= 1.0e-12)
        return false;
    gap = (distanceValue - sdf.range.x) * 0.01;
    gradient = mat3(rod.hairToWorld) * localGradient;
    return true;
}
#endif

// id -> 几何。gap 是「有符号距离 - margin」的物理米；gradient 的量纲与 gap 一致
// （胶囊是单位向量，SDF 的 |grad| 一般不为 1）。
bool RodKkt10ContactGap(uint id, vec3 position, out float gap, out vec3 gradient)
{
    gap = 0.0;
    gradient = vec3(0);
    if (id < 8u)
    {
        vec3 normal;
        gap = RodKkt10CapsuleDistance(id, position, normal);
        gradient = normal;
        return true;
    }
#ifdef VL_ROD_SDF
    return RodKkt10SdfQuery(position, gap, gradient);
#else
    return false;
#endif
}

mat3 RodKkt10ContactHessian(uint id, vec3 position, vec3 gradient)
{
    if (id < 8u)
        return RodKkt10CapsuleHessian(id, position, normalize(gradient));
    // SDF 的解析二阶导需要 6 次额外三线性采样；M1 先用 Gauss-Newton（0）。
    // 这是一处明确的近似，不能声称与 CPU 中心差分 Hessian 等价。
    return mat3(0);
}

// ---------------------------------------------------------------------------
// 活跃集
// ---------------------------------------------------------------------------

// 把候选 (id, 几何 gap, 梯度, 乘子) 尝试加入选择结果。
// 依赖法向必须剔除，否则该点的 10x10 块奇异（band LU 直接丢秩）：
//   * 平行：投影后残差 -> 0。
//   * 反平行：|dot| -> 1，但投影会把残差放大到 2 倍，CPU 只用「残差范数过小」
//     判定漏掉这一支（两个方向相反的约束 gap 都 < 0 时行向量互相矛盾）。
// 因此这里先按 |dot| 判平行/反平行，再做 Gram-Schmidt。
void RodKkt10TryAddContact(uint id, float gap, vec3 gradient, float multiplier)
{
    if (rodKktSelectedCount >= 3u)
        return;
    vec3 normal = normalize(gradient);
    for (uint axis = 0u; axis < rodKktSelectedCount; ++axis)
        if (abs(dot(rodKktBasis[axis], normal)) > 1.0 - 1.0e-6)
            return;
    for (uint axis = 0u; axis < rodKktSelectedCount; ++axis)
        normal -= rodKktBasis[axis] * dot(rodKktBasis[axis], normal);
    if (!(length(normal) > 1.0e-10))
        return;
    rodKktBasis[rodKktSelectedCount] = normalize(normal);
    rodKktSelected[rodKktSelectedCount] = vec4(float(id), gap, multiplier, 1);
    ++rodKktSelectedCount;
}

// 逐点刷新活跃集：胶囊候选 + SDF 候选，保留上一轮正乘子身份，按几何 gap 升序补足。
// 与 CPU SelectContactActiveSet 的差异：CPU 用 std::sort，这里用固定 9 项的选择排序，
// 但排序键（retained 优先 -> 几何 gap 升序 -> identity 升序）一致。
void RodKkt10UpdateActiveSet(uint point)
{
    vec3 position = RodKkt10PointPosition(point);
    float lengthTolerance = RodKkt10GapTolerance();
    for (uint candidate = 0u; candidate < RodKktMaxContactCandidates; ++candidate)
    {
        rodKktCandidateId[candidate] = 0xFFFFFFFFu;
        rodKktCandidateGap[candidate] = 0.0;
        rodKktCandidateNormal[candidate] = vec3(0);
    }
    // 胶囊数量上限 8；用常量上界循环以便展开，运行期用 params.counts.w 截断。
    for (uint collider = 0u; collider < 8u; ++collider)
    {
        if (collider >= params.counts.w)
            continue;
        float gap;
        vec3 gradient;
        if (!RodKkt10ContactGap(collider, position, gap, gradient))
            continue;
        rodKktCandidateId[collider] = collider;
        rodKktCandidateGap[collider] = gap / max(length(gradient), 1.0e-14);
        rodKktCandidateNormal[collider] = gradient;
    }
#ifdef VL_ROD_SDF
    {
        float gap;
        vec3 gradient;
        if (RodKkt10SdfQuery(position, gap, gradient))
        {
            rodKktCandidateId[8u] = 8u;
            rodKktCandidateGap[8u] = gap / max(length(gradient), 1.0e-14);
            rodKktCandidateNormal[8u] = gradient;
        }
    }
#endif
    rodKktSelectedCount = 0u;
    rodKktSelected[0u] = vec4(-1, 0, 0, 0);
    rodKktSelected[1u] = vec4(-1, 0, 0, 0);
    rodKktSelected[2u] = vec4(-1, 0, 0, 0);
    // 1) 保留上一轮已活跃且乘子为正的身份（CPU 按 identity 升序保留）。
    uint retainedId[3];
    float retainedMultiplier[3];
    uint retainedCount = 0u;
    for (uint slot = 0u; slot < 3u; ++slot)
        if (rodKktContacts[slot].w != 0.0 && rodKktContacts[slot].x >= 0.0 &&
            rodKktContacts[slot].z > 0.0)
        {
            uint id = uint(rodKktContacts[slot].x);
            if (id < RodKktMaxContactCandidates &&
                rodKktCandidateId[id] != 0xFFFFFFFFu &&
                (rodKktExcludedContacts & (1u << id)) == 0u)
            {
                retainedId[retainedCount] = id;
                retainedMultiplier[retainedCount] = rodKktContacts[slot].z;
                ++retainedCount;
            }
        }
    for (uint first = 0u; first < retainedCount; ++first)
        for (uint second = first + 1u; second < retainedCount; ++second)
            if (retainedId[second] < retainedId[first])
            {
                uint swapId = retainedId[first];
                retainedId[first] = retainedId[second];
                retainedId[second] = swapId;
                float swapValue = retainedMultiplier[first];
                retainedMultiplier[first] = retainedMultiplier[second];
                retainedMultiplier[second] = swapValue;
            }
    for (uint retained = 0u; retained < retainedCount; ++retained)
    {
        uint id = retainedId[retained];
        if (rodKktCandidateGap[id] < -lengthTolerance)
            rodKktExcludedContacts &= ~(1u << id);
        RodKkt10TryAddContact(id, rodKktCandidateGap[id],
            rodKktCandidateNormal[id], retainedMultiplier[retained]);
    }
    // 2) 其余候选按几何 gap 升序补足；被排除的身份只在再次穿透时才允许回来。
    for (uint pass = 0u; pass < RodKktMaxContactCandidates; ++pass)
    {
        uint best = 0xFFFFFFFFu;
        for (uint candidate = 0u; candidate < RodKktMaxContactCandidates; ++candidate)
        {
            if (rodKktCandidateId[candidate] == 0xFFFFFFFFu)
                continue;
            bool alreadySelected = false;
            for (uint slot = 0u; slot < rodKktSelectedCount; ++slot)
                if (uint(rodKktSelected[slot].x) == candidate)
                    alreadySelected = true;
            if (alreadySelected)
                continue;
            if ((rodKktExcludedContacts & (1u << candidate)) != 0u &&
                rodKktCandidateGap[candidate] >= -lengthTolerance)
                continue;
            if (rodKktCandidateGap[candidate] > lengthTolerance)
                continue;
            if (best == 0xFFFFFFFFu ||
                rodKktCandidateGap[candidate] < rodKktCandidateGap[best])
                best = candidate;
        }
        if (best == 0xFFFFFFFFu)
            break;
        if (rodKktCandidateGap[best] < -lengthTolerance)
            rodKktExcludedContacts &= ~(1u << best);
        RodKkt10TryAddContact(best, rodKktCandidateGap[best],
            rodKktCandidateNormal[best], 0.0);
    }
    for (uint slot = 0u; slot < 3u; ++slot)
        rodKktContacts[slot] = slot < rodKktSelectedCount ?
            rodKktSelected[slot] : vec4(-1, 0, 0, 0);
}

// 逐点刷新 incoming axial 约束。点 0 没有左侧边，恒为不活跃（CPU 的 row 9 单位行）。
void RodKkt10UpdateAxial(uint point)
{
    vec4 previous = rodKktAxial[point];
    vec4 current = vec4(0);
#ifndef VL_ROD_KKT10_AXIAL_LIMIT
    // M1 阶段关闭第 10 分量：见文件头差异说明。row 9 退化为单位行。
    rodKktAxial[point] = current;
    return;
#else
    if (point > 0u)
    {
        bool leftFree = v2Positions[point - 1u].w != 0.0;
        bool rightFree = v2Positions[point].w != 0.0;
        if (leftFree || rightFree)
        {
            float restLength = restStrains[point - 1u].w;
            vec3 left = RodKkt10PointPosition(point - 1u);
            vec3 right = RodKkt10PointPosition(point);
            float lengthValue = length(right - left);
            float gap = (1.0 + RodKktMaximumTensileStrain) * restLength - lengthValue;
            float lengthTolerance = RodKkt10GapTolerance();
            bool penetrates = gap < -lengthTolerance;
            bool retained = previous.z != 0.0 && previous.x > 0.0 && !rodKktAxialExcluded;
            bool tensileActive = penetrates ||
                (!rodKktAxialExcluded && gap <= lengthTolerance) || retained;
            if (tensileActive && penetrates)
                rodKktAxialExcluded = false;
            if (tensileActive)
                current = vec4(previous.z != 0.0 ? previous.x : 0.0, gap, 1.0, 0.0);
        }
    }
    rodKktAxial[point] = current;
#endif
}

// ---------------------------------------------------------------------------
// 装配
// ---------------------------------------------------------------------------

void RodKkt10BuildEdges(uint point, out RodKkt10Edge incoming, out RodKkt10Edge outgoing)
{
    incoming.enabled = false;
    incoming.direction = vec3(0);
    incoming.normalDerivative = mat3(0);
    incoming.gap = 0.0;
    incoming.multiplier = 0.0;
    incoming.leftFree = false;
    incoming.rightFree = false;
    outgoing = incoming;
    if (point > 0u && rodKktAxial[point].z != 0.0)
    {
        vec3 left = RodKkt10PointPosition(point - 1u);
        vec3 right = RodKkt10PointPosition(point);
        float lengthValue = length(right - left);
        if (lengthValue > 1.0e-14)
        {
            incoming.enabled = true;
            incoming.direction = (right - left) / lengthValue;
            incoming.normalDerivative =
                (mat3(1) - outerProduct(incoming.direction, incoming.direction)) / lengthValue;
            incoming.gap = rodKktAxial[point].y;
            incoming.multiplier = rodKktAxial[point].x;
            incoming.leftFree = v2Positions[point - 1u].w != 0.0;
            incoming.rightFree = v2Positions[point].w != 0.0;
        }
    }
    if (point + 1u < 32u && rodKktAxial[point + 1u].z != 0.0)
    {
        vec3 left = RodKkt10PointPosition(point);
        vec3 right = RodKkt10PointPosition(point + 1u);
        float lengthValue = length(right - left);
        if (lengthValue > 1.0e-14)
        {
            outgoing.enabled = true;
            outgoing.direction = (right - left) / lengthValue;
            outgoing.normalDerivative =
                (mat3(1) - outerProduct(outgoing.direction, outgoing.direction)) / lengthValue;
            outgoing.gap = rodKktAxial[point + 1u].y;
            outgoing.multiplier = rodKktAxial[point + 1u].x;
            outgoing.leftFree = v2Positions[point].w != 0.0;
            outgoing.rightFree = v2Positions[point + 1u].w != 0.0;
        }
    }
}

// 把本点的活跃接触 / 轴限打包成 RodKkt10AssembleRow 需要的输入。
void RodKkt10BuildContacts(uint point, vec3 position, out RodKkt10Contact contacts[3])
{
    for (uint slot = 0u; slot < 3u; ++slot)
    {
        contacts[slot].enabled = false;
        contacts[slot].gradient = vec3(0);
        contacts[slot].hessian = mat3(0);
        contacts[slot].gap = 0.0;
        contacts[slot].multiplier = 0.0;
        if (rodKktContacts[slot].x < 0.0 || rodKktContacts[slot].w == 0.0)
            continue;
        uint id = uint(rodKktContacts[slot].x);
        float gap;
        vec3 gradient;
        if (!RodKkt10ContactGap(id, position, gap, gradient))
            continue;
        contacts[slot].enabled = true;
        contacts[slot].gradient = gradient;
        contacts[slot].gap = gap;
        contacts[slot].multiplier = rodKktContacts[slot].z;
        contacts[slot].hessian = RodKkt10ContactHessian(id, position, gradient);
    }
}

// 由 lane == point 调用：一次装配出该点的 10x10 下 / 对角 / 上块与 10 残差。
RodKkt10Band RodKkt10AssemblePoint(uint point)
{
    RodKkt10Band band;
    RodV2Matrix baseLower = point > 0u ? RodV2Tangent(point, point - 1u) : RodV2Zero();
    RodV2Matrix baseDiagonal = RodV2Tangent(point, point);
    RodV2Matrix baseUpper = point < 31u ? RodV2Tangent(point, point + 1u) : RodV2Zero();
    RodV2Vector baseResidual = RodV2Residual(point);
    RodKkt10Contact contacts[3];
    RodKkt10BuildContacts(point, RodKkt10PointPosition(point), contacts);
    RodKkt10Edge incoming;
    RodKkt10Edge outgoing;
    RodKkt10BuildEdges(point, incoming, outgoing);
    RodKkt10AssembleRow(baseLower, baseDiagonal, baseUpper, baseResidual,
        v2Positions[point].w != 0.0, contacts, incoming, outgoing,
        v2ForceScale, v2LengthScale,
        band.lower, band.diagonal, band.upper, band.residual);
    return band;
}

// 只算 10 残差（不装配切线）。线搜索要反复测量，切线装配太贵。
// 每一项都必须与 RodKkt10AssembleRow 的残差行保持一致。
RodKkt10Vector RodKkt10ResidualVector(uint point)
{
    RodV2Vector base = RodV2Residual(point);
    RodKkt10Vector value;
    for (uint component = 0u; component < 10u; ++component)
        value.values[component] = component < 3u ? base.linear[component] :
            component < 6u ? base.angular[component - 3u] : 0.0;
    bool freePoint = v2Positions[point].w != 0.0;
    RodKkt10Contact contacts[3];
    RodKkt10BuildContacts(point, RodKkt10PointPosition(point), contacts);
    for (uint slot = 0u; slot < 3u; ++slot)
    {
        if (!freePoint || !contacts[slot].enabled)
            continue;
        for (uint axis = 0u; axis < 3u; ++axis)
            value.values[axis] -= contacts[slot].multiplier *
                contacts[slot].gradient[axis] / v2ForceScale;
        value.values[6u + slot] = contacts[slot].gap / v2LengthScale;
    }
    RodKkt10Edge incoming;
    RodKkt10Edge outgoing;
    RodKkt10BuildEdges(point, incoming, outgoing);
    if (incoming.enabled && (incoming.leftFree || incoming.rightFree))
    {
        value.values[9] = incoming.gap / v2LengthScale;
        if (incoming.rightFree)
            for (uint axis = 0u; axis < 3u; ++axis)
                value.values[axis] += incoming.multiplier *
                    incoming.direction[axis] / v2ForceScale;
    }
    if (outgoing.enabled && outgoing.leftFree)
        for (uint axis = 0u; axis < 3u; ++axis)
            value.values[axis] -= outgoing.multiplier *
                outgoing.direction[axis] / v2ForceScale;
    return value;
}

// ---------------------------------------------------------------------------
// 重叠修复
// ---------------------------------------------------------------------------

// 位置投影式接触修复，对应 CPU 的 RepairInitialContactOverlap。
// 为什么需要它：隐式牛顿的 merit 是整根 guide 的残差平方和。当作者形状
// 「整段穿进粗胶囊」时（本文验证资产里 chest 胶囊就会），一次到位的大位移
// 会让相邻段的剪切/弯曲能暴涨，Armijo 只接受极小的 alpha，24 轮之后仍然停在
// 「穿透但没有更差」的不动点上（实测 contact 残差 0.049 不再下降）。
// CPU 因此在步进前单独做投影修复；这里在每个 substep 起点做同一件事，
// 并且只在重叠超过总长 1% 时才动，避免把正常的小穿透从隐式求解手里夺走。
// 代价：修复直接改位置、不携带动量，属于声明的稳定化步骤而非隐式解的一部分。
void RodKkt10RepairOverlap(uint point)
{
    if (v2Positions[point].w == 0.0)
        return;
    float threshold = 0.01 * v2LengthScale;
    float deepest = 0.0;
    for (uint iteration = 0u; iteration < 16u; ++iteration)
    {
        vec3 position = RodKkt10PointPosition(point);
        vec3 correction = vec3(0);
        float localDeepest = 0.0;
        for (uint collider = 0u; collider < 8u; ++collider)
        {
            if (collider >= params.counts.w)
                continue;
            float gap;
            vec3 gradient;
            if (!RodKkt10ContactGap(collider, position, gap, gradient))
                continue;
            float squared = max(dot(gradient, gradient), 1.0e-28);
            float geometricGap = gap / sqrt(squared);
            if (geometricGap >= -threshold)
                continue;
            correction -= gap * gradient / squared;
            localDeepest = min(localDeepest, geometricGap);
        }
#ifdef VL_ROD_SDF
        {
            float gap;
            vec3 gradient;
            if (RodKkt10SdfQuery(position, gap, gradient))
            {
                float squared = max(dot(gradient, gradient), 1.0e-28);
                float geometricGap = gap / sqrt(squared);
                if (geometricGap < -threshold)
                {
                    correction -= gap * gradient / squared;
                    localDeepest = min(localDeepest, geometricGap);
                }
            }
        }
#endif
        if (!(localDeepest < -threshold))
            break;
        v2Positions[point].xyz += correction;
        deepest = min(deepest, localDeepest);
    }
    if (deepest < 0.0)
    {
        atomicAdd(rodKktRepairCount, 1u);
        atomicMax(rodKktRepairDepthBits, floatBitsToUint(-deepest));
    }
}

// ---------------------------------------------------------------------------
// 测量（merit / 最大分量 / 可行性）
// ---------------------------------------------------------------------------

// 与 RodV2Measure 同构，但残差取 10 分量，并顺带检查负乘子。
// 不可行（存在负乘子）时不允许提前收敛。CPU 另外要求互补 |lambda*gap|/(F*L) <= 1e-9；
// GPU 侧 gap 本身已经是 merit 的一个分量，1e-9 在 fp32 上不可达，故以 merit 阈值代替，
// 只单独保留乘子符号条件。
void RodKkt10Measure(uint lane, bool incrementValid)
{
    RodKkt10Vector residual = RodKkt10ResidualVector(lane);
    bool valid = incrementValid && RodFinite(v2Positions[lane].xyz);
    float squared = 0.0;
    float maximum = 0.0;
    for (uint component = 0u; component < 10u; ++component)
    {
        if (isnan(residual.values[component]) || isinf(residual.values[component]))
            valid = false;
        squared += residual.values[component] * residual.values[component];
        maximum = max(maximum, abs(residual.values[component]));
    }
    if (lane < 31u)
    {
        valid = valid && length(RodLog(RodMul(RodConjugate(oldOrientations[lane]),
            v2Orientations[lane]))) < RodRotationLimit;
        if (lane < 30u)
            valid = valid && length(RodLog(RodV2RelativeJoint(lane, v2Orientations[lane],
                v2Orientations[lane + 1u]))) < RodRotationLimit;
    }
    v2Metric[lane] = vec4(squared, maximum, 0, valid ? 1.0 : 0.0);
#ifdef VL_ROD_KKT10_DEBUG
    // 每个 measure 重新累积，保留的是「最近一次测量」的画像。
    if (lane == 0u)
        rodKktDebug = uvec4(0u);
    barrier();
    float contactResidual = 0.0;
    for (uint component = 6u; component < 9u; ++component)
        contactResidual = max(contactResidual, abs(residual.values[component]));
    float positionResidual = 0.0;
    for (uint component = 0u; component < 6u; ++component)
        positionResidual = max(positionResidual, abs(residual.values[component]));
    uint activeSlots = 0u;
    float maximumMultiplier = 0.0;
    for (uint slot = 0u; slot < 3u; ++slot)
        if (rodKktContacts[slot].w != 0.0)
        {
            ++activeSlots;
            maximumMultiplier = max(maximumMultiplier,
                abs(rodKktContacts[slot].z) / v2ForceScale);
        }
    atomicAdd(rodKktDebug.x, activeSlots);
    atomicMax(rodKktDebug.y, floatBitsToUint(contactResidual));
    atomicMax(rodKktDebug.z, floatBitsToUint(positionResidual));
    atomicMax(rodKktDebug.w, floatBitsToUint(maximumMultiplier));
#endif
    barrier();
    if (lane == 0u)
    {
        rodKktFeasible = 1u;
        v2Merit = 0.0;
        v2Maximum = 0.0;
        v2DomainValid = 1u;
        for (uint point = 0u; point < 32u; ++point)
        {
            v2Merit += v2Metric[point].x;
            v2Maximum = max(v2Maximum, v2Metric[point].y);
            if (v2Metric[point].w == 0.0)
                v2DomainValid = 0u;
        }
        if (isnan(v2Merit) || isinf(v2Merit))
            v2DomainValid = 0u;
    }
    barrier();
    // 逐点可行性：负乘子剔除并继续迭代。
    bool feasible = true;
    if (v2Positions[lane].w != 0.0)
        for (uint slot = 0u; slot < 3u; ++slot)
        {
            if (rodKktContacts[slot].x < 0.0 || rodKktContacts[slot].w == 0.0)
                continue;
            if (rodKktContacts[slot].z < -1.0e-9 * v2ForceScale)
            {
                rodKktExcludedContacts |= 1u << uint(rodKktContacts[slot].x);
                feasible = false;
            }
        }
    if (rodKktAxial[lane].z != 0.0 &&
        rodKktAxial[lane].x < -1.0e-9 * v2ForceScale)
    {
        rodKktAxialExcluded = true;
        feasible = false;
    }
    if (!feasible)
        atomicMin(rodKktFeasible, 0u);
    barrier();
}

// ---------------------------------------------------------------------------
// 主元与带状求解
// ---------------------------------------------------------------------------

// 等比例行 / 列缩放 + 全主元 LU，右端列 0..9 是上块，列 10 是残差。
// 逐项对应 CPU rodAxialKkt.cpp 的 EquilibratedPivot 与 SolveAxialContactSystem。
// 工作区在 shared：本 workgroup 只有一个 warp，所有 lane 冗余执行同一段代码，
// 写入的是同一组值，因此不需要（也不该）为每个 lane 复制一份 private 数组。
bool RodKktEquilibratedPivot(uint dimension, out float pivotRatio)
{
    pivotRatio = 1.0;
    for (uint row = 0u; row < dimension; ++row)
    {
        float maximum = 0.0;
        for (uint column = 0u; column < dimension; ++column)
            maximum = max(maximum, abs(rodKktWorkMatrix[row * 20u + column]));
        float scale = maximum > 0.0 ? 1.0 / maximum : 1.0;
        for (uint column = 0u; column < dimension; ++column)
            rodKktWorkMatrix[row * 20u + column] *= scale;
        for (uint entry = 0u; entry < 11u; ++entry)
            rodKktWorkRight[row * 11u + entry] *= scale;
    }
    for (uint column = 0u; column < dimension; ++column)
    {
        float maximum = 0.0;
        for (uint row = 0u; row < dimension; ++row)
            maximum = max(maximum, abs(rodKktWorkMatrix[row * 20u + column]));
        rodKktColumnScale[column] = maximum > 0.0 ? 1.0 / maximum : 1.0;
        rodKktPermutation[column] = column;
        for (uint row = 0u; row < dimension; ++row)
            rodKktWorkMatrix[row * 20u + column] *= rodKktColumnScale[column];
    }
    float maximumPivot = 0.0;
    float minimumPivot = 3.402823466e38;
    for (uint column = 0u; column < dimension; ++column)
    {
        float largest = 0.0;
        uint pivotRow = column;
        uint pivotColumn = column;
        for (uint row = column; row < dimension; ++row)
            for (uint entry = column; entry < dimension; ++entry)
                if (abs(rodKktWorkMatrix[row * 20u + entry]) > largest)
                {
                    largest = abs(rodKktWorkMatrix[row * 20u + entry]);
                    pivotRow = row;
                    pivotColumn = entry;
                }
        if (largest == 0.0 || isnan(largest) || isinf(largest))
            return false;
        maximumPivot = max(maximumPivot, largest);
        minimumPivot = min(minimumPivot, largest);
        for (uint entry = 0u; entry < dimension; ++entry)
        {
            float value = rodKktWorkMatrix[column * 20u + entry];
            rodKktWorkMatrix[column * 20u + entry] = rodKktWorkMatrix[pivotRow * 20u + entry];
            rodKktWorkMatrix[pivotRow * 20u + entry] = value;
        }
        for (uint entry = 0u; entry < 11u; ++entry)
        {
            float value = rodKktWorkRight[column * 11u + entry];
            rodKktWorkRight[column * 11u + entry] = rodKktWorkRight[pivotRow * 11u + entry];
            rodKktWorkRight[pivotRow * 11u + entry] = value;
        }
        for (uint row = 0u; row < dimension; ++row)
        {
            float value = rodKktWorkMatrix[row * 20u + column];
            rodKktWorkMatrix[row * 20u + column] = rodKktWorkMatrix[row * 20u + pivotColumn];
            rodKktWorkMatrix[row * 20u + pivotColumn] = value;
        }
        uint original = rodKktPermutation[column];
        rodKktPermutation[column] = rodKktPermutation[pivotColumn];
        rodKktPermutation[pivotColumn] = original;
        for (uint row = column + 1u; row < dimension; ++row)
        {
            float factor = rodKktWorkMatrix[row * 20u + column] /
                rodKktWorkMatrix[column * 20u + column];
            for (uint entry = column; entry < dimension; ++entry)
                rodKktWorkMatrix[row * 20u + entry] -=
                    factor * rodKktWorkMatrix[column * 20u + entry];
            for (uint entry = 0u; entry < 11u; ++entry)
                rodKktWorkRight[row * 11u + entry] -=
                    factor * rodKktWorkRight[column * 11u + entry];
        }
    }
    // CPU 阈值：64 * n * epsilon(double)。GPU 用 fp32 epsilon，比例相同。
    if (minimumPivot <= 64.0 * float(dimension) * 1.1920928955078125e-7 * maximumPivot)
        return false;
    pivotRatio = minimumPivot / maximumPivot;
    for (int row = int(dimension) - 1; row >= 0; --row)
        for (uint entry = 0u; entry < 11u; ++entry)
        {
            float value = rodKktWorkRight[uint(row) * 11u + entry];
            for (uint column = uint(row + 1); column < dimension; ++column)
                value -= rodKktWorkMatrix[uint(row) * 20u + column] *
                    rodKktSolution[column * 11u + entry];
            rodKktSolution[uint(row) * 11u + entry] =
                value / rodKktWorkMatrix[uint(row) * 20u + uint(row)];
        }
    for (uint row = 0u; row < dimension; ++row)
        for (uint entry = 0u; entry < 11u; ++entry)
            rodKktWorkRight[rodKktPermutation[row] * 11u + entry] =
                rodKktColumnScale[rodKktPermutation[row]] *
                    rodKktSolution[row * 11u + entry];
    return true;
}

// 单点 10x10：把对角 / 上块 / 残差装进工作区后做一次主元分解。
bool RodKkt10FactorNode(RodKkt10Matrix diagonal, RodKkt10Matrix upper,
    RodKkt10Vector residual, out float pivotRatio)
{
    for (uint row = 0u; row < 10u; ++row)
    {
        for (uint column = 0u; column < 10u; ++column)
        {
            rodKktWorkMatrix[row * 20u + column] = diagonal.values[row * 10u + column];
            rodKktWorkRight[row * 11u + column] = upper.values[row * 10u + column];
        }
        rodKktWorkRight[row * 11u + 10u] = residual.values[row];
    }
    return RodKktEquilibratedPivot(10u, pivotRatio);
}

// 局部秩亏时把下一点并入 supernode（CPU 的 20x20 联立分支）。
bool RodKkt10FactorSupernode(RodKkt10Matrix diagonal, RodKkt10Matrix upper,
    RodKkt10Vector residual, out float pivotRatio)
{
    for (uint row = 0u; row < 20u; ++row)
    {
        for (uint column = 0u; column < 20u; ++column)
            rodKktWorkMatrix[row * 20u + column] = row < 10u ?
                (column < 10u ? diagonal.values[row * 10u + column] :
                    upper.values[row * 10u + column - 10u]) :
                (column < 10u ? rodKktBand[1u].lower.values[(row - 10u) * 10u + column] :
                    rodKktBand[1u].diagonal.values[(row - 10u) * 10u + column - 10u]);
        for (uint column = 0u; column < 10u; ++column)
            rodKktWorkRight[row * 11u + column] = row < 10u ? 0.0 :
                rodKktBand[1u].upper.values[(row - 10u) * 10u + column];
        rodKktWorkRight[row * 11u + 10u] = row < 10u ? residual.values[row] :
            -rodKktBand[1u].residual.values[row - 10u];
    }
    return RodKktEquilibratedPivot(20u, pivotRatio);
}

// 全 lane 冗余执行的前向 / 后向代入。
// 返回状态码：0 成功、1 空、2 末端单点秩亏、3 两点 supernode 秩亏、4 原始线性残差超限。
uint RodKkt10SolveBand(uint count)
{
    if (count == 0u || count > 32u)
        return 1u;
    uint lane = gl_LocalInvocationID.x;
    uint status = 0u;
    float smallestPivotRatio = 1.0;
    uint point = 0u;
    while (point < count)
    {
        if (lane == point)
            rodKktBand[0u] = RodKkt10AssemblePoint(point);
        barrier();
        RodKkt10Matrix diagonal = rodKktBand[0u].diagonal;
        RodKkt10Matrix upper = rodKktBand[0u].upper;
        RodKkt10Vector residual;
        for (uint row = 0u; row < 10u; ++row)
            residual.values[row] = -rodKktBand[0u].residual.values[row];
        if (point > 0u)
        {
            RodKkt10Matrix lower = rodKktBand[0u].lower;
            for (uint row = 0u; row < 10u; ++row)
                for (uint inner = 0u; inner < 10u; ++inner)
                {
                    residual.values[row] -= lower.values[row * 10u + inner] *
                        rodKktFactorRight[point - 1u].values[inner];
                    for (uint column = 0u; column < 10u; ++column)
                        diagonal.values[row * 10u + column] -=
                            lower.values[row * 10u + inner] *
                                rodKktFactorUpper[point - 1u].values[inner * 10u + column];
                }
        }
        uint nodes = 1u;
        float pivotRatio = 1.0;
        bool factored = RodKkt10FactorNode(diagonal, upper, residual, pivotRatio);
        if (!factored)
        {
            if (point + 1u >= count)
            {
                status = 2u;
                break;
            }
            if (lane == point + 1u)
                rodKktBand[1u] = RodKkt10AssemblePoint(point + 1u);
            barrier();
            nodes = 2u;
            factored = RodKkt10FactorSupernode(diagonal, upper, residual, pivotRatio);
            if (!factored)
            {
                status = 3u;
                break;
            }
        }
        smallestPivotRatio = min(smallestPivotRatio, pivotRatio);
        for (uint node = 0u; node < nodes; ++node)
        {
            rodKktNextPoint[point + node] = point + nodes;
            for (uint row = 0u; row < 10u; ++row)
            {
                rodKktFactorRight[point + node].values[row] =
                    rodKktWorkRight[(node * 10u + row) * 11u + 10u];
                for (uint column = 0u; column < 10u; ++column)
                    rodKktFactorUpper[point + node].values[row * 10u + column] =
                        rodKktWorkRight[(node * 10u + row) * 11u + column];
            }
        }
        barrier();
        point += nodes;
    }
    if (status != 0u)
        return status;
    for (int index = int(count) - 1; index >= 0; --index)
    {
        for (uint row = 0u; row < 10u; ++row)
        {
            float value = rodKktFactorRight[index].values[row];
            uint next = rodKktNextPoint[index];
            if (next < count)
                for (uint column = 0u; column < 10u; ++column)
                    value -= rodKktFactorUpper[index].values[row * 10u + column] *
                        rodKktDirection[next].values[column];
            rodKktDirection[index].values[row] = value;
        }
        // 下一步会读别人写的方向；单 warp 也显式同步，避免依赖实现细节。
        barrier();
    }
#ifdef VL_ROD_KKT10_RESIDUAL_CHECK
    // CPU 复核 A d = b。需要重新装配所有点的块，默认关闭以省掉一轮切线装配。
    float originalResidual = 0.0;
    if (lane == 0u)
        for (uint index = 0u; index < count; ++index)
        {
            RodKkt10Band band = RodKkt10AssemblePoint(index);
            for (uint row = 0u; row < 10u; ++row)
            {
                float value = band.residual.values[row];
                for (uint column = 0u; column < 10u; ++column)
                {
                    value += band.diagonal.values[row * 10u + column] *
                        rodKktDirection[index].values[column];
                    if (index > 0u)
                        value += band.lower.values[row * 10u + column] *
                            rodKktDirection[index - 1u].values[column];
                    if (index + 1u < count)
                        value += band.upper.values[row * 10u + column] *
                            rodKktDirection[index + 1u].values[column];
                }
                if (isnan(value) || isinf(value))
                    return 4u;
                originalResidual = max(originalResidual, abs(value));
            }
        }
    if (originalResidual > 1.0e-5)
        return 5u;
#endif
    return 0u;
}

#endif
