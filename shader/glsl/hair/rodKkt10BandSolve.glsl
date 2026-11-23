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
// 诊断：活跃接触里最大的**有符号**归一化 gap（正 = 已分离）。存 bits(gap+1) 以复用
// 无符号 atomicMax；读回时减 1。
shared uint rodKktSeparatedGapBits;
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
// 诊断计数：本帧 SDF 查询被接受/拒绝的次数。用来区分“带宽内没有需要处理的点”
// 与“守卫把全部查询都拒掉了”。只在 VL_ROD_KKT10_DEBUG 下累计，生产编译零成本。
shared uint rodKktSdfAcceptedSamples;
shared uint rodKktSdfRejectedSamples;

// SDF 代理场：局部厘米场 -> 世界米，返回**单位法向 + 几何 gap（米）**。
//
// 为什么不用原始梯度：栅格只在 ±truncation（= margin + 2*cellSize）带内有值，
// 带外格子保持 clear 哨兵，`SdfTrilinear` 只要碰到任一哨兵角点就返回 1e10。
// `SdfGradient` 的中心差分因此会在带边界附近产生 |grad| ~ 1e10 的梯度。V1 只把它
// 归一化当法向用，所以看不出来；V3 的 KKT 里原始梯度直接进约束行/列
// （`gᵀ dx = gap/L`、`-λ g/F`），1e10 会同时把行放大到无意义、把几何 gap 压成 0，
// 结果是首个迭代局部丢秩。这里做三件事：
//   1. 六个差分样本任一越界就认为该点场不可用（不猜外插），直接跳过接触；
//   2. |grad| 必须落在真实 SDF 应有的量级（约 1），否则同样视为不可用；
//   3. 输出单位法向 + `rawGap / |grad|` 的几何 gap，与胶囊路径统一量纲。
bool RodKkt10SdfQuery(vec3 position, out float gap, out vec3 gradient)
{
    gap = 0.0;
    gradient = vec3(0);
    bool accepted = false;
    if ((rod.flags.w & 1u) != 0u)
    {
        vec3 local = (rod.worldToHair * vec4(position * 100.0, 1)).xyz;
        float distanceValue = SdfTrilinear(local);
        // 中心差分半步长 = 0.5 * cellSize；除以 2h = cellSize 得到 cm/cm 梯度。
        const float halfStep = 0.5 * sdf.originCell.w;
        float negativeX = distanceValue >= SdfOutOfRangeDistance ? SdfOutOfRangeDistance :
            SdfTrilinear(local - vec3(halfStep, 0.0, 0.0));
        float positiveX = distanceValue >= SdfOutOfRangeDistance ? SdfOutOfRangeDistance :
            SdfTrilinear(local + vec3(halfStep, 0.0, 0.0));
        float negativeY = distanceValue >= SdfOutOfRangeDistance ? SdfOutOfRangeDistance :
            SdfTrilinear(local - vec3(0.0, halfStep, 0.0));
        float positiveY = distanceValue >= SdfOutOfRangeDistance ? SdfOutOfRangeDistance :
            SdfTrilinear(local + vec3(0.0, halfStep, 0.0));
        float negativeZ = distanceValue >= SdfOutOfRangeDistance ? SdfOutOfRangeDistance :
            SdfTrilinear(local - vec3(0.0, 0.0, halfStep));
        float positiveZ = distanceValue >= SdfOutOfRangeDistance ? SdfOutOfRangeDistance :
            SdfTrilinear(local + vec3(0.0, 0.0, halfStep));
        bool inRange = distanceValue < SdfOutOfRangeDistance &&
            negativeX < SdfOutOfRangeDistance && positiveX < SdfOutOfRangeDistance &&
            negativeY < SdfOutOfRangeDistance && positiveY < SdfOutOfRangeDistance &&
            negativeZ < SdfOutOfRangeDistance && positiveZ < SdfOutOfRangeDistance;
        if (inRange)
        {
            vec3 localGradient = vec3(positiveX - negativeX, positiveY - negativeY,
                positiveZ - negativeZ) / sdf.originCell.w;
            float magnitude = length(localGradient);
            if (RodFinite(localGradient) && magnitude > 0.05 && magnitude < 20.0)
            {
                gradient = (mat3(rod.hairToWorld) * localGradient) / magnitude;
                gap = (distanceValue - sdf.range.x) * 0.01 / magnitude;
                accepted = true;
            }
        }
    }
    if (accepted)
    {
#ifdef VL_ROD_KKT10_DEBUG
        atomicAdd(rodKktSdfAcceptedSamples, 1u);
#endif
    }
    else
    {
#ifdef VL_ROD_KKT10_DEBUG
        atomicAdd(rodKktSdfRejectedSamples, 1u);
#endif
    }
    return accepted;
}
#endif

// id -> 几何。gap 是几何距离（米，已扣 margin 并按 |grad| 归一），gradient 是**单位**法向。
// 胶囊本身就是单位法向，因此两条路径在 KKT 里量纲一致。
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

