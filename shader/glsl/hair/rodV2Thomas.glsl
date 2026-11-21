#ifndef VL_ROD_V2_THOMAS
#define VL_ROD_V2_THOMAS

bool RodV2SolveMultiple(RodV2Matrix matrix, RodV2Matrix upper, RodV2Vector right,
    out RodV2Matrix reducedUpper, out RodV2Vector reducedRight)
{
    float values[78];
    float scale = 0.0;
    for (uint row = 0u; row < 6u; ++row)
        for (uint column = 0u; column < 6u; ++column)
            scale = max(scale, abs(RodV2Entry(matrix, row, column)));
    if (!(scale > 0.0))
        return false;
    for (uint row = 0u; row < 6u; ++row)
    {
        for (uint column = 0u; column < 6u; ++column)
        {
            values[row * 13u + column] = RodV2Entry(matrix, row, column) / scale;
            values[row * 13u + column + 6u] = RodV2Entry(upper, row, column) / scale;
        }
        values[row * 13u + 12u] = (row < 3u ? right.linear[row] : right.angular[row - 3u]) / scale;
    }
    for (uint pivot = 0u; pivot < 6u; ++pivot)
    {
        uint selected = pivot;
        for (uint row = pivot + 1u; row < 6u; ++row)
            if (abs(values[row * 13u + pivot]) > abs(values[selected * 13u + pivot]))
                selected = row;
        if (!(abs(values[selected * 13u + pivot]) > 1.0e-20))
            return false;
        for (uint column = 0u; column < 13u; ++column)
        {
            float swapValue = values[pivot * 13u + column];
            values[pivot * 13u + column] = values[selected * 13u + column];
            values[selected * 13u + column] = swapValue;
        }
        for (uint row = pivot + 1u; row < 6u; ++row)
        {
            float multiplier = values[row * 13u + pivot] / values[pivot * 13u + pivot];
            for (uint column = pivot + 1u; column < 13u; ++column)
                values[row * 13u + column] -= multiplier * values[pivot * 13u + column];
            values[row * 13u + pivot] = 0.0;
        }
    }
    for (int row = 5; row >= 0; --row)
        for (uint column = 6u; column < 13u; ++column)
        {
            float value = values[uint(row) * 13u + column];
            for (uint entry = uint(row) + 1u; entry < 6u; ++entry)
                value -= values[uint(row) * 13u + entry] * values[entry * 13u + column];
            values[uint(row) * 13u + column] = value / values[uint(row) * 13u + uint(row)];
        }
    reducedUpper = RodV2Zero();
    for (uint row = 0u; row < 6u; ++row)
        for (uint column = 0u; column < 6u; ++column)
            RodV2Set(reducedUpper, row, column, values[row * 13u + column + 6u]);
    reducedRight = RodV2Vector(vec3(values[12], values[25], values[38]),
        vec3(values[51], values[64], values[77]));
    return RodFinite(reducedRight.linear) && RodFinite(reducedRight.angular);
}

RodV2Vector RodV2Thomas(uint lane, RodV2Matrix lower, RodV2Matrix diagonal,
    RodV2Matrix upper, RodV2Vector right)
{
    if (lane == 0u)
        rodV2LinearStatus = 0u;
    RodV2StoreBand(0u, lane, 0u, lower);
    RodV2StoreBand(0u, lane, 1u, diagonal);
    RodV2StoreBand(0u, lane, 2u, upper);
    rodV2RightLinear[lane] = right.linear;
    rodV2RightAngular[lane] = right.angular;
    barrier();
    if (lane == 0u)
    {
        RodV2Matrix previousUpper = RodV2Zero();
        RodV2Vector previousRight = RodV2Vector(vec3(0), vec3(0));
        for (uint block = 0u; block < 32u; ++block)
        {
            RodV2Matrix currentDiagonal = RodV2LoadBand(0u, block, 1u);
            RodV2Vector currentRight = RodV2LoadRight(0u, block);
            if (block > 0u)
            {
                RodV2Matrix currentLower = RodV2LoadBand(0u, block, 0u);
                currentDiagonal = RodV2Add(currentDiagonal,
                    RodV2Negative(RodV2Multiply(currentLower, previousUpper)));
                RodV2Vector product = RodV2Multiply(currentLower, previousRight);
                currentRight = RodV2Add(currentRight, RodV2Vector(-product.linear, -product.angular));
            }
            if (!RodV2SolveMultiple(currentDiagonal, RodV2LoadBand(0u, block, 2u),
                currentRight, previousUpper, previousRight))
            {
                rodV2LinearStatus = 4u;
                previousUpper = RodV2Zero();
                previousRight = RodV2Vector(vec3(0), vec3(0));
            }
            RodV2StoreBand(1u, block, 2u, previousUpper);
            rodV2RightLinear[32u + block] = previousRight.linear;
            rodV2RightAngular[32u + block] = previousRight.angular;
        }
        for (int block = 30; block >= 0; --block)
        {
            RodV2Vector product = RodV2Multiply(RodV2LoadBand(1u, uint(block), 2u),
                RodV2LoadRight(1u, uint(block) + 1u));
            rodV2RightLinear[32u + uint(block)] -= product.linear;
            rodV2RightAngular[32u + uint(block)] -= product.angular;
        }
    }
    barrier();
    return RodV2LoadRight(1u, lane);
}

#endif
