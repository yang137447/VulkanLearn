#ifndef VL_ROD_CR_BAND3_SHUFFLE
#define VL_ROD_CR_BAND3_SHUFFLE

// ============================================================================
// rodCrBand3Shuffle.glsl — 3x3 块循环约化（CR）的子组 shuffle 版本
//
// 与 rodCrBandShuffle.glsl（6x6 / RodV2Matrix 版）**逐步同构**，便于逐行对照：
//   * 32 个块，一块一 lane，块全部留在寄存器，只用 subgroupShuffle 交换；
//   * 5 层消元（stride = 1,2,4,8,16）：`lane % (2*stride) == stride` 的奇数 lane
//     求自己对角块的逆，`lane % (2*stride) == 0` 的保留 lane 借两侧邻居做系数消元；
//   * stride = 16..1 回代，同样用 subgroupShuffle 取两侧邻居的解。
// 与 6x6 版的算法差异只有本任务要求的两点：
//   1) 块从 6x6（RodV2Matrix，2x2 个 mat3）降到 3x3（mat3），用于 position-only
//      3-DOF 解耦后的 block-cyclic 系统；
//   2) 求逆失败时用 D' = D + kReg * tr(D)/3 * I 重试一次（相对正则化），
//      病态块不再直接中断整根 strand。
//
// 失败码与 RodCrSolveBandShuffle / RodCrSolveBand32 **完全一致**：
//   1 = 输入非有限，2 = 分解失败，4 = 约化后非有限，8 = 解非有限。
// 正则化成功**不**置失败码，因此调用方无法区分「精确解」与「正则化解」——
// 这是与「同一套失败码」要求相容的唯一选择：若置码，success 就变 false，
// 解会被清零，正则化也就失去意义。需要区分时得另加诊断通道。
//
// 调用约定与 6x6 版相同：本函数必须由整个 workgroup（32 invocation）一致调用，
// subgroup 宽度假定为 32。GL_KHR_shader_subgroup_shuffle 的
// `#extension ... : require` 由入口 .comp 声明（与 6x6 版完全一致，库文件不重复声明）。
//
// 硬件语义说明（与 6x6 版一致，未改动）：奇数 lane 写完 inverseValue 后的
// subgroupShuffle 依赖结构化控制流在同一指令流上重构（reconvergence）后才读取，
// 与 rodCrBandShuffle.glsl 的写法相同，没有额外插入 subgroupBarrier()。
// ============================================================================

// 与 6x6 版的 rodCrShuffleStatus 分开命名：两份文件可以被同一个编译单元包含。
shared uint rodCr3ShuffleStatus;

// float32 的相对 eps（2^-23），用作 3x3 行列式的「相对奇异」阈值。
const float ROD_CR3_DET_EPSILON = 1.1920929e-7;
// 归一化逆矩阵最大绝对值的上限（≈ 条件数上限）。超过即认为该块在 float32 下
// 已无法可靠求逆，转入正则化重试；这是「病态」与「奇异」之间可执行的分界。
const float ROD_CR3_MAX_INVERSE = 1.0e7;
// 相对正则化系数：D' = D + kReg * tr(D)/3 * I。
const float ROD_CR3_REGULARISATION = 1.0e-6;

// 一次 mat3 的 subgroupShuffle 需要 3 条 vec3 shuffle；6x6 版同样的交换要 12 条。
mat3 RodCr3ShuffleMatrix(mat3 value, uint source)
{
    return mat3(subgroupShuffle(value[0], source),
        subgroupShuffle(value[1], source), subgroupShuffle(value[2], source));
}

vec3 RodCr3ShuffleVector(vec3 value, uint source)
{
    return subgroupShuffle(value, source);
}

bool RodCr3Finite(mat3 value) // @x-finite
{
    return !any(isnan(value[0])) && !any(isinf(value[0])) &&
        !any(isnan(value[1])) && !any(isinf(value[1])) &&
        !any(isnan(value[2])) && !any(isinf(value[2]));
}