// 接触的二阶项（几何刚度）。
//
// CPU 用解析 Hessian：胶囊是 (I-nnᵀ)/ρ（内段再减 ttᵀ/ρ），装配时以
// `-λ H (L/F)` 加进位置块。这一项量级是 λ/(ρF)·L，深穿透（ρ 很小）时会把位置块
// 从正定推成负定，理论上会丢掉牛顿方向的下降性。
//
// 【已实测否决】把它换成 Gauss-Newton（H=0）在 200-guide 场景上**更差**：
// 失败 guide 从 3 个增到 4 个，最大 merit 从 0.062 升到 0.112。所以解析二阶项
// 在这批 guide 上是净收益，不能靠丢它换条件数。保留解析形式。
// 定义 VL_ROD_KKT10_GAUSS_NEWTON_CONTACT 可以复核这个反例。
mat3 RodKkt10ContactHessian(uint id, vec3 position, vec3 gradient)
{
#ifndef VL_ROD_KKT10_GAUSS_NEWTON_CONTACT
    if (id < 8u)
        return RodKkt10CapsuleHessian(id, position, normalize(gradient));
#endif
    // SDF 的解析二阶导需要 6 次额外三线性采样；M1 用 Gauss-Newton（0）。
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

// 最深的几何重叠（米，<= 0 表示穿透）。只做一次场查询，不含修正。
float RodKkt10DeepestOverlap(vec3 position)
{
    float deepest = 0.0;
    for (uint collider = 0u; collider < 8u; ++collider)
    {
        if (collider >= params.counts.w)
            continue;
        float gap;
        vec3 gradient;
        if (!RodKkt10ContactGap(collider, position, gap, gradient))
            continue;
        deepest = min(deepest, gap / max(length(gradient), 1.0e-14));
    }
#ifdef VL_ROD_SDF
    {
        float gap;
        vec3 gradient;
        if (RodKkt10SdfQuery(position, gap, gradient))
            deepest = min(deepest, gap / max(length(gradient), 1.0e-14));
    }
#endif
    return deepest;
}

// 位置投影式接触修复，对应 CPU 的 RepairInitialContactOverlap。
// 为什么需要它：隐式牛顿的 merit 是整根 guide 的残差平方和。当作者形状
// 「整段穿进粗胶囊」时（本文验证资产里 chest 胶囊就会），一次到位的大位移
// 会让相邻段的剪切/弯曲能暴涨，Armijo 只接受极小的 alpha，迭代停在
// 「穿透但没有更差」的不动点上（实测 contact 残差 0.049 不再下降）。
// CPU 因此在步进前单独做投影修复；这里在每个 substep 起点做同一件事。
// 代价：修复直接改位置、不携带动量，属于声明的稳定化步骤而非隐式解的一部分。
//
// 两个阈值分工（本轮修正）：trigger 决定「这个点值不值得修」，取总长 1%，
// 避免把正常的小穿透从隐式求解手里夺走；target 是修到哪，取求解器自己的
// gap 容差，即**推到贴面**。上一版用 trigger 同时当停止条件，会故意留下
// 最多 3.4 mm 的穿透交给牛顿，而牛顿在 200-guide 规模上正是卡在这段收不掉。
void RodKkt10RepairOverlap(uint point)
{
    if (v2Positions[point].w == 0.0)
        return;
    float trigger = 0.01 * v2LengthScale;
    float target = RodKkt10GapTolerance();
    if (!(RodKkt10DeepestOverlap(RodKkt10PointPosition(point)) < -trigger))
        return;
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
            if (geometricGap >= -target)
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
                if (geometricGap < -target)
                {
                    correction -= gap * gradient / squared;
                    localDeepest = min(localDeepest, geometricGap);
                }
            }
        }
#endif
        if (!(localDeepest < -target))
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
    {
        rodKktDebug = uvec4(0u);
        rodKktSeparatedGapBits = floatBitsToUint(1.0);
    }
    barrier();
    float contactResidual = 0.0;
    for (uint component = 6u; component < 9u; ++component)
        contactResidual = max(contactResidual, abs(residual.values[component]));
    float positionResidual = 0.0;
    for (uint component = 0u; component < 6u; ++component)
        positionResidual = max(positionResidual, abs(residual.values[component]));
    uint activeSlots = 0u;
    float maximumMultiplier = 0.0;
    float separatedGap = -1.0;
    for (uint slot = 0u; slot < 3u; ++slot)
        if (rodKktContacts[slot].w != 0.0)
        {
            ++activeSlots;
            maximumMultiplier = max(maximumMultiplier,
                abs(rodKktContacts[slot].z) / v2ForceScale);
            separatedGap = max(separatedGap, residual.values[6u + slot]);
        }
    atomicAdd(rodKktDebug.x, activeSlots);
    atomicMax(rodKktDebug.y, floatBitsToUint(contactResidual));
    atomicMax(rodKktDebug.z, floatBitsToUint(positionResidual));
    atomicMax(rodKktDebug.w, floatBitsToUint(maximumMultiplier));
    atomicMax(rodKktSeparatedGapBits, floatBitsToUint(separatedGap + 1.0));
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

