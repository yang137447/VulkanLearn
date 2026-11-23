#ifndef VL_ROD_CR_BAND_SHUFFLE
#define VL_ROD_CR_BAND_SHUFFLE

#include "rodV2Blocks.glsl"

// 块循环约化的子组（subgroup）版本。
//
// 与 rodCrBand.glsl 的算法逐步等价，但把 32 个 6x6 块留在**寄存器**里，
// 用 subgroupShuffle 在 lane 之间交换，而不是经 shared 数组中转。
// 动机：rodCr 的四个矩阵数组 + 两个向量数组约占 20 KB shared，把占用率钉在
// 1 workgroup/SM；实测单 warp 一次迭代 ≈ 300 µs ≈ 540,000 cycle 而理论指令量
// 只有约 4,200 条（≈128 cycle/指令），是「只有一个 warp、访存延迟完全裸露」的
// 典型特征。换成 shuffle 之后 shared 只剩一个状态字。
//
// 交换代价：一个 RodV2Matrix 是 4 个块 × 3 个列向量 = 12 次 shuffle
// （vec3 可以整列一次 shuffle），每层每个 even lane 需要两侧邻居的
// inverse/lower/upper/right，约 76 次 shuffle，5 层合计约 380 次——远低于
// 原实现的 shared 访问次数，且都是寄存器到寄存器。

shared uint rodCrShuffleStatus;

RodV2Matrix RodCrShuffleMatrix(RodV2Matrix value, uint source)
{
    RodV2Matrix result;
    for (uint column = 0u; column < 3u; ++column)
    {
        result.linearLinear[column] =
            subgroupShuffle(value.linearLinear[column], source);
        result.linearAngular[column] =
            subgroupShuffle(value.linearAngular[column], source);
        result.angularLinear[column] =
            subgroupShuffle(value.angularLinear[column], source);
        result.angularAngular[column] =
            subgroupShuffle(value.angularAngular[column], source);
    }
    return result;
}

RodV2Vector RodCrShuffleVector(RodV2Vector value, uint source)
{
    return RodV2Vector(subgroupShuffle(value.linear, source),
        subgroupShuffle(value.angular, source));
}

bool RodCrShuffleFinite(RodV2Matrix matrix)
{
    bool finite = true;
    for (uint column = 0u; column < 3u; ++column)
    {
        finite = finite && !any(isnan(matrix.linearLinear[column])) &&
            !any(isinf(matrix.linearLinear[column]));
        finite = finite && !any(isnan(matrix.linearAngular[column])) &&
            !any(isinf(matrix.linearAngular[column]));
        finite = finite && !any(isnan(matrix.angularLinear[column])) &&
            !any(isinf(matrix.angularLinear[column]));
        finite = finite && !any(isnan(matrix.angularAngular[column])) &&
            !any(isinf(matrix.angularAngular[column]));
    }
    return finite;
}

bool RodCrShuffleFinite(RodV2Vector value)
{
    return !any(isnan(value.linear)) && !any(isinf(value.linear)) &&
        !any(isnan(value.angular)) && !any(isinf(value.angular));
}

RodV2Vector RodCrShuffleSubtract(RodV2Vector first, RodV2Vector last)
{
    return RodV2Vector(first.linear - last.linear, first.angular - last.angular);
}