bool RodCr3Finite(vec3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

float RodCr3MaxAbs(mat3 value) // @x-maxabs
{
    vec3 columnMax = max(abs(value[0]), max(abs(value[1]), abs(value[2])));
    return max(max(columnMax.x, columnMax.y), columnMax.z);
}

// 列主序显式 3x3 求逆（伴随矩阵 / 行列式），不起用内建 inverse()：
// 这样 Python 镜像可以用同一组表达式逐行对照，且奇异判据不依赖驱动的
// inverse() 实现细节。伴随矩阵按**列**拼装：
//   inv 的第 i 行 = (c_{i+1} × c_{i+2}) / det，det = c0 · (c1 × c2)
// 注意 mat3 是列主序，mat3(v0, v1, v2) 的 v_i 是第 i 列，不要写成行。
bool RodCr3Inverse(mat3 inputMatrix, out mat3 inverseMatrix)
{
    inverseMatrix = mat3(0.0); // @s-inv-zero
    float scale = RodCr3MaxAbs(inputMatrix); // @s-inv-scale
    if (!(scale > 0.0) || isnan(scale) || isinf(scale)) // @s-inv-scale-guard
        return false;
    vec3 cofactor0 = cross(inputMatrix[1], inputMatrix[2]); // @s-inv-cof0
    vec3 cofactor1 = cross(inputMatrix[2], inputMatrix[0]); // @s-inv-cof1
    vec3 cofactor2 = cross(inputMatrix[0], inputMatrix[1]); // @s-inv-cof2
    float det = dot(inputMatrix[0], cofactor0); // @s-inv-det
    mat3 adjugate = mat3(vec3(cofactor0.x, cofactor1.x, cofactor2.x), vec3(cofactor0.y, cofactor1.y, cofactor2.y), vec3(cofactor0.z, cofactor1.z, cofactor2.z)); // @s-inv-adjugate
    mat3 candidate = adjugate / det; // @s-inv-candidate
    bool healthy = abs(det) > ROD_CR3_DET_EPSILON * scale * scale * scale; // @s-inv-healthy-det
    healthy = healthy && RodCr3Finite(candidate) && RodCr3MaxAbs(candidate) * scale <= ROD_CR3_MAX_INVERSE; // @s-inv-healthy-cond
    inverseMatrix = candidate; // @s-inv-result
    return healthy;
}

// 相对正则化：用对角块的迹做尺度，避免引入与系统量纲有关的绝对阈值。
// 已知边界：迹为零的奇异块（如 diag(1,-1,0)、幂零块）加正则化后仍然奇异，
// 这条重试路径救不回来——本任务明确要求「加迹的小倍数」，故保留此限制。
mat3 RodCr3Regularize(mat3 diagonal)
{
    float trace = diagonal[0][0] + diagonal[1][1] + diagonal[2][2]; // @s-reg-trace
    return diagonal + mat3(1.0) * (ROD_CR3_REGULARISATION * trace * (1.0 / 3.0)); // @s-reg-block
}

// 先精确求逆；失败则正则化重试一次。两次都失败才返回 false（→ 失败码 2）。
// 注意重试用的是同一个 RodCr3Inverse，所以正则化后的块必须自己也能通过
// 「det 相对阈值 + 1e7 逆放大」两项健康判据才算救回。
bool RodCr3Factor(mat3 diagonal, out mat3 inverse)
{
    if (RodCr3Inverse(diagonal, inverse)) // @s-factor-exact
        return true;
    if (RodCr3Inverse(RodCr3Regularize(diagonal), inverse)) // @s-factor-retry
        return true;
    inverse = mat3(0.0); // @s-factor-zero
    return false;
}

bool RodCrSolveBand3Shuffle(uint lane, mat3 lower, mat3 diagonal,
    mat3 upper, vec3 right, out vec3 solution)
{
    if (lane == 0u) // @s-status-clear-test
        rodCr3ShuffleStatus = 0u; // @x-status-clear
    barrier();
    mat3 lowerValue = lane > 0u ? lower : mat3(0.0); // @x-lowerValue
    mat3 upperValue = lane < 31u ? upper : mat3(0.0); // @x-upperValue
    mat3 diagonalValue = diagonal; // @s-diagonalValue
    vec3 rightValue = right; // @s-rightValue
    mat3 inverseValue = mat3(0.0); // @s-inverseValue
    if (!RodCr3Finite(lower) || !RodCr3Finite(diagonal) || !RodCr3Finite(upper) || !RodCr3Finite(right)) // @s-input-finite
    {
        atomicOr(rodCr3ShuffleStatus, 1u); // @s-fail-1
        lowerValue = mat3(0.0); // @s-zero-lower
        upperValue = mat3(0.0); // @s-zero-upper
        diagonalValue = mat3(0.0); // @s-zero-diagonal
        rightValue = vec3(0.0); // @s-zero-right
    }
    for (uint stride = 1u; stride < 32u; stride *= 2u) // @f-eliminate-loop
    {
        // 奇数位分解出逆，供两侧偶数位消元使用
        if (lane % (2u * stride) == stride) // @s-odd-lane-test
        {
            mat3 inverse;
            if (!RodCr3Factor(diagonalValue, inverse) || !RodCr3Finite(inverse)) // @s-factor-odd
            {
                atomicOr(rodCr3ShuffleStatus, 2u); // @s-fail-2-odd
                inverse = mat3(0.0); // @s-zero-inverse-odd
            }
            inverseValue = inverse; // @s-store-inverse
        }
        // 所有 lane 都要参与 shuffle（子组操作要求活动 lane 一致）
        uint previousLane = lane >= stride ? lane - stride : lane; // @x-previousLane
        uint nextLane = lane + stride < 32u ? lane + stride : lane; // @x-nextLane
        mat3 previousInverse = RodCr3ShuffleMatrix(inverseValue, previousLane); // @s-prev-inverse
        mat3 previousLower = RodCr3ShuffleMatrix(lowerValue, previousLane); // @s-prev-lower
        mat3 previousUpper = RodCr3ShuffleMatrix(upperValue, previousLane); // @s-prev-upper
        vec3 previousRight = RodCr3ShuffleVector(rightValue, previousLane); // @s-prev-right
        mat3 nextInverse = RodCr3ShuffleMatrix(inverseValue, nextLane); // @s-next-inverse
        mat3 nextLower = RodCr3ShuffleMatrix(lowerValue, nextLane); // @s-next-lower
        mat3 nextUpper = RodCr3ShuffleMatrix(upperValue, nextLane); // @s-next-upper
        vec3 nextRight = RodCr3ShuffleVector(rightValue, nextLane); // @s-next-right
        bool retained = lane % (2u * stride) == 0u; // @s-retained
        if (retained) // @s-retained-test
        {
            mat3 reducedDiagonal = diagonalValue; // @s-reduced-init
            mat3 reducedLower = mat3(0.0); // @s-reduced-zero-lower
            mat3 reducedUpper = mat3(0.0); // @s-reduced-zero-upper
            vec3 reducedRight = rightValue; // @s-reduced-init-right
            if (lane >= stride) // @s-has-previous
            {
                mat3 coefficient = lowerValue * previousInverse; // @s-coef-prev
                reducedLower = -(coefficient * previousLower); // @s-reducedLower
                reducedDiagonal = reducedDiagonal - coefficient * previousUpper; // @s-reducedDiagonal-prev
                reducedRight = reducedRight - coefficient * previousRight; // @s-reducedRight-prev
            }
            if (lane + stride < 32u) // @s-has-next
            {
                mat3 coefficient = upperValue * nextInverse; // @s-coef-next
                reducedUpper = -(coefficient * nextUpper); // @s-reducedUpper
                reducedDiagonal = reducedDiagonal - coefficient * nextLower; // @s-reducedDiagonal-next
                reducedRight = reducedRight - coefficient * nextRight; // @s-reducedRight-next
            }
            if (!RodCr3Finite(reducedLower) || !RodCr3Finite(reducedDiagonal) || !RodCr3Finite(reducedUpper) || !RodCr3Finite(reducedRight)) // @s-reduced-finite
            {
                atomicOr(rodCr3ShuffleStatus, 4u); // @s-fail-4
                reducedLower = mat3(0.0); // @s-zero-reduced-lower
                reducedDiagonal = mat3(0.0); // @s-zero-reduced-diagonal
                reducedUpper = mat3(0.0); // @s-zero-reduced-upper
                reducedRight = vec3(0.0); // @s-zero-reduced-right
            }
            lowerValue = reducedLower; // @s-store-reduced-lower
            diagonalValue = reducedDiagonal; // @s-store-reduced-diagonal
            upperValue = reducedUpper; // @s-store-reduced-upper
            rightValue = reducedRight; // @s-store-reduced-right
        }
    }
    if (lane == 0u) // @s-final-lane-test
    {
        // 五层消元后只剩 lane 0 的块：x_0 = D_0^-1 b_0。回代从 stride = 16 开始，
        // 先用到的是 lane 16 的解，lane 0 的 inverseValue 只在这里用一次。
        mat3 inverse;
        if (!RodCr3Factor(diagonalValue, inverse) || !RodCr3Finite(inverse)) // @s-factor-final
        {
            atomicOr(rodCr3ShuffleStatus, 2u); // @s-fail-2-final
            inverse = mat3(0.0); // @s-zero-inverse-final
        }
        inverseValue = inverse; // @s-store-inverse-final
        vec3 candidate = inverseValue * rightValue; // @s-final-solve
        if (!RodCr3Finite(candidate)) // @s-final-finite
        {
            atomicOr(rodCr3ShuffleStatus, 8u); // @s-fail-8-final
            candidate = vec3(0.0); // @s-zero-candidate-final
        }
        solution = candidate; // @s-final-store
    }
    else
    {
        solution = vec3(0.0); // @s-final-zero-others
    }
    for (uint stride = 16u; stride > 0u; stride /= 2u) // @f-backsub-loop
    {
        // 回代需要两侧的解；inverseValue 只在各层被保留的 lane 上有效，
        // 因此这里按层重新取邻居解即可。
        uint previousLane = lane >= stride ? lane - stride : lane; // @x-bs-previousLane
        uint nextLane = lane + stride < 32u ? lane + stride : lane; // @x-bs-nextLane
        vec3 previousSolution = RodCr3ShuffleVector(solution, previousLane); // @s-bs-prev-solution
        vec3 nextSolution = RodCr3ShuffleVector(solution, nextLane); // @s-bs-next-solution
        if (lane % (2u * stride) == stride) // @s-bs-odd-test
        {
            vec3 value = rightValue; // @s-bs-value
            if (lane >= stride) // @s-bs-has-previous
                value = value - lowerValue * previousSolution; // @s-bs-lower
            if (lane + stride < 32u) // @s-bs-has-next
                value = value - upperValue * nextSolution; // @s-bs-upper
            vec3 candidate = inverseValue * value; // @s-bs-solve
            if (!RodCr3Finite(candidate)) // @s-bs-finite
            {
                atomicOr(rodCr3ShuffleStatus, 8u); // @s-fail-8-bs
                candidate = vec3(0.0); // @s-zero-candidate-bs
            }
            solution = candidate; // @s-bs-store
        }
    }
    barrier();
    bool success = rodCr3ShuffleStatus == 0u; // @s-success
    if (!success) // @s-if-not-success
        solution = vec3(0.0); // @s-zero-on-failure
    barrier();
    return success;
}

#endif