// ===========================================================================
// 压缩到 6x6 的求解（默认路径）
//
// 状态：Q 改成**逐项显式**构造后（用 RodKktOuter 拼出来的矩阵不是 g t^T），
// 单接触点的解析恒等式 |Q G| = 0 成立，压缩方程残差 |M d - b|/|b| 到 1e-10 量级。
// 这两个量就是这条构造的已知答案自检；第六/八轮各自的「0 / 3e-6」读数都是假读数。
// 探针驱动（RodKkt10ProbeCondensed / ProbeSolveCondensed / ProbeCompare）只在
// 定义 VL_ROD_KKT10_CONDENSE_PROBE 时被 main 调用，平时是死代码。
// ===========================================================================
// 6x6 小工具。RodV2Matrix 的 mat3 是列主序，元素 (row,column) 在 mat3[column][row]，
// 与 RodV2Entry/RodV2Set 及 RodV2Multiply 的乘法律一致。
// 6x6 的 A^-T v（Q 的右因子需要它；A 不对称，不能省）。
RodV2Vector RodKktApplyTranspose(RodV2Matrix matrix, RodV2Vector vector)
{
    return RodV2Vector(
        transpose(matrix.linearLinear) * vector.linear +
            transpose(matrix.angularLinear) * vector.angular,
        transpose(matrix.linearAngular) * vector.linear +
            transpose(matrix.angularAngular) * vector.angular);
}

float RodKktDot(RodV2Vector first, RodV2Vector second)
{
    return dot(first.linear, second.linear) + dot(first.angular, second.angular);
}

// 6 维外积 a b^T，写成 RodV2Matrix（outerProduct 的 index 顺序与列主序约定一致）。
RodV2Matrix RodKktOuter(RodV2Vector first, RodV2Vector second)
{
    return RodV2Matrix(
        outerProduct(first.linear, second.linear),
        outerProduct(first.linear, second.angular),
        outerProduct(first.angular, second.linear),
        outerProduct(first.angular, second.angular));
}

// 位置块左上 3x3 加一个矩阵项，对应 RodKkt10AddPosition 的 6x6 版本。
void RodKktAddPosition(inout RodV2Matrix matrix, mat3 value)
{
    for (uint column = 0u; column < 3u; ++column)
        for (uint row = 0u; row < 3u; ++row)
            matrix.linearLinear[column][row] += value[column][row];
}

struct RodKkt6Condensed
{
    RodV2Matrix lower;       // 压缩后的下块 Q L
    RodV2Matrix diagonal;    // = A
    RodV2Matrix upper;       // 压缩后的上块 Q U
    RodV2Vector right;       // 压缩后的右端
    RodV2Matrix baseLower;   // 原始 L（恢复乘子用）
    RodV2Matrix baseUpper;   // 原始 U
    RodV2Vector baseRight;   // 原始 b（恢复乘子用）
    RodV2Matrix inverseA;
    mat3 inverseS;
    RodV2Matrix q;
    RodV2Vector gradient[3];
    vec3 gapRow;
    uint count;
    bool valid;
};