// 与 RodCrSolveBand32 相同的语义与失败码：
//   1 = 输入非有限，2 = 分解失败，4 = 约化后非有限，8 = 解非有限。
bool RodCrSolveBandShuffle(uint lane, RodV2Matrix lower, RodV2Matrix diagonal,
    RodV2Matrix upper, RodV2Vector right, out RodV2Vector solution)
{
    if (lane == 0u)
        rodCrShuffleStatus = 0u;
    barrier();
    RodV2Matrix lowerValue = lane > 0u ? lower : RodV2Zero();
    RodV2Matrix upperValue = lane < 31u ? upper : RodV2Zero();
    RodV2Matrix diagonalValue = diagonal;
    RodV2Vector rightValue = right;
    RodV2Matrix inverseValue = RodV2Zero();
    if (!RodCrShuffleFinite(lower) || !RodCrShuffleFinite(diagonal) ||
        !RodCrShuffleFinite(upper) || !RodCrShuffleFinite(right))
    {
        atomicOr(rodCrShuffleStatus, 1u);
        lowerValue = RodV2Zero();
        upperValue = RodV2Zero();
        diagonalValue = RodV2Zero();
        rightValue = RodV2Vector(vec3(0), vec3(0));
    }
    for (uint stride = 1u; stride < 32u; stride *= 2u)
    {
        // 奇数位分解出逆，供两侧偶数位消元使用
        if (lane % (2u * stride) == stride)
        {
            RodV2Matrix inverse;
            if (!RodV2Inverse(diagonalValue, inverse) || !RodCrShuffleFinite(inverse))
            {
                atomicOr(rodCrShuffleStatus, 2u);
                inverse = RodV2Zero();
            }
            inverseValue = inverse;
        }
        // 所有 lane 都要参与 shuffle（子组操作要求活动 lane 一致）
        uint previousLane = lane >= stride ? lane - stride : lane;
        uint nextLane = lane + stride < 32u ? lane + stride : lane;
        RodV2Matrix previousInverse = RodCrShuffleMatrix(inverseValue, previousLane);
        RodV2Matrix previousLower = RodCrShuffleMatrix(lowerValue, previousLane);
        RodV2Matrix previousUpper = RodCrShuffleMatrix(upperValue, previousLane);
        RodV2Vector previousRight = RodCrShuffleVector(rightValue, previousLane);
        RodV2Matrix nextInverse = RodCrShuffleMatrix(inverseValue, nextLane);
        RodV2Matrix nextLower = RodCrShuffleMatrix(lowerValue, nextLane);
        RodV2Matrix nextUpper = RodCrShuffleMatrix(upperValue, nextLane);
        RodV2Vector nextRight = RodCrShuffleVector(rightValue, nextLane);
        bool retained = lane % (2u * stride) == 0u;
        if (retained)
        {
            RodV2Matrix reducedDiagonal = diagonalValue;
            RodV2Matrix reducedLower = RodV2Zero();
            RodV2Matrix reducedUpper = RodV2Zero();
            RodV2Vector reducedRight = rightValue;
            if (lane >= stride)
            {
                RodV2Matrix coefficient = RodV2Multiply(lowerValue, previousInverse);
                reducedLower = RodV2Negative(RodV2Multiply(coefficient, previousLower));
                reducedDiagonal = RodV2Add(reducedDiagonal,
                    RodV2Negative(RodV2Multiply(coefficient, previousUpper)));
                reducedRight = RodCrShuffleSubtract(reducedRight,
                    RodV2Multiply(coefficient, previousRight));
            }
            if (lane + stride < 32u)
            {
                RodV2Matrix coefficient = RodV2Multiply(upperValue, nextInverse);
                reducedUpper = RodV2Negative(RodV2Multiply(coefficient, nextUpper));
                reducedDiagonal = RodV2Add(reducedDiagonal,
                    RodV2Negative(RodV2Multiply(coefficient, nextLower)));
                reducedRight = RodCrShuffleSubtract(reducedRight,
                    RodV2Multiply(coefficient, nextRight));
            }
            if (!RodCrShuffleFinite(reducedLower) || !RodCrShuffleFinite(reducedDiagonal) ||
                !RodCrShuffleFinite(reducedUpper) || !RodCrShuffleFinite(reducedRight))
            {
                atomicOr(rodCrShuffleStatus, 4u);
                reducedLower = RodV2Zero();
                reducedDiagonal = RodV2Zero();
                reducedUpper = RodV2Zero();
                reducedRight = RodV2Vector(vec3(0), vec3(0));
            }
            lowerValue = reducedLower;
            diagonalValue = reducedDiagonal;
            upperValue = reducedUpper;
            rightValue = reducedRight;
        }
    }
    if (lane == 0u)
    {
        RodV2Matrix inverse;
        if (!RodV2Inverse(diagonalValue, inverse) || !RodCrShuffleFinite(inverse))
        {
            atomicOr(rodCrShuffleStatus, 2u);
            inverse = RodV2Zero();
        }
        inverseValue = inverse;
        RodV2Vector candidate = RodV2Multiply(inverse, rightValue);
        if (!RodCrShuffleFinite(candidate))
        {
            atomicOr(rodCrShuffleStatus, 8u);
            candidate = RodV2Vector(vec3(0), vec3(0));
        }
        solution = candidate;
    }
    else
    {
        solution = RodV2Vector(vec3(0), vec3(0));
    }
    for (uint stride = 16u; stride > 0u; stride /= 2u)
    {
        // 回代需要两侧的解；inverseValue 只在各层被保留的 lane 上有效，
        // 因此这里按层重新取邻居解即可。
        uint previousLane = lane >= stride ? lane - stride : lane;
        uint nextLane = lane + stride < 32u ? lane + stride : lane;
        RodV2Vector previousSolution = RodCrShuffleVector(solution, previousLane);
        RodV2Vector nextSolution = RodCrShuffleVector(solution, nextLane);
        if (lane % (2u * stride) == stride)
        {
            RodV2Vector value = rightValue;
            if (lane >= stride)
                value = RodCrShuffleSubtract(value,
                    RodV2Multiply(lowerValue, previousSolution));
            if (lane + stride < 32u)
                value = RodCrShuffleSubtract(value,
                    RodV2Multiply(upperValue, nextSolution));
            RodV2Vector candidate = RodV2Multiply(inverseValue, value);
            if (!RodCrShuffleFinite(candidate))
            {
                atomicOr(rodCrShuffleStatus, 8u);
                candidate = RodV2Vector(vec3(0), vec3(0));
            }
            solution = candidate;
        }
    }
    barrier();
    bool success = rodCrShuffleStatus == 0u;
    if (!success)
        solution = RodV2Vector(vec3(0), vec3(0));
    barrier();
    return success;
}

#endif
