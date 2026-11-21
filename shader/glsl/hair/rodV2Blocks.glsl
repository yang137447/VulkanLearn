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

bool RodV2Inverse(RodV2Matrix inputMatrix, out RodV2Matrix inverseMatrix)
{
    float augmented[72];
    float scale = 0.0;
    for (uint row = 0u; row < 6u; ++row)
        for (uint column = 0u; column < 6u; ++column)
            scale = max(scale, abs(RodV2Entry(inputMatrix, row, column)));
    if (!(scale > 0.0) || isnan(scale) || isinf(scale))
        return false;
    for (uint row = 0u; row < 6u; ++row)
        for (uint column = 0u; column < 12u; ++column)
            augmented[row * 12u + column] = column < 6u ?
                RodV2Entry(inputMatrix, row, column) / scale : (column - 6u == row ? 1.0 : 0.0);
    for (uint pivot = 0u; pivot < 6u; ++pivot)
    {
        uint selected = pivot;
        for (uint row = pivot + 1u; row < 6u; ++row)
            if (abs(augmented[row * 12u + pivot]) > abs(augmented[selected * 12u + pivot]))
                selected = row;
        float pivotValue = augmented[selected * 12u + pivot];
        if (!(abs(pivotValue) > 1.0e-20))
            return false;
        for (uint column = 0u; column < 12u; ++column)
        {
            float swapValue = augmented[pivot * 12u + column];
            augmented[pivot * 12u + column] = augmented[selected * 12u + column] / pivotValue;
            augmented[selected * 12u + column] = selected == pivot ?
                augmented[pivot * 12u + column] : swapValue;
        }
        for (uint row = 0u; row < 6u; ++row)
            if (row != pivot)
            {
                float multiplier = augmented[row * 12u + pivot];
                for (uint column = 0u; column < 12u; ++column)
                    augmented[row * 12u + column] -= multiplier * augmented[pivot * 12u + column];
            }
    }
    inverseMatrix = RodV2Zero();
    for (uint row = 0u; row < 6u; ++row)
        for (uint column = 0u; column < 6u; ++column)
            RodV2Set(inverseMatrix, row, column, augmented[row * 12u + column + 6u] / scale);
    return true;
}

#endif