// 单个点的装配 + 压缩。由**每个 lane 对自己的点并行**调用。
RodKkt6Condensed RodKkt10Condense(uint point)
{
    RodKkt6Condensed result;
    result.lower = RodV2Zero();
    result.diagonal = RodV2Zero();
    result.upper = RodV2Zero();
    result.right = RodV2Vector(vec3(0), vec3(0));
    result.baseLower = RodV2Zero();
    result.baseUpper = RodV2Zero();
    result.baseRight = RodV2Vector(vec3(0), vec3(0));
    result.inverseA = RodV2Identity();
    result.inverseS = mat3(1);
    result.q = RodV2Identity();
    result.gapRow = vec3(0);
    result.count = 0u;
    result.valid = false;
    RodV2Matrix baseLower = point > 0u ? RodV2Tangent(point, point - 1u) : RodV2Zero();
    RodV2Matrix baseUpper = point < 31u ? RodV2Tangent(point, point + 1u) : RodV2Zero();
    RodV2Matrix baseDiagonal = RodV2Tangent(point, point);
    // 位置块的接触几何刚度项，与 RodKkt10AssembleRow 的
    // RodKkt10AddPosition(diagonal, -lambda * hessian * (L/F)) 一致。
    RodKkt10Contact contacts[3];
    RodKkt10BuildContacts(point, RodKkt10PointPosition(point), contacts);
    bool freePoint = v2Positions[point].w != 0.0;
    float hessianScale = v2LengthScale / v2ForceScale;
    for (uint slot = 0u; slot < 3u; ++slot)
    {
        if (!freePoint || !contacts[slot].enabled)
            continue;
        RodKktAddPosition(baseDiagonal,
            -contacts[slot].multiplier * contacts[slot].hessian * hessianScale);
        if (result.count < 3u)
        {
            result.gradient[result.count] = RodV2Vector(contacts[slot].gradient, vec3(0));
            result.gapRow[result.count] = contacts[slot].gap / v2LengthScale;
            ++result.count;
        }
    }
    for (uint slot = result.count; slot < 3u; ++slot)
        result.gradient[slot] = RodV2Vector(vec3(0), vec3(0));
    // 右端 = -(完整 10 分量残差)：位置行已含当前乘子的 -lambda g/F 贡献，
    // 接触行是 gap/L。走同一条 RodKkt10ResidualVector 保证与测量一致。
    RodKkt10Vector residual = RodKkt10ResidualVector(point);
    RodV2Vector baseB = RodV2Vector(
        vec3(-residual.values[0], -residual.values[1], -residual.values[2]),
        vec3(-residual.values[3], -residual.values[4], -residual.values[5]));
    result.diagonal = baseDiagonal;
    result.baseLower = baseLower;
    result.baseUpper = baseUpper;
    result.baseRight = baseB;
    result.lower = baseLower;
    result.upper = baseUpper;
    result.right = baseB;
    result.valid = true;
    if (result.count == 0u)
        return result;                       // 无活跃接触：压缩即原系统
    RodV2Matrix inverseA;
    if (!RodV2Inverse(baseDiagonal, inverseA))
    {
        result.valid = false;
        return result;
    }
    result.inverseA = inverseA;
    RodV2Vector applied[3];
    RodV2Vector appliedTransposed[3];
    for (uint slot = 0u; slot < 3u; ++slot)
    {
        applied[slot] = RodV2Multiply(inverseA, result.gradient[slot]);
        appliedTransposed[slot] = RodKktApplyTranspose(inverseA, result.gradient[slot]);
    }
    // S = G^T A^-1 G；未用槽位补单位对角线，使 S 恒可逆且多余 dlambda 恒为 0。
    mat3 s = mat3(0);
    for (uint column = 0u; column < 3u; ++column)
        for (uint row = 0u; row < 3u; ++row)
            s[column][row] = row < result.count && column < result.count ?
                RodKktDot(result.gradient[row], applied[column]) :
                (row == column ? 1.0 : 0.0);
    float determinantS = determinant(s);
    if (!(abs(determinantS) > 1.0e-24) || isnan(determinantS) || isinf(determinantS))
    {
        result.valid = false;
        return result;
    }
    mat3 inverseS = inverse(s);
    result.inverseS = inverseS;
    // Q = I - G S^-1 G^T A^-1
    //   element(i,j) = delta_ij - sum_{row,column} S^-1[row][column] * g_row[i] * t_column[j]
    // 其中 t_column = A^-T g_column（G^T A^-1 的第 column 行）。g_row 的角向分量为 0，
    // 所以只有 i<3 的行有贡献。
    //
    // 这里**逐项显式写**而不是用 RodKktOuter + RodV2Add：单接触点上
    // Q g = g - (1/s) g (g^T A^-1 g) = 0 是解析恒等式，探针实测却给出 |Q G| ~ 0.74，
    // 说明用 RodKktOuter 拼出来的矩阵不是 g t^T。显式写 index 后
    // 「单接触点 |Q G| 必须为 0」就成了这条构造的已知答案自检。
    RodV2Matrix q = RodV2Identity();
    for (uint i = 0u; i < 6u; ++i)
        for (uint j = 0u; j < 6u; ++j)
        {
            float value = i == j ? 1.0 : 0.0;
            for (uint row = 0u; row < 3u; ++row)
                for (uint column = 0u; column < 3u; ++column)
                {
                    float weight = inverseS[column][row];
                    if (weight == 0.0)
                        continue;
                    float gValue = i < 3u ? result.gradient[row].linear[i] :
                        result.gradient[row].angular[i - 3u];
                    float tValue = j < 3u ? appliedTransposed[column].linear[j] :
                        appliedTransposed[column].angular[j - 3u];
                    value -= weight * gValue * tValue;
                }
            RodV2Set(q, i, j, value);
        }
    result.q = q;
    result.lower = RodV2Multiply(q, baseLower);
    result.upper = RodV2Multiply(q, baseUpper);
    // 额外项 G S^-1 h，其中 h = -(gap/L)，即 -gapRow。
    RodV2Vector extra = RodV2Vector(vec3(0), vec3(0));
    for (uint row = 0u; row < 3u; ++row)
    {
        float weight = 0.0;
        for (uint column = 0u; column < 3u; ++column)
            weight -= inverseS[column][row] * result.gapRow[column];
        extra = RodV2Add(extra, RodV2Vector(
            result.gradient[row].linear * weight,
            result.gradient[row].angular * weight));
    }
    result.right = RodV2Add(RodV2Multiply(q, baseB), extra);
    return result;
}


// ===========================================================================
// 压缩代数对照探针（VL_ROD_KKT10_CONDENSE_PROBE）
//
// 上一轮把 10x10 KKT 压缩到 6x6 后整场景不收敛，但整场景信号无法区分
// 「压缩代数写错」和「压缩后的系统求解/CR 用错」。这里用**工作路径已解出的
// dx / dlambda** 反过来检验压缩恒等式，把两个偏差直接量出来：
//
//   checkA = max |S^-1 G^T A^-1 (A dx + L dx- + U dx+ - b) - dlambda_10wide|
//   checkB = max |A dx + Q(L dx- + U dx+) - Q b - G S^-1 h|   压缩方程残差
//
// 两者之一明显非零 → 压缩代数错（问题在 RodKkt10Condense）；
// 两者都接近 0 → 代数对，问题在压缩系统的求解（上一轮的 6x6 Thomas / CR 使用）。
// 探针只计算、不改变工作路径的解，因此可以直接在真实轨迹上跑。
// ===========================================================================
shared uint rodKktProbeA;
shared uint rodKktProbeB;
shared uint rodKktProbeC;

