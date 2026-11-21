#ifndef VL_ROD_SMOOTH_FIELD
#define VL_ROD_SMOOTH_FIELD

const uint RodSmoothAvailable = 0u;
const uint RodSmoothOutsideDomain = 1u;
const uint RodSmoothPositiveSaturated = 2u;
const uint RodSmoothNegativeSaturated = 3u;
const uint RodSmoothTruncatedSupport = 4u;
const uint RodSmoothInvalidInput = 5u;
const uint RodSmoothNonFinite = 6u;
const uint RodSmoothBoundaryUncertain = 7u;

struct RodSmoothGrid
{
    uvec3 dimensions;
    vec3 originMetres;
    float cellMetres;
    float bandMetres;
};

struct RodSmoothSample
{
    uint status;
    float value;
    vec3 gradient;
    mat3 hessian;
    uvec3 supportBase;
    float minimumCoefficient;
    float maximumCoefficient;
};

struct RodSmoothBasis
{
    vec4 value;
    vec4 first;
    vec4 second;
};

float RodSmoothCoefficient(uvec3 index);

bool RodSmoothFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool RodSmoothFinite(vec3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

float RodSmoothRoundLower(float value)
{
    if (abs(value) < 2.350988701644575e-38)
        return -2.350988701644575e-38;
    uint bits = floatBitsToUint(value);
    return uintBitsToFloat(value > 0.0 ? bits - 4u : bits + 4u);
}

float RodSmoothRoundUpper(float value)
{
    if (abs(value) < 2.350988701644575e-38)
        return 2.350988701644575e-38;
    uint bits = floatBitsToUint(value);
    return uintBitsToFloat(value > 0.0 ? bits + 4u : bits - 4u);
}

bool RodSmoothInverseBounds(float cell, float inverseCell, out vec2 inverses)
{
    if (!(cell >= 1.1754943508222875e-38) ||
        !(inverseCell >= 1.1754943508222875e-38))
        return false;
    inverses = vec2(inverseCell);
    for (uint attempt = 0u; attempt < 8u; ++attempt)
    {
        inverses = vec2(RodSmoothRoundLower(inverses.x), RodSmoothRoundUpper(inverses.y));
        if (!(inverses.x > 0.0) || any(isinf(inverses)) || any(isnan(inverses)))
            return false;
        precise float firstProduct = cell * inverses.x;
        precise float lastProduct = cell * inverses.y;
        if (!RodSmoothFinite(firstProduct) || !RodSmoothFinite(lastProduct))
            return false;
        if (RodSmoothRoundUpper(firstProduct) <= 1.0 &&
            RodSmoothRoundLower(lastProduct) >= 1.0)
            return true;
    }
    return false;
}

bool RodSmoothCoordinateBounds(float position, float origin, vec2 inverses,
    out vec2 bounds)
{
    precise float difference = position - origin;
    if (!RodSmoothFinite(difference))
        return false;
    vec2 differences = vec2(RodSmoothRoundLower(difference), RodSmoothRoundUpper(difference));
    precise vec4 products = vec4(differences.x * inverses.x, differences.x * inverses.y,
        differences.y * inverses.x, differences.y * inverses.y);
    if (any(isnan(products)) || any(isinf(products)))
        return false;
    float minimum = min(min(products.x, products.y), min(products.z, products.w));
    float maximum = max(max(products.x, products.y), max(products.z, products.w));
    precise float lowerDifference = RodSmoothRoundLower(minimum) - 0.5;
    precise float upperDifference = RodSmoothRoundUpper(maximum) - 0.5;
    bounds = vec2(RodSmoothRoundLower(lowerDifference), RodSmoothRoundUpper(upperDifference));
    return !any(isnan(bounds)) && !any(isinf(bounds));
}

RodSmoothBasis RodSmoothBasisAt(float fraction)
{
    float square = fraction * fraction;
    float cube = square * fraction;
    float remaining = 1.0 - fraction;
    return RodSmoothBasis(
        vec4(remaining * remaining * remaining / 6.0,
            (3.0 * cube - 6.0 * square + 4.0) / 6.0,
            (-3.0 * cube + 3.0 * square + 3.0 * fraction + 1.0) / 6.0,
            cube / 6.0),
        vec4(-0.5 * remaining * remaining, 1.5 * square - 2.0 * fraction,
            -1.5 * square + fraction + 0.5, 0.5 * square),
        vec4(remaining, 3.0 * fraction - 2.0, 1.0 - 3.0 * fraction, fraction));
}

RodSmoothSample RodSampleSmoothField(RodSmoothGrid grid, vec3 positionMetres)
{
    RodSmoothSample result = RodSmoothSample(RodSmoothInvalidInput, 0.0,
        vec3(0), mat3(0), uvec3(0), 0.0, 0.0);
    if (any(lessThan(grid.dimensions, uvec3(4))) ||
        any(greaterThan(grid.dimensions, uvec3(16777216))) ||
        !RodSmoothFinite(grid.originMetres) || !RodSmoothFinite(positionMetres) ||
        !RodSmoothFinite(grid.cellMetres) || !(grid.cellMetres > 0.0) ||
        !RodSmoothFinite(grid.bandMetres) || !(grid.bandMetres > 0.0))
        return result;
    precise float inverseCell = 1.0 / grid.cellMetres;
    float inverseCellSquared = inverseCell * inverseCell;
    if (!RodSmoothFinite(inverseCell) || !RodSmoothFinite(inverseCellSquared))
        return result;
    precise vec3 coordinate = (positionMetres - grid.originMetres) * inverseCell - vec3(0.5);
    if (!RodSmoothFinite(coordinate))
    {
        result.status = RodSmoothNonFinite;
        return result;
    }
    RodSmoothBasis basis[3];
    vec2 inverses;
    if (!RodSmoothInverseBounds(grid.cellMetres, inverseCell, inverses))
    {
        result.status = RodSmoothBoundaryUncertain;
        return result;
    }
    uvec3 supportFirst;
    uvec3 supportLast;
    bool uncertain = false;
    for (uint axis = 0u; axis < 3u; ++axis)
    {
        float upper = float(grid.dimensions[axis] - 2u);
        vec2 bounds;
        if (!RodSmoothCoordinateBounds(positionMetres[axis], grid.originMetres[axis],
            inverses, bounds))
        {
            result.status = RodSmoothNonFinite;
            return result;
        }
        if (bounds.y < 1.0 || bounds.x > upper)
        {
            result.status = RodSmoothOutsideDomain;
            return result;
        }
        uncertain = uncertain || bounds.x < 1.0 || bounds.y > upper ||
            coordinate[axis] < 1.0 || coordinate[axis] > upper;
        if (bounds.x >= 1.0 && bounds.y <= upper)
        {
            uint firstCell = bounds.x == upper ? grid.dimensions[axis] - 3u :
                uint(floor(bounds.x));
            uint lastCell = bounds.y == upper ? grid.dimensions[axis] - 3u :
                uint(floor(bounds.y));
            supportFirst[axis] = firstCell - 1u;
            supportLast[axis] = lastCell + 2u;
            uncertain = uncertain || lastCell - firstCell > 1u;
        }
    }
    if (uncertain)
    {
        result.status = RodSmoothBoundaryUncertain;
        return result;
    }
    for (uint axis = 0u; axis < 3u; ++axis)
    {
        float upper = float(grid.dimensions[axis] - 2u);
        uint cell = coordinate[axis] == upper ? grid.dimensions[axis] - 3u :
            uint(floor(coordinate[axis]));
        result.supportBase[axis] = cell - 1u;
        basis[axis] = RodSmoothBasisAt(coordinate[axis] - float(cell));
    }
    float reference = RodSmoothCoefficient(result.supportBase);
    if (!RodSmoothFinite(reference) || reference < -grid.bandMetres || reference > grid.bandMetres)
        return result;
    result.value = reference;
    result.minimumCoefficient = reference;
    result.maximumCoefficient = reference;
    bool allPositive = true;
    bool allNegative = true;
    bool truncated = false;
    for (uint third = 0u; third < 4u; ++third)
        for (uint second = 0u; second < 4u; ++second)
            for (uint first = 0u; first < 4u; ++first)
            {
                float coefficient = RodSmoothCoefficient(result.supportBase + uvec3(first, second, third));
                if (!RodSmoothFinite(coefficient) ||
                    coefficient < -grid.bandMetres || coefficient > grid.bandMetres)
                {
                    result.status = RodSmoothInvalidInput;
                    return result;
                }
                result.minimumCoefficient = min(result.minimumCoefficient, coefficient);
                result.maximumCoefficient = max(result.maximumCoefficient, coefficient);
                allPositive = allPositive && coefficient == grid.bandMetres;
                allNegative = allNegative && coefficient == -grid.bandMetres;
                truncated = truncated || coefficient == grid.bandMetres || coefficient == -grid.bandMetres;
                float difference = coefficient - reference;
                uvec3 offsets = uvec3(first, second, third);
                float weight = basis[0].value[first] * basis[1].value[second] * basis[2].value[third];
                result.value += difference * weight;
                for (uint axis = 0u; axis < 3u; ++axis)
                {
                    uint other = (axis + 1u) % 3u;
                    uint remaining = (axis + 2u) % 3u;
                    float otherWeight = basis[other].value[offsets[other]] *
                        basis[remaining].value[offsets[remaining]];
                    result.gradient[axis] += difference * basis[axis].first[offsets[axis]] *
                        otherWeight * inverseCell;
                    result.hessian[axis][axis] += difference * basis[axis].second[offsets[axis]] *
                        otherWeight * inverseCellSquared;
                    for (uint column = axis + 1u; column < 3u; ++column)
                    {
                        uint last = 3u - axis - column;
                        result.hessian[column][axis] += difference * basis[axis].first[offsets[axis]] *
                            basis[column].first[offsets[column]] * basis[last].value[offsets[last]] *
                            inverseCellSquared;
                    }
                }
            }
    for (uint axis = 0u; axis < 3u; ++axis)
        for (uint column = axis + 1u; column < 3u; ++column)
            result.hessian[axis][column] = result.hessian[column][axis];
    if (any(notEqual(supportFirst, result.supportBase)) ||
        any(notEqual(supportLast, result.supportBase + uvec3(3))))
    {
        for (uint third = supportFirst.z; third <= supportLast.z; ++third)
            for (uint second = supportFirst.y; second <= supportLast.y; ++second)
                for (uint first = supportFirst.x; first <= supportLast.x; ++first)
                {
                    float coefficient = RodSmoothCoefficient(uvec3(first, second, third));
                    if (!RodSmoothFinite(coefficient) ||
                        coefficient < -grid.bandMetres || coefficient > grid.bandMetres)
                    {
                        result.status = RodSmoothInvalidInput;
                        return result;
                    }
                    result.minimumCoefficient = min(result.minimumCoefficient, coefficient);
                    result.maximumCoefficient = max(result.maximumCoefficient, coefficient);
                    allPositive = allPositive && coefficient == grid.bandMetres;
                    allNegative = allNegative && coefficient == -grid.bandMetres;
                    truncated = truncated || coefficient == grid.bandMetres || coefficient == -grid.bandMetres;
                }
    }
    if (!RodSmoothFinite(result.value) || !RodSmoothFinite(result.gradient) ||
        !RodSmoothFinite(result.hessian[0]) || !RodSmoothFinite(result.hessian[1]) ||
        !RodSmoothFinite(result.hessian[2]))
        result.status = RodSmoothNonFinite;
    else if (allPositive)
        result.status = RodSmoothPositiveSaturated;
    else if (allNegative)
        result.status = RodSmoothNegativeSaturated;
    else if (truncated)
        result.status = RodSmoothTruncatedSupport;
    else
        result.status = RodSmoothAvailable;
    return result;
}

#endif
