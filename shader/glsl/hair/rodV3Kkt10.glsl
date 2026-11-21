#ifndef VL_ROD_V3_KKT10
#define VL_ROD_V3_KKT10

#include "rodV2Blocks.glsl"

struct RodKkt10Matrix
{
    float values[100];
};

struct RodKkt10Vector
{
    float values[10];
};

struct RodKkt10Contact
{
    vec3 gradient;
    mat3 hessian;
    float gap;
    float multiplier;
    bool enabled;
};

struct RodKkt10Edge
{
    vec3 direction;
    mat3 normalDerivative;
    float gap;
    float multiplier;
    bool enabled;
    bool leftFree;
    bool rightFree;
};

RodKkt10Matrix RodKkt10Zero()
{
    RodKkt10Matrix result;
    for (uint entry = 0u; entry < 100u; ++entry)
        result.values[entry] = 0.0;
    return result;
}

RodKkt10Matrix RodKkt10Embed(RodV2Matrix base)
{
    RodKkt10Matrix result = RodKkt10Zero();
    for (uint row = 0u; row < 6u; ++row)
        for (uint column = 0u; column < 6u; ++column)
            result.values[row * 10u + column] = RodV2Entry(base, row, column);
    return result;
}

void RodKkt10AddPosition(inout RodKkt10Matrix matrix, mat3 value)
{
    for (uint row = 0u; row < 3u; ++row)
        for (uint column = 0u; column < 3u; ++column)
            matrix.values[row * 10u + column] += value[column][row];
}

void RodKkt10AssembleRow(RodV2Matrix baseLower, RodV2Matrix baseDiagonal,
    RodV2Matrix baseUpper, RodV2Vector baseResidual, bool freePoint,
    RodKkt10Contact contacts[3], RodKkt10Edge incoming, RodKkt10Edge outgoing,
    float forceScale, float lengthScale, out RodKkt10Matrix lower,
    out RodKkt10Matrix diagonal, out RodKkt10Matrix upper,
    out RodKkt10Vector residual)
{
    lower = RodKkt10Embed(baseLower);
    diagonal = RodKkt10Embed(baseDiagonal);
    upper = RodKkt10Embed(baseUpper);
    for (uint component = 0u; component < 10u; ++component)
        residual.values[component] = component < 3u ? baseResidual.linear[component] :
            component < 6u ? baseResidual.angular[component - 3u] : 0.0;
    for (uint slot = 0u; slot < 3u; ++slot)
    {
        uint multiplierColumn = 6u + slot;
        diagonal.values[multiplierColumn * 10u + multiplierColumn] = 1.0;
        if (!freePoint || !contacts[slot].enabled)
            continue;
        diagonal.values[multiplierColumn * 10u + multiplierColumn] = 0.0;
        for (uint axis = 0u; axis < 3u; ++axis)
        {
            residual.values[axis] -= contacts[slot].multiplier *
                contacts[slot].gradient[axis] / forceScale;
            diagonal.values[axis * 10u + multiplierColumn] = -contacts[slot].gradient[axis];
            diagonal.values[multiplierColumn * 10u + axis] = contacts[slot].gradient[axis];
        }
        residual.values[multiplierColumn] = contacts[slot].gap / lengthScale;
        RodKkt10AddPosition(diagonal, -contacts[slot].multiplier *
            contacts[slot].hessian * (lengthScale / forceScale));
    }
    diagonal.values[99] = 1.0;
    if (incoming.enabled && (incoming.leftFree || incoming.rightFree))
    {
        diagonal.values[99] = 0.0;
        residual.values[9] = incoming.gap / lengthScale;
        for (uint axis = 0u; axis < 3u; ++axis)
        {
            if (incoming.leftFree)
                lower.values[90u + axis] = incoming.direction[axis];
            if (incoming.rightFree)
            {
                residual.values[axis] += incoming.multiplier * incoming.direction[axis] / forceScale;
                diagonal.values[axis * 10u + 9u] = incoming.direction[axis];
                diagonal.values[90u + axis] = -incoming.direction[axis];
            }
        }
        mat3 stiffness = incoming.multiplier * incoming.normalDerivative *
            (lengthScale / forceScale);
        if (incoming.rightFree)
            RodKkt10AddPosition(diagonal, stiffness);
        if (incoming.leftFree && incoming.rightFree)
            RodKkt10AddPosition(lower, -stiffness);
    }
    if (outgoing.enabled && outgoing.leftFree)
    {
        for (uint axis = 0u; axis < 3u; ++axis)
        {
            residual.values[axis] -= outgoing.multiplier * outgoing.direction[axis] / forceScale;
            upper.values[axis * 10u + 9u] = -outgoing.direction[axis];
        }
        mat3 stiffness = outgoing.multiplier * outgoing.normalDerivative *
            (lengthScale / forceScale);
        RodKkt10AddPosition(diagonal, stiffness);
        if (outgoing.rightFree)
            RodKkt10AddPosition(upper, -stiffness);
    }
}

bool RodKkt10PivotSolve(RodKkt10Matrix matrix, RodKkt10Vector right,
    out RodKkt10Vector solution)
{
    for (uint column = 0u; column < 10u; ++column)
    {
        uint pivot = column;
        float largest = abs(matrix.values[column * 10u + column]);
        for (uint row = column + 1u; row < 10u; ++row)
        {
            float value = abs(matrix.values[row * 10u + column]);
            if (value > largest)
            {
                largest = value;
                pivot = row;
            }
        }
        if (largest == 0.0 || isnan(largest) || isinf(largest))
            return false;
        if (pivot != column)
        {
            for (uint entry = 0u; entry < 10u; ++entry)
            {
                float value = matrix.values[column * 10u + entry];
                matrix.values[column * 10u + entry] = matrix.values[pivot * 10u + entry];
                matrix.values[pivot * 10u + entry] = value;
            }
            float value = right.values[column];
            right.values[column] = right.values[pivot];
            right.values[pivot] = value;
        }
        for (uint row = column + 1u; row < 10u; ++row)
        {
            float factor = matrix.values[row * 10u + column] /
                matrix.values[column * 10u + column];
            for (uint entry = column; entry < 10u; ++entry)
                matrix.values[row * 10u + entry] -= factor * matrix.values[column * 10u + entry];
            right.values[row] -= factor * right.values[column];
        }
    }
    for (int row = 9; row >= 0; --row)
    {
        float value = right.values[row];
        for (uint column = uint(row + 1); column < 10u; ++column)
            value -= matrix.values[uint(row) * 10u + column] * solution.values[column];
        solution.values[row] = value / matrix.values[uint(row) * 10u + uint(row)];
        if (isnan(solution.values[row]) || isinf(solution.values[row]))
            return false;
    }
    return true;
}

#endif