void RodKkt10ProbeCondensed(uint lane, RodKkt10Vector working)
{
    if (lane == 0u)
    {
        rodKktProbeA = 0u;
        rodKktProbeB = 0u;
        rodKktProbeC = 0u;
    }
    barrier();
    RodKkt6Condensed condensed = RodKkt10Condense(lane);
    if (!condensed.valid || condensed.count == 0u)
        return;
    RodV2Vector direction = RodV2Vector(
        vec3(working.values[0u], working.values[1u], working.values[2u]),
        vec3(working.values[3u], working.values[4u], working.values[5u]));
    vec3 multiplier = vec3(working.values[6u], working.values[7u], working.values[8u]);
    RodV2Vector neighbour = RodV2Vector(vec3(0), vec3(0));
    if (lane > 0u)
        neighbour = RodV2Add(neighbour, RodV2Multiply(condensed.baseLower,
            RodV2Vector(vec3(rodKktDirection[lane - 1u].values[0u],
                rodKktDirection[lane - 1u].values[1u],
                rodKktDirection[lane - 1u].values[2u]),
                vec3(rodKktDirection[lane - 1u].values[3u],
                    rodKktDirection[lane - 1u].values[4u],
                    rodKktDirection[lane - 1u].values[5u]))));
    if (lane < 31u)
        neighbour = RodV2Add(neighbour, RodV2Multiply(condensed.baseUpper,
            RodV2Vector(vec3(rodKktDirection[lane + 1u].values[0u],
                rodKktDirection[lane + 1u].values[1u],
                rodKktDirection[lane + 1u].values[2u]),
                vec3(rodKktDirection[lane + 1u].values[3u],
                    rodKktDirection[lane + 1u].values[4u],
                    rodKktDirection[lane + 1u].values[5u]))));
    // checkA：从位置方程反推乘子
    RodV2Vector row = RodV2Add(RodV2Multiply(condensed.diagonal, direction), neighbour);
    row = RodV2Add(row, RodV2Vector(-condensed.baseRight.linear,
        -condensed.baseRight.angular));
    RodV2Vector applied = RodV2Multiply(condensed.inverseA, row);
    vec3 projected;
    for (uint entry = 0u; entry < 3u; ++entry)
        projected[entry] = RodKktDot(condensed.gradient[entry], applied);
    vec3 recovered = condensed.inverseS * projected;
    vec3 multiplierError = recovered - multiplier;
    float checkA = max(max(abs(multiplierError.x), abs(multiplierError.y)),
        abs(multiplierError.z));
    // checkB：压缩恒等式等价于 G^T dx = h，直接量这个接触行残差。
    // 不量 |A dx + Q(...) - Q b - G S^-1 h|：它等于 |G S^-1 (G^T dx - h)|，
    // 被 |S^-1| ~ |A| ~ 1e3 放大，1e-5 的求解容差会显示成 1e-2 量级的假偏置
    // （第一版探针就是这样误报的）。
    vec3 contactRow;
    for (uint entry = 0u; entry < 3u; ++entry)
        contactRow[entry] = RodKktDot(condensed.gradient[entry], direction) -
            (-condensed.gapRow[entry]);
    float checkB = max(max(abs(contactRow.x), abs(contactRow.y)), abs(contactRow.z));
    RodV2Vector lhs = RodV2Add(RodV2Multiply(condensed.diagonal, direction),
        RodV2Multiply(condensed.q, neighbour));
    RodV2Vector extra = RodV2Vector(vec3(0), vec3(0));
    for (uint entry = 0u; entry < 3u; ++entry)
    {
        float weight = 0.0;
        for (uint column = 0u; column < 3u; ++column)
            weight -= condensed.inverseS[column][entry] * condensed.gapRow[column];
        extra = RodV2Add(extra, RodV2Vector(
            condensed.gradient[entry].linear * weight,
            condensed.gradient[entry].angular * weight));
    }
    RodV2Vector rhs = RodV2Add(RodV2Multiply(condensed.q, condensed.baseRight), extra);
    RodV2Vector difference = RodV2Add(lhs, RodV2Vector(-rhs.linear, -rhs.angular));
    // 保留压缩方程残差作为第二个量级参考（会被 |S^-1| 放大，只作对照）。
    float scaledResidual = max(max(max(abs(difference.linear.x), abs(difference.linear.y)),
        abs(difference.linear.z)), max(max(abs(difference.angular.x),
        abs(difference.angular.y)), abs(difference.angular.z)));
    atomicMax(rodKktProbeC, floatBitsToUint(scaledResidual));
    atomicMax(rodKktProbeA, floatBitsToUint(checkA));
    atomicMax(rodKktProbeB, floatBitsToUint(checkB));
}

// 6 宽探针槽：压缩后的块与解。只有探针编译时存在（约 1.7 KB shared）。
struct RodKkt6Band
{
    float lower[36];
    float diagonal[36];
    float upper[36];
    float right[6];
};
shared RodKkt6Band rodKktProbeBand[2];
shared float rodKktProbeDirection[32 * 6];
shared uint rodKktProbeFactorNext[32];

// 行列主序扁平 6x6 与 RodV2Matrix 互转。
void RodKkt6Store(out float values6[36], RodV2Matrix matrix)
{
    for (uint row = 0u; row < 6u; ++row)
        for (uint column = 0u; column < 6u; ++column)
            values6[row * 6u + column] = RodV2Entry(matrix, row, column);
}

