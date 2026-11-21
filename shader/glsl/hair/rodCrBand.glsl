#ifndef VL_ROD_CR_BAND
#define VL_ROD_CR_BAND

#include "rodV2Blocks.glsl"

shared RodV2Matrix rodCrLower[32];
shared RodV2Matrix rodCrDiagonal[32];
shared RodV2Matrix rodCrUpper[32];
shared RodV2Matrix rodCrInverse[32];
shared RodV2Vector rodCrRight[32];
shared RodV2Vector rodCrSolution[32];
shared uint rodCrStatus;

bool RodCrFinite(vec3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

bool RodCrFinite(mat3 matrix)
{
    return RodCrFinite(matrix[0]) && RodCrFinite(matrix[1]) && RodCrFinite(matrix[2]);
}

bool RodCrFinite(RodV2Matrix matrix)
{
    return RodCrFinite(matrix.linearLinear) && RodCrFinite(matrix.linearAngular) &&
        RodCrFinite(matrix.angularLinear) && RodCrFinite(matrix.angularAngular);
}

bool RodCrFinite(RodV2Vector value)
{
    return RodCrFinite(value.linear) && RodCrFinite(value.angular);
}

RodV2Vector RodCrSubtract(RodV2Vector first, RodV2Vector last)
{
    return RodV2Vector(first.linear-last.linear, first.angular-last.angular);
}

bool RodCrFactor(RodV2Matrix matrix, out RodV2Matrix inverse)
{
    inverse = RodV2Zero();
    if (!RodV2Inverse(matrix, inverse) || !RodCrFinite(inverse))
    {
        inverse = RodV2Zero();
        return false;
    }
    return true;
}

bool RodCrSolveBand32(uint lane, RodV2Matrix lower, RodV2Matrix diagonal,
    RodV2Matrix upper, RodV2Vector right, out RodV2Vector solution)
{
    if (lane == 0u)
        rodCrStatus = 0u;
    rodCrLower[lane] = lane > 0u ? lower : RodV2Zero();
    rodCrDiagonal[lane] = diagonal;
    rodCrUpper[lane] = lane < 31u ? upper : RodV2Zero();
    rodCrRight[lane] = right;
    rodCrInverse[lane] = RodV2Zero();
    rodCrSolution[lane] = RodV2Vector(vec3(0), vec3(0));
    barrier();
    if (!RodCrFinite(lower) || !RodCrFinite(diagonal) ||
        !RodCrFinite(upper) || !RodCrFinite(right))
    {
        atomicOr(rodCrStatus, 1u);
        rodCrLower[lane] = RodV2Zero();
        rodCrDiagonal[lane] = RodV2Zero();
        rodCrUpper[lane] = RodV2Zero();
        rodCrRight[lane] = RodV2Vector(vec3(0), vec3(0));
    }
    barrier();
    for (uint stride = 1u; stride < 32u; stride *= 2u)
    {
        if (lane % (2u * stride) == stride)
        {
            RodV2Matrix inverse;
            if (!RodCrFactor(rodCrDiagonal[lane], inverse))
                atomicOr(rodCrStatus, 2u);
            rodCrInverse[lane] = inverse;
        }
        barrier();
        RodV2Matrix reducedLower = RodV2Zero();
        RodV2Matrix reducedDiagonal = RodV2Zero();
        RodV2Matrix reducedUpper = RodV2Zero();
        RodV2Vector reducedRight = RodV2Vector(vec3(0), vec3(0));
        bool retained = lane % (2u * stride) == 0u;
        if (retained)
        {
            reducedDiagonal = rodCrDiagonal[lane];
            reducedRight = rodCrRight[lane];
            if (lane >= stride)
            {
                uint neighbor = lane - stride;
                RodV2Matrix coefficient = RodV2Multiply(rodCrLower[lane], rodCrInverse[neighbor]);
                reducedLower = RodV2Negative(RodV2Multiply(coefficient, rodCrLower[neighbor]));
                reducedDiagonal = RodV2Add(reducedDiagonal,
                    RodV2Negative(RodV2Multiply(coefficient, rodCrUpper[neighbor])));
                reducedRight = RodCrSubtract(reducedRight,
                    RodV2Multiply(coefficient, rodCrRight[neighbor]));
            }
            if (lane + stride < 32u)
            {
                uint neighbor = lane + stride;
                RodV2Matrix coefficient = RodV2Multiply(rodCrUpper[lane], rodCrInverse[neighbor]);
                reducedUpper = RodV2Negative(RodV2Multiply(coefficient, rodCrUpper[neighbor]));
                reducedDiagonal = RodV2Add(reducedDiagonal,
                    RodV2Negative(RodV2Multiply(coefficient, rodCrLower[neighbor])));
                reducedRight = RodCrSubtract(reducedRight,
                    RodV2Multiply(coefficient, rodCrRight[neighbor]));
            }
            if (!RodCrFinite(reducedLower) || !RodCrFinite(reducedDiagonal) ||
                !RodCrFinite(reducedUpper) || !RodCrFinite(reducedRight))
            {
                atomicOr(rodCrStatus, 4u);
                reducedLower = RodV2Zero();
                reducedDiagonal = RodV2Zero();
                reducedUpper = RodV2Zero();
                reducedRight = RodV2Vector(vec3(0), vec3(0));
            }
        }
        barrier();
        if (retained)
        {
            rodCrLower[lane] = reducedLower;
            rodCrDiagonal[lane] = reducedDiagonal;
            rodCrUpper[lane] = reducedUpper;
            rodCrRight[lane] = reducedRight;
        }
        barrier();
    }
    if (lane == 0u)
    {
        RodV2Matrix inverse;
        if (!RodCrFactor(rodCrDiagonal[0], inverse))
            atomicOr(rodCrStatus, 2u);
        rodCrSolution[0] = RodV2Multiply(inverse, rodCrRight[0]);
        if (!RodCrFinite(rodCrSolution[0]))
        {
            atomicOr(rodCrStatus, 8u);
            rodCrSolution[0] = RodV2Vector(vec3(0), vec3(0));
        }
    }
    barrier();
    for (uint stride = 16u; stride > 0u; stride /= 2u)
    {
        if (lane % (2u * stride) == stride)
        {
            RodV2Vector value = rodCrRight[lane];
            if (lane >= stride)
                value = RodCrSubtract(value, RodV2Multiply(rodCrLower[lane],
                    rodCrSolution[lane - stride]));
            if (lane + stride < 32u)
                value = RodCrSubtract(value, RodV2Multiply(rodCrUpper[lane],
                    rodCrSolution[lane + stride]));
            RodV2Vector candidate = RodV2Multiply(rodCrInverse[lane], value);
            if (!RodCrFinite(candidate))
            {
                atomicOr(rodCrStatus, 8u);
                candidate = RodV2Vector(vec3(0), vec3(0));
            }
            rodCrSolution[lane] = candidate;
        }
        barrier();
    }
    bool success = rodCrStatus == 0u;
    solution = success ? rodCrSolution[lane] : RodV2Vector(vec3(0), vec3(0));
    barrier();
    return success;
}

#endif
