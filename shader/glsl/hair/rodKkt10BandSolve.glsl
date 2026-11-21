#ifndef VL_ROD_KKT10_BAND_SOLVE
#define VL_ROD_KKT10_BAND_SOLVE

#include "rodV3Kkt10.glsl"

RodKkt10Matrix RodKktLoadBand(uint point, uint band);
RodKkt10Vector RodKktLoadResidual(uint point);
shared RodKkt10Matrix rodKktFactorUpper[32];
shared RodKkt10Vector rodKktFactorRight[32];
shared RodKkt10Vector rodKktDirection[32];
shared uint rodKktNextPoint[32];

bool RodKktEquilibratedPivot(uint dimension, inout float matrix[400],
    inout float right[220], out float maximumPivot, out float minimumPivot)
{
    float columnScale[20];
    uint permutation[20];
    for (uint row = 0u; row < dimension; ++row)
    {
        float maximum = 0.0;
        for (uint column = 0u; column < dimension; ++column)
            maximum = max(maximum, abs(matrix[row * 20u + column]));
        float scale = maximum > 0.0 ? 1.0 / maximum : 1.0;
        for (uint column = 0u; column < dimension; ++column)
            matrix[row * 20u + column] *= scale;
        for (uint entry = 0u; entry < 11u; ++entry)
            right[row * 11u + entry] *= scale;
    }
    for (uint column = 0u; column < dimension; ++column)
    {
        float maximum = 0.0;
        for (uint row = 0u; row < dimension; ++row)
            maximum = max(maximum, abs(matrix[row * 20u + column]));
        columnScale[column] = maximum > 0.0 ? 1.0 / maximum : 1.0;
        permutation[column] = column;
        for (uint row = 0u; row < dimension; ++row)
            matrix[row * 20u + column] *= columnScale[column];
    }
    maximumPivot = 0.0;
    minimumPivot = 3.402823466e38;
    for (uint column = 0u; column < dimension; ++column)
    {
        float largest = 0.0;
        uint pivotRow = column;
        uint pivotColumn = column;
        for (uint row = column; row < dimension; ++row)
            for (uint entry = column; entry < dimension; ++entry)
                if (abs(matrix[row * 20u + entry]) > largest)
                {
                    largest = abs(matrix[row * 20u + entry]);
                    pivotRow = row;
                    pivotColumn = entry;
                }
        if (largest == 0.0 || isnan(largest) || isinf(largest))
            return false;
        maximumPivot = max(maximumPivot, largest);
        minimumPivot = min(minimumPivot, largest);
        for (uint entry = 0u; entry < dimension; ++entry)
        {
            float value = matrix[column * 20u + entry];
            matrix[column * 20u + entry] = matrix[pivotRow * 20u + entry];
            matrix[pivotRow * 20u + entry] = value;
        }
        for (uint entry = 0u; entry < 11u; ++entry)
        {
            float value = right[column * 11u + entry];
            right[column * 11u + entry] = right[pivotRow * 11u + entry];
            right[pivotRow * 11u + entry] = value;
        }
        for (uint row = 0u; row < dimension; ++row)
        {
            float value = matrix[row * 20u + column];
            matrix[row * 20u + column] = matrix[row * 20u + pivotColumn];
            matrix[row * 20u + pivotColumn] = value;
        }
        uint original = permutation[column];
        permutation[column] = permutation[pivotColumn];
        permutation[pivotColumn] = original;
        for (uint row = column + 1u; row < dimension; ++row)
        {
            float factor = matrix[row * 20u + column] / matrix[column * 20u + column];
            for (uint entry = column; entry < dimension; ++entry)
                matrix[row * 20u + entry] -= factor * matrix[column * 20u + entry];
            for (uint entry = 0u; entry < 11u; ++entry)
                right[row * 11u + entry] -= factor * right[column * 11u + entry];
        }
    }
    if (minimumPivot <= 64.0 * float(dimension) * 1.1920928955078125e-7 * maximumPivot)
        return false;
    float solution[220];
    for (int row = int(dimension) - 1; row >= 0; --row)
        for (uint entry = 0u; entry < 11u; ++entry)
        {
            float value = right[uint(row) * 11u + entry];
            for (uint column = uint(row + 1); column < dimension; ++column)
                value -= matrix[uint(row) * 20u + column] * solution[column * 11u + entry];
            solution[uint(row) * 11u + entry] = value / matrix[uint(row) * 20u + uint(row)];
        }
    for (uint row = 0u; row < dimension; ++row)
        for (uint entry = 0u; entry < 11u; ++entry)
            right[permutation[row] * 11u + entry] =
                columnScale[permutation[row]] * solution[row * 11u + entry];
    return true;
}