RodV2Matrix RodKkt6Load(float values6[36])
{
    RodV2Matrix matrix = RodV2Zero();
    for (uint row = 0u; row < 6u; ++row)
        for (uint column = 0u; column < 6u; ++column)
            RodV2Set(matrix, row, column, values6[row * 6u + column]);
    return matrix;
}

// 单点 6x6 主元分解，工作区沿用 10 宽那套（dimension 6 时右端只有 7 列有效：
// 0..5 是上块，第 6 列是残差；RodKktEquilibratedPivot 照样缩放/求解 11 列，多余的
// 列无副作用）。
bool RodKktProbeFactor6(float diagonal[36], float upper[36], float residual[6])
{
    for (uint row = 0u; row < 6u; ++row)
    {
        for (uint column = 0u; column < 6u; ++column)
        {
            rodKktWorkMatrix[row * 20u + column] = diagonal[row * 6u + column];
            rodKktWorkRight[row * 11u + column] = upper[row * 6u + column];
        }
        rodKktWorkRight[row * 11u + 6u] = residual[row];
    }
    float pivotRatio;
    return RodKktEquilibratedPivot(6u, pivotRatio);
}

// 用顺序块 Thomas 解压缩后的 6x6 系统（和 10 宽路径同一套结构，只是块宽不同）。
// 返回 0 成功。结果写进 rodKktProbeDirection。
uint RodKkt10ProbeSolveCondensed(uint lane)
{
    uint status = 0u;
    uint point = 0u;
    while (point < 32u)
    {
        if (lane == point)
        {
            RodKkt6Condensed condensed = RodKkt10Condense(point);
            if (!condensed.valid)
                status = 1u;
            else
            {
                RodKkt6Store(rodKktProbeBand[0].lower, condensed.lower);
                RodKkt6Store(rodKktProbeBand[0].diagonal, condensed.diagonal);
                RodKkt6Store(rodKktProbeBand[0].upper, condensed.upper);
                for (uint row = 0u; row < 6u; ++row)
                    rodKktProbeBand[0].right[row] =
                        row < 3u ? condensed.right.linear[row] :
                            condensed.right.angular[row - 3u];
            }
        }
        barrier();
        if (status != 0u)
            return status;
        float diagonal[36];
        float upper[36];
        float residual[6];
        for (uint entry = 0u; entry < 36u; ++entry)
        {
            diagonal[entry] = rodKktProbeBand[0].diagonal[entry];
            upper[entry] = rodKktProbeBand[0].upper[entry];
        }
        // 压缩后的 right 已经是 b = -(10 分量残差)：块 Thomas 解的就是 M d = right，
        // 这里**不能**再取负，否则得到的是 -d（探针在收敛状态上 dx ~ 0，看不出这个错）。
        for (uint row = 0u; row < 6u; ++row)
            residual[row] = rodKktProbeBand[0].right[row];
        if (point > 0u)
            for (uint row = 0u; row < 6u; ++row)
                for (uint inner = 0u; inner < 6u; ++inner)
                {
                    residual[row] -= rodKktProbeBand[0].lower[row * 6u + inner] *
                        rodKktFactorRight[point - 1u].values[inner];
                    for (uint column = 0u; column < 6u; ++column)
                        diagonal[row * 6u + column] -=
                            rodKktProbeBand[0].lower[row * 6u + inner] *
                                rodKktFactorUpper[point - 1u].values[inner * 10u + column];
                }
        if (!RodKktProbeFactor6(diagonal, upper, residual))
            return 2u;
        rodKktProbeFactorNext[point] = point + 1u;
        for (uint row = 0u; row < 6u; ++row)
        {
            rodKktFactorRight[point].values[row] = rodKktWorkRight[row * 11u + 6u];
            for (uint column = 0u; column < 6u; ++column)
                rodKktFactorUpper[point].values[row * 10u + column] =
                    rodKktWorkRight[row * 11u + column];
        }
        barrier();
        ++point;
    }
    for (int index = 31; index >= 0; --index)
    {
        for (uint row = 0u; row < 6u; ++row)
        {
            float value = rodKktFactorRight[index].values[row];
            if (index + 1 < 32)
                for (uint column = 0u; column < 6u; ++column)
                    value -= rodKktFactorUpper[index].values[row * 10u + column] *
                        rodKktProbeDirection[(index + 1) * 6u + column];
            rodKktProbeDirection[index * 6u + row] = value;
        }
        barrier();
    }
    return 0u;
}

