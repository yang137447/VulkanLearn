#ifndef VL_ROD_FAST_BLOCKS
#define VL_ROD_FAST_BLOCKS

#include "rodV2Blocks.glsl"

bool RodFastFinite(mat3 matrix)
{
    return !any(isnan(matrix[0])) && !any(isinf(matrix[0])) &&
        !any(isnan(matrix[1])) && !any(isinf(matrix[1])) &&
        !any(isnan(matrix[2])) && !any(isinf(matrix[2]));
}

bool RodFastFinite(RodV2Matrix matrix)
{
    return RodFastFinite(matrix.linearLinear) && RodFastFinite(matrix.linearAngular) &&
        RodFastFinite(matrix.angularLinear) && RodFastFinite(matrix.angularAngular);
}

float RodFastMaxAbs(mat3 matrix)
{
    vec3 maximum = max(max(abs(matrix[0]), abs(matrix[1])), abs(matrix[2]));
    return max(maximum.x, max(maximum.y, maximum.z));
}

float RodFastMaxAbs(RodV2Matrix matrix)
{
    return max(max(RodFastMaxAbs(matrix.linearLinear), RodFastMaxAbs(matrix.linearAngular)),
        max(RodFastMaxAbs(matrix.angularLinear), RodFastMaxAbs(matrix.angularAngular)));
}

RodV2Matrix RodFastScale(RodV2Matrix matrix, float scale)
{
    return RodV2Matrix(matrix.linearLinear * scale, matrix.linearAngular * scale,
        matrix.angularLinear * scale, matrix.angularAngular * scale);
}

float RodFastNormInf(RodV2Matrix matrix)
{
    vec3 linear = abs(matrix.linearLinear[0]) + abs(matrix.linearLinear[1]) +
        abs(matrix.linearLinear[2]) + abs(matrix.linearAngular[0]) +
        abs(matrix.linearAngular[1]) + abs(matrix.linearAngular[2]);
    vec3 angular = abs(matrix.angularLinear[0]) + abs(matrix.angularLinear[1]) +
        abs(matrix.angularLinear[2]) + abs(matrix.angularAngular[0]) +
        abs(matrix.angularAngular[1]) + abs(matrix.angularAngular[2]);
    vec3 maximum = max(linear, angular);
    return max(maximum.x, max(maximum.y, maximum.z));
}

bool RodFastInverse3(mat3 matrix, out mat3 result)
{
    float scale = RodFastMaxAbs(matrix);
    if (!(scale > 0.0) || isnan(scale) || isinf(scale))
        return false;
    mat3 normalized = matrix / scale;
    vec3 first = cross(normalized[1], normalized[2]);
    vec3 second = cross(normalized[2], normalized[0]);
    vec3 third = cross(normalized[0], normalized[1]);
    float determinant = dot(normalized[0], first);
    float lengths = length(normalized[0]) * length(normalized[1]) * length(normalized[2]);
    if (!(lengths > 0.0) || isnan(lengths) || isinf(lengths) ||
        !(abs(determinant) > 7.62939453125e-6 * lengths))
        return false;
    result = (transpose(mat3(first, second, third)) / determinant) / scale;
    return RodFastFinite(result);
}

bool RodFastScreen(RodV2Matrix matrix, RodV2Matrix candidate)
{
    float matrixScale = RodFastMaxAbs(matrix);
    float inverseScale = RodFastMaxAbs(candidate);
    if (!(matrixScale > 0.0) || !(inverseScale > 0.0))
        return false;
    float reciprocalProduct = matrixScale >= inverseScale ?
        (1.0 / matrixScale) / inverseScale : (1.0 / inverseScale) / matrixScale;
    if (!(reciprocalProduct >= 1.1754943508222875e-38) ||
        isnan(reciprocalProduct) || isinf(reciprocalProduct))
        return false;
    RodV2Matrix normalizedMatrix = RodFastScale(matrix, 1.0 / matrixScale);
    RodV2Matrix normalizedInverse = RodFastScale(candidate, 1.0 / inverseScale);
    float denominator = max(reciprocalProduct,
        RodFastNormInf(normalizedMatrix) * RodFastNormInf(normalizedInverse));
    RodV2Matrix identity = RodFastScale(RodV2Identity(), reciprocalProduct);
    RodV2Matrix left = RodV2Add(identity,
        RodV2Negative(RodV2Multiply(normalizedMatrix, normalizedInverse)));
    RodV2Matrix right = RodV2Add(identity,
        RodV2Negative(RodV2Multiply(normalizedInverse, normalizedMatrix)));
    if (!RodFastFinite(left) || !RodFastFinite(right) ||
        !(denominator > 0.0) || isnan(denominator) || isinf(denominator))
        return false;
    return max(RodFastNormInf(left), RodFastNormInf(right)) / denominator <= 7.62939453125e-6;
}

bool RodFastSchurInverse(RodV2Matrix matrix, out RodV2Matrix result)
{
    if (!RodFastFinite(matrix))
        return false;
    mat3 primary;
    if (!RodFastInverse3(matrix.linearLinear, primary))
        return false;
    mat3 upper = primary * matrix.linearAngular;
    mat3 lower = matrix.angularLinear * primary;
    mat3 schur;
    if (!RodFastInverse3(matrix.angularAngular - matrix.angularLinear * upper, schur))
        return false;
    mat3 upperProduct = upper * schur;
    mat3 lowerProduct = schur * lower;
    result = RodV2Matrix(primary + upperProduct * lower, -upperProduct,
        -lowerProduct, schur);
    return RodFastFinite(result) && RodFastScreen(matrix, result);
}

bool RodFastInverse(RodV2Matrix inputMatrix, out RodV2Matrix inverseMatrix)
{
    if (RodFastSchurInverse(inputMatrix, inverseMatrix))
        return true;
    if (!RodFastFinite(inputMatrix))
        return false;
    return RodV2Inverse(inputMatrix, inverseMatrix) && RodFastFinite(inverseMatrix);
}

#endif