uint RodKktSolveBand32(uint count, out float originalResidual,
    out uint supernodeCount, out float smallestPivotRatio)
{
    originalResidual = 0.0;
    supernodeCount = 0u;
    smallestPivotRatio = 1.0;
    if (count == 0u || count > 32u)
        return 1u;
    for (uint point = 0u; point < count;)
    {
        RodKkt10Matrix diagonal = RodKktLoadBand(point, 1u);
        RodKkt10Vector residual = RodKktLoadResidual(point);
        RodKkt10Matrix upper = RodKktLoadBand(point, 2u);
        for (uint row = 0u; row < 10u; ++row)
            residual.values[row] = -residual.values[row];
        if (point > 0u)
        {
            RodKkt10Matrix lower = RodKktLoadBand(point, 0u);
            for (uint row = 0u; row < 10u; ++row)
                for (uint inner = 0u; inner < 10u; ++inner)
                {
                    residual.values[row] -= lower.values[row * 10u + inner] *
                        rodKktFactorRight[point - 1u].values[inner];
                    for (uint column = 0u; column < 10u; ++column)
                        diagonal.values[row * 10u + column] -= lower.values[row * 10u + inner] *
                            rodKktFactorUpper[point - 1u].values[inner * 10u + column];
                }
        }
        float matrix[400];
        float right[220];
        for (uint row = 0u; row < 10u; ++row)
        {
            for (uint column = 0u; column < 10u; ++column)
            {
                matrix[row * 20u + column] = diagonal.values[row * 10u + column];
                right[row * 11u + column] = upper.values[row * 10u + column];
            }
            right[row * 11u + 10u] = residual.values[row];
        }
        float maximumPivot, minimumPivot;
        uint nodes = 1u;
        bool success = RodKktEquilibratedPivot(10u, matrix, right, maximumPivot, minimumPivot);
        if (!success)
        {
            if (point + 1u >= count)
                return 2u;
            nodes = 2u;
            ++supernodeCount;
            RodKkt10Matrix nextLower = RodKktLoadBand(point + 1u, 0u);
            RodKkt10Matrix nextDiagonal = RodKktLoadBand(point + 1u, 1u);
            RodKkt10Matrix nextUpper = RodKktLoadBand(point + 1u, 2u);
            RodKkt10Vector nextResidual = RodKktLoadResidual(point + 1u);
            for (uint row = 0u; row < 20u; ++row)
            {
                for (uint column = 0u; column < 20u; ++column)
                    matrix[row * 20u + column] = row < 10u ?
                        (column < 10u ? diagonal.values[row * 10u + column] :
                            upper.values[row * 10u + column - 10u]) :
                        (column < 10u ? nextLower.values[(row - 10u) * 10u + column] :
                            nextDiagonal.values[(row - 10u) * 10u + column - 10u]);
                for (uint column = 0u; column < 10u; ++column)
                    right[row * 11u + column] = row < 10u ? 0.0 :
                        nextUpper.values[(row - 10u) * 10u + column];
                right[row * 11u + 10u] = row < 10u ? residual.values[row] :
                    -nextResidual.values[row - 10u];
            }
            success = RodKktEquilibratedPivot(20u, matrix, right, maximumPivot, minimumPivot);
            if (!success)
                return 3u;
        }
        smallestPivotRatio = min(smallestPivotRatio, minimumPivot / maximumPivot);
        for (uint node = 0u; node < nodes; ++node)
        {
            rodKktNextPoint[point + node] = point + nodes;
            for (uint row = 0u; row < 10u; ++row)
            {
                rodKktFactorRight[point + node].values[row] = right[(node * 10u + row) * 11u + 10u];
                for (uint column = 0u; column < 10u; ++column)
                    rodKktFactorUpper[point + node].values[row * 10u + column] =
                        right[(node * 10u + row) * 11u + column];
            }
        }
        point += nodes;
    }
    for (int point = int(count) - 1; point >= 0; --point)
        for (uint row = 0u; row < 10u; ++row)
        {
            float value = rodKktFactorRight[point].values[row];
            uint next = rodKktNextPoint[point];
            if (next < count)
                for (uint column = 0u; column < 10u; ++column)
                    value -= rodKktFactorUpper[point].values[row * 10u + column] *
                        rodKktDirection[next].values[column];
            rodKktDirection[point].values[row] = value;
        }
    for (uint point = 0u; point < count; ++point)
    {
        RodKkt10Vector value = RodKktLoadResidual(point);
        for (uint band = 0u; band < 3u; ++band)
        {
            if ((band == 0u && point == 0u) || (band == 2u && point + 1u == count))
                continue;
            uint source = band == 0u ? point - 1u : band == 2u ? point + 1u : point;
            RodKkt10Matrix matrix = RodKktLoadBand(point, band);
            for (uint row = 0u; row < 10u; ++row)
                for (uint column = 0u; column < 10u; ++column)
                    value.values[row] += matrix.values[row * 10u + column] *
                        rodKktDirection[source].values[column];
        }
        for (uint row = 0u; row < 10u; ++row)
        {
            if (isnan(value.values[row]) || isinf(value.values[row]))
                return 4u;
            originalResidual = max(originalResidual, abs(value.values[row]));
        }
    }
    return originalResidual <= 1.0e-5 ? 0u : 5u;
}

#endif