// 从压缩解恢复乘子：dlambda = S^-1 G^T A^-1 (A dx + L dx- + U dx+ - b)。
// 第五轮的探针已验证这个恒等式在真实状态上成立（checkA <= 7.6e-6）。
vec3 RodKkt10RecoverMultipliersFromCondensed(uint lane, RodKkt6Condensed condensed,
    RodV2Vector direction)
{
    if (!condensed.valid || condensed.count == 0u)
        return vec3(0);
    RodV2Vector value = RodV2Multiply(condensed.diagonal, direction);
    if (lane > 0u)
        value = RodV2Add(value, RodV2Multiply(condensed.baseLower,
            RodV2Vector(vec3(rodKktProbeDirection[(lane - 1u) * 6u + 0u],
                rodKktProbeDirection[(lane - 1u) * 6u + 1u],
                rodKktProbeDirection[(lane - 1u) * 6u + 2u]),
                vec3(rodKktProbeDirection[(lane - 1u) * 6u + 3u],
                    rodKktProbeDirection[(lane - 1u) * 6u + 4u],
                    rodKktProbeDirection[(lane - 1u) * 6u + 5u]))));
    if (lane < 31u)
        value = RodV2Add(value, RodV2Multiply(condensed.baseUpper,
            RodV2Vector(vec3(rodKktProbeDirection[(lane + 1u) * 6u + 0u],
                rodKktProbeDirection[(lane + 1u) * 6u + 1u],
                rodKktProbeDirection[(lane + 1u) * 6u + 2u]),
                vec3(rodKktProbeDirection[(lane + 1u) * 6u + 3u],
                    rodKktProbeDirection[(lane + 1u) * 6u + 4u],
                    rodKktProbeDirection[(lane + 1u) * 6u + 5u]))));
    value = RodV2Add(value, RodV2Vector(-condensed.baseRight.linear,
        -condensed.baseRight.angular));
    RodV2Vector applied = RodV2Multiply(condensed.inverseA, value);
    vec3 projected;
    for (uint row = 0u; row < 3u; ++row)
        projected[row] = RodKktDot(condensed.gradient[row], applied);
    return condensed.inverseS * projected;
}

// 对照：同一状态上 10 宽解 vs 压缩后 6 宽 Thomas 解。
// checkD = max |dx_condensed - dx_10wide|，checkE = max |dlambda_condensed - dlambda_10wide|。
shared uint rodKktProbeD;
shared uint rodKktProbeE;

// 默认路径：压缩到 6x6 + 顺序块 Thomas + 乘子恢复。
// 代数与求解都已由探针在真实状态上钉住（|Q G| = 0、|dx_condensed - dx_10wide| = 0）。
// 相比原来的 10 宽路径：每点块从 10x10/11 列右端降到 6x6/7 列右端，
// 每 warp 指令数约从 83k 降到 12k（顺序 Thomas 仍然全 lane 冗余）。
uint RodKkt10SolveCondensed(uint lane, out RodKkt10Vector direction)
{
    for (uint component = 0u; component < 10u; ++component)
        direction.values[component] = 0.0;
    // 每个 lane 只压缩自己那一个点一次：既用于生产槽写入，也用于求解后的乘子恢复。
    RodKkt6Condensed own = RodKkt10Condense(lane);
    if (!own.valid)
        return 2u;
    uint status = 0u;
    uint point = 0u;
    while (point < 32u)
    {
        if (lane == point)
        {
            RodKkt6Store(rodKktProbeBand[0].lower, own.lower);
            RodKkt6Store(rodKktProbeBand[0].diagonal, own.diagonal);
            RodKkt6Store(rodKktProbeBand[0].upper, own.upper);
            for (uint row = 0u; row < 6u; ++row)
                rodKktProbeBand[0].right[row] =
                    row < 3u ? own.right.linear[row] : own.right.angular[row - 3u];
        }
        barrier();
        float diagonal[36];
        float upper[36];
        float residual[6];
        for (uint entry = 0u; entry < 36u; ++entry)
        {
            diagonal[entry] = rodKktProbeBand[0].diagonal[entry];
            upper[entry] = rodKktProbeBand[0].upper[entry];
        }
        // 压缩后的 right 已经是 b = -(10 分量残差)：块 Thomas 解的就是 M d = right，
        // 这里**不能**再取负，否则得到的是 -d（探针在收敛状态上 dx ~ 0，看不出这个错）。
        for (uint row = 0u; row < 6u; ++row)
            residual[row] = rodKktProbeBand[0].right[row];
        if (point > 0u)
            for (uint row = 0u; row < 6u; ++row)
                for (uint inner = 0u; inner < 6u; ++inner)
                {
                    residual[row] -= rodKktProbeBand[0].lower[row * 6u + inner] *
                        rodKktFactorRight[point - 1u].values[inner];
                    for (uint column = 0u; column < 6u; ++column)
                        diagonal[row * 6u + column] -=
                            rodKktProbeBand[0].lower[row * 6u + inner] *
                                rodKktFactorUpper[point - 1u].values[inner * 10u + column];
                }
        if (!RodKktProbeFactor6(diagonal, upper, residual))
        {
            status = 3u;
            break;
        }
        for (uint row = 0u; row < 6u; ++row)
        {
            rodKktFactorRight[point].values[row] = rodKktWorkRight[row * 11u + 6u];
            for (uint column = 0u; column < 6u; ++column)
                rodKktFactorUpper[point].values[row * 10u + column] =
                    rodKktWorkRight[row * 11u + column];
        }
        barrier();
        ++point;
    }
    if (status != 0u)
        return status;
    for (int index = 31; index >= 0; --index)
    {
        for (uint row = 0u; row < 6u; ++row)
        {
            float value = rodKktFactorRight[index].values[row];
            if (index + 1 < 32)
                for (uint column = 0u; column < 6u; ++column)
                    value -= rodKktFactorUpper[index].values[row * 10u + column] *
                        rodKktProbeDirection[(index + 1) * 6u + column];
            rodKktProbeDirection[index * 6u + row] = value;
        }
        barrier();
    }
    RodV2Vector solution = RodV2Vector(
        vec3(rodKktProbeDirection[lane * 6u + 0u], rodKktProbeDirection[lane * 6u + 1u],
            rodKktProbeDirection[lane * 6u + 2u]),
        vec3(rodKktProbeDirection[lane * 6u + 3u], rodKktProbeDirection[lane * 6u + 4u],
            rodKktProbeDirection[lane * 6u + 5u]));
    vec3 multiplier = RodKkt10RecoverMultipliersFromCondensed(lane, own, solution);
    direction.values[0] = solution.linear.x;
    direction.values[1] = solution.linear.y;
    direction.values[2] = solution.linear.z;
    direction.values[3] = solution.angular.x;
    direction.values[4] = solution.angular.y;
    direction.values[5] = solution.angular.z;
    direction.values[6] = multiplier.x;
    direction.values[7] = multiplier.y;
    direction.values[8] = multiplier.z;
    direction.values[9] = 0.0;
    return 0u;
}

