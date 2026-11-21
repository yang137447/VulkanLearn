#ifndef VL_ROD_COMPENSATED_STATE
#define VL_ROD_COMPENSATED_STATE

void RodCompensatedAdd(float first, float second, out float high, out float low)
{
    precise float sum = first + second;
    precise float reconstructed = sum - first;
    precise float error = (first - (sum - reconstructed)) + (second - reconstructed);
    high = sum;
    low = error;
}

void RodCompensatedAccumulate(inout float high, inout float low, float value)
{
    precise float sum, error;
    RodCompensatedAdd(high, value, sum, error);
    RodCompensatedAdd(sum, low + error, high, low);
}

void RodCompensatedPosition(vec3 baseHigh, vec3 baseLow, vec3 increment,
    out vec3 high, out vec3 low)
{
    high = baseHigh;
    low = baseLow;
    for (uint axis = 0u; axis < 3u; ++axis)
        RodCompensatedAccumulate(high[axis], low[axis], increment[axis]);
}

void RodCompensatedQuaternion(vec4 baseHigh, vec4 baseLow, vec4 increment,
    out vec4 high, out vec4 low)
{
    high = vec4(0);
    low = vec4(0);
    for (uint row = 0u; row < 4u; ++row)
        for (uint column = 0u; column < 4u; ++column)
        {
            float coefficient;
            if (row == 3u)
                coefficient = column == 3u ? increment.w : -increment[column];
            else if (column == 3u)
                coefficient = increment[row];
            else if (column == row)
                coefficient = increment.w;
            else
            {
                uint remaining = 3u - row - column;
                coefficient = ((row + 1u) % 3u == column ? 1.0 : -1.0) * increment[remaining];
            }
            precise float product = baseHigh[column] * coefficient;
            precise float error = fma(baseHigh[column], coefficient, -product) +
                baseLow[column] * coefficient;
            RodCompensatedAccumulate(high[row], low[row], product);
            RodCompensatedAccumulate(high[row], low[row], error);
        }
    precise float squaredHigh = 0.0;
    precise float squaredLow = 0.0;
    for (uint axis = 0u; axis < 4u; ++axis)
    {
        precise float product = high[axis] * high[axis];
        precise float error = fma(high[axis], high[axis], -product) +
            2.0 * high[axis] * low[axis] + low[axis] * low[axis];
        RodCompensatedAccumulate(squaredHigh, squaredLow, product);
        RodCompensatedAccumulate(squaredHigh, squaredLow, error);
    }
    precise float magnitude = sqrt(squaredHigh);
    precise float magnitudeLow = (fma(-magnitude, magnitude, squaredHigh) + squaredLow) /
        (2.0 * magnitude);
    for (uint axis = 0u; axis < 4u; ++axis)
    {
        precise float value = high[axis] / magnitude;
        precise float error = (fma(-value, magnitude, high[axis]) + low[axis] -
            value * magnitudeLow) / magnitude;
        RodCompensatedAdd(value, error, high[axis], low[axis]);
    }
}

vec3 RodCompensatedBodyOffset(vec4 high, vec4 low)
{
    return 2.0 * (high.w * low.xyz - low.w * high.xyz - cross(high.xyz, low.xyz));
}

#endif
