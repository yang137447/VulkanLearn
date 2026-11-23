#ifndef VL_ROD_V2_BLOCKS
#define VL_ROD_V2_BLOCKS

struct RodV2Vector
{
    vec3 linear;
    vec3 angular;
};

struct RodV2Matrix
{
    mat3 linearLinear;
    mat3 linearAngular;
    mat3 angularLinear;
    mat3 angularAngular;
};

RodV2Matrix RodV2Zero()
{
    return RodV2Matrix(mat3(0), mat3(0), mat3(0), mat3(0));
}

RodV2Matrix RodV2Identity()
{
    return RodV2Matrix(mat3(1), mat3(0), mat3(0), mat3(1));
}

RodV2Vector RodV2Multiply(RodV2Matrix matrix, RodV2Vector vector)
{
    return RodV2Vector(matrix.linearLinear * vector.linear +
        matrix.linearAngular * vector.angular, matrix.angularLinear * vector.linear +
        matrix.angularAngular * vector.angular);
}

RodV2Matrix RodV2Multiply(RodV2Matrix left, RodV2Matrix right)
{
    return RodV2Matrix(
        left.linearLinear * right.linearLinear + left.linearAngular * right.angularLinear,
        left.linearLinear * right.linearAngular + left.linearAngular * right.angularAngular,
        left.angularLinear * right.linearLinear + left.angularAngular * right.angularLinear,
        left.angularLinear * right.linearAngular + left.angularAngular * right.angularAngular);
}

RodV2Matrix RodV2Add(RodV2Matrix first, RodV2Matrix second)
{
    return RodV2Matrix(first.linearLinear + second.linearLinear,
        first.linearAngular + second.linearAngular,
        first.angularLinear + second.angularLinear,
        first.angularAngular + second.angularAngular);
}

RodV2Matrix RodV2Negative(RodV2Matrix matrix)
{
    return RodV2Matrix(-matrix.linearLinear, -matrix.linearAngular,
        -matrix.angularLinear, -matrix.angularAngular);
}

RodV2Vector RodV2Add(RodV2Vector first, RodV2Vector second)
{
    return RodV2Vector(first.linear + second.linear, first.angular + second.angular);
}

float RodV2Entry(RodV2Matrix matrix, uint row, uint column)
{
    if (row < 3u)
        return column < 3u ? matrix.linearLinear[column][row] :
            matrix.linearAngular[column - 3u][row];
    return column < 3u ? matrix.angularLinear[column][row - 3u] :
        matrix.angularAngular[column - 3u][row - 3u];
}

void RodV2Set(inout RodV2Matrix matrix, uint row, uint column, float value)
{
    if (row < 3u)
    {
        if (column < 3u)
            matrix.linearLinear[column][row] = value;
        else
            matrix.linearAngular[column - 3u][row] = value;
    }
    else
    {
        if (column < 3u)
            matrix.angularLinear[column][row - 3u] = value;
        else
            matrix.angularAngular[column - 3u][row - 3u] = value;
    }
}

// 把 3x3 块写进 6x6 的指定块位置。element(r,c) 在 mat3 里是 m[c][r]（列主序），
// 所以这里用 block[column][row]，不要写反——手写索引很容易把块转置掉。
void RodV2SetBlock(inout RodV2Matrix matrix, uint baseRow, uint baseColumn, mat3 block)
{
    for (uint column = 0u; column < 3u; ++column)
        for (uint row = 0u; row < 3u; ++row)
            RodV2Set(matrix, baseRow + row, baseColumn + column, block[column][row]);
}

bool RodV2Inverse(RodV2Matrix inputMatrix, out RodV2Matrix inverseMatrix)
{
    // 6x6 求逆走 2x2 分块 + 内置 mat3 inverse，**不使用任何动态索引数组**。
    //
    // 原实现用 float augmented[72] 做全主元 Gauss-Jordan，其中
    // augmented[selected * 12u + pivot] 是动态索引，GLSL 会把这种数组放进
    // local memory（= 全局内存）。每求一次逆就是几百次带依赖的全延迟访存，
    // 这正是实测「单 warp 一次迭代 ≈ 0.25 ms / 约 4,200 条指令 ≈ 107 cycle/指令」
    // 的来源——不是寄存器溢出，也不是占用率不够。RodV2Matrix 本身是 2x2 个 mat3，
    // 用块公式可以全程留在寄存器里。
    //
    // M = [[A, B], [C, D]],  S = D - C A^-1 B
    //   M^-1 = [[A^-1 + A^-1 B S^-1 C A^-1,  -A^-1 B S^-1],
    //           [-S^-1 C A^-1,               S^-1        ]]
    // A 奇异时改用 D 作枢轴块。
    inverseMatrix = RodV2Zero();
    float scale = 0.0;
    for (uint row = 0u; row < 6u; ++row)
        for (uint column = 0u; column < 6u; ++column)
            scale = max(scale, abs(RodV2Entry(inputMatrix, row, column)));
    if (!(scale > 0.0) || isnan(scale) || isinf(scale))
        return false;
    RodV2Matrix matrix = inputMatrix;
    for (uint row = 0u; row < 6u; ++row)
        for (uint column = 0u; column < 6u; ++column)
            RodV2Set(matrix, row, column, RodV2Entry(matrix, row, column) / scale);

    mat3 a = matrix.linearLinear;
    mat3 b = matrix.linearAngular;
    mat3 c = matrix.angularLinear;
    mat3 d = matrix.angularAngular;
    if (abs(determinant(a)) > 0.0)
    {
        mat3 inverseA = inverse(a);
        mat3 s = d - c * inverseA * b;
        if (!(abs(determinant(s)) > 0.0))
            return false;
        mat3 inverseS = inverse(s);
        mat3 inverseAB = inverseA * b;
        RodV2SetBlock(inverseMatrix, 0u, 0u, inverseA + inverseAB * inverseS * c * inverseA);
        RodV2SetBlock(inverseMatrix, 0u, 3u, -inverseAB * inverseS);
        RodV2SetBlock(inverseMatrix, 3u, 0u, -inverseS * c * inverseA);
        RodV2SetBlock(inverseMatrix, 3u, 3u, inverseS);
    }
    else if (abs(determinant(d)) > 0.0)
    {
        mat3 inverseD = inverse(d);
        mat3 s = a - b * inverseD * c;
        if (!(abs(determinant(s)) > 0.0))
            return false;
        mat3 inverseS = inverse(s);
        mat3 inverseDC = inverseD * c;
        RodV2SetBlock(inverseMatrix, 0u, 0u, inverseS);
        RodV2SetBlock(inverseMatrix, 0u, 3u, -inverseS * b * inverseD);
        RodV2SetBlock(inverseMatrix, 3u, 0u, -inverseDC * inverseS);
        RodV2SetBlock(inverseMatrix, 3u, 3u,
            inverseD + inverseD * c * inverseS * b * inverseD);
    }
    else
    {
        return false;
    }
    for (uint row = 0u; row < 6u; ++row)
        for (uint column = 0u; column < 6u; ++column)
        {
            float value = RodV2Entry(inverseMatrix, row, column) / scale;
            if (isnan(value) || isinf(value))
            {
                inverseMatrix = RodV2Zero();
                return false;
            }
            RodV2Set(inverseMatrix, row, column, value);
        }
    return true;
}

#endif