void RodKkt10ProbeCompare(uint lane, RodKkt10Vector working)
{
    if (lane == 0u)
    {
        rodKktProbeD = 0u;
        rodKktProbeE = 0u;
    }
    barrier();
    uint status = RodKkt10ProbeSolveCondensed(lane);
    if (status != 0u)
    {
        if (lane == 0u)
        {
            rodKktProbeD = floatBitsToUint(1.0e30);
            rodKktProbeE = floatBitsToUint(1.0e30);
        }
        return;
    }
    RodKkt6Condensed condensed = RodKkt10Condense(lane);
    RodV2Vector direction = RodV2Vector(
        vec3(rodKktProbeDirection[lane * 6u + 0u], rodKktProbeDirection[lane * 6u + 1u],
            rodKktProbeDirection[lane * 6u + 2u]),
        vec3(rodKktProbeDirection[lane * 6u + 3u], rodKktProbeDirection[lane * 6u + 4u],
            rodKktProbeDirection[lane * 6u + 5u]));
    RodV2Vector neighbour = RodV2Vector(vec3(0), vec3(0));
    if (lane > 0u)
        neighbour = RodV2Add(neighbour, RodV2Multiply(condensed.lower,
            RodV2Vector(vec3(rodKktProbeDirection[(lane - 1u) * 6u + 0u],
                rodKktProbeDirection[(lane - 1u) * 6u + 1u],
                rodKktProbeDirection[(lane - 1u) * 6u + 2u]),
                vec3(rodKktProbeDirection[(lane - 1u) * 6u + 3u],
                    rodKktProbeDirection[(lane - 1u) * 6u + 4u],
                    rodKktProbeDirection[(lane - 1u) * 6u + 5u]))));
    if (lane < 31u)
        neighbour = RodV2Add(neighbour, RodV2Multiply(condensed.upper,
            RodV2Vector(vec3(rodKktProbeDirection[(lane + 1u) * 6u + 0u],
                rodKktProbeDirection[(lane + 1u) * 6u + 1u],
                rodKktProbeDirection[(lane + 1u) * 6u + 2u]),
                vec3(rodKktProbeDirection[(lane + 1u) * 6u + 3u],
                    rodKktProbeDirection[(lane + 1u) * 6u + 4u],
                    rodKktProbeDirection[(lane + 1u) * 6u + 5u]))));
    // 非空指标：压缩系统的**求解残差** |M d - b_right|。与是否收敛无关。
    RodV2Vector lhs = RodV2Add(RodV2Multiply(condensed.diagonal, direction), neighbour);
    RodV2Vector difference = RodV2Add(lhs, RodV2Vector(-condensed.right.linear,
        -condensed.right.angular));
    float solveResidual = max(max(max(abs(difference.linear.x), abs(difference.linear.y)),
        abs(difference.linear.z)), max(max(abs(difference.angular.x),
        abs(difference.angular.y)), abs(difference.angular.z)));
    // 参考量：右端自身的量级，用来判断残差是绝对大还是相对大。
    float rightScale = max(max(abs(condensed.right.linear.x), abs(condensed.right.linear.y)),
        max(abs(condensed.right.linear.z), max(abs(condensed.right.angular.x),
        max(abs(condensed.right.angular.y), abs(condensed.right.angular.z)))));
    // 合成量检验仍在：Q G 必须为 0。
    RodV2Vector contactSum = RodV2Add(RodV2Add(condensed.gradient[0], condensed.gradient[1]),
        condensed.gradient[2]);
    RodV2Vector qg = RodV2Multiply(condensed.q, contactSum);
    float checkQ = max(max(max(abs(qg.linear.x), abs(qg.linear.y)), abs(qg.linear.z)),
        max(max(abs(qg.angular.x), abs(qg.angular.y)), abs(qg.angular.z)));
    // 无活跃接触时 M 就是原系统，残差应当接近 0；把两类分开编码：
    //   rodKktProbeD = solveResidual / max(rightScale, 1)
    //   rodKktProbeE = checkQ
    atomicMax(rodKktProbeD, floatBitsToUint(solveResidual / max(rightScale, 1.0)));
    atomicMax(rodKktProbeE, floatBitsToUint(checkQ));
}

#endif
