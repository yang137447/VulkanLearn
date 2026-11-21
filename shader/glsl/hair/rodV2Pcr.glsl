#ifndef VL_ROD_V2_PCR
#define VL_ROD_V2_PCR

#include "rodV2Blocks.glsl"

shared vec4 rodV2BandStorage[1728];
shared vec3 rodV2RightLinear[64];
shared vec3 rodV2RightAngular[64];
shared uint rodV2LinearStatus;

uint RodV2BandOffset(uint bank, uint lane, uint band)
{
    return bank * 864u + lane * 27u + band * 9u;
}

void RodV2StoreBand(uint bank, uint lane, uint band, RodV2Matrix matrix)
{
    uint offset = RodV2BandOffset(bank, lane, band);
    for (uint entry = 0u; entry < 36u; ++entry)
        rodV2BandStorage[offset + entry / 4u][entry % 4u] =
            RodV2Entry(matrix, entry / 6u, entry % 6u);
}

RodV2Matrix RodV2LoadBand(uint bank, uint lane, uint band)
{
    RodV2Matrix matrix = RodV2Zero();
    uint offset = RodV2BandOffset(bank, lane, band);
    for (uint entry = 0u; entry < 36u; ++entry)
        RodV2Set(matrix, entry / 6u, entry % 6u,
            rodV2BandStorage[offset + entry / 4u][entry % 4u]);
    return matrix;
}

RodV2Vector RodV2LoadRight(uint bank, uint lane)
{
    return RodV2Vector(rodV2RightLinear[bank * 32u + lane],
        rodV2RightAngular[bank * 32u + lane]);
}

RodV2Vector RodV2Pcr(uint lane, RodV2Matrix lower, RodV2Matrix diagonal,
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
    uint bank = 0u;
    for (uint stride = 1u; stride < 32u; stride <<= 1u)
    {
        lower = RodV2Zero();
        upper = RodV2Zero();
        diagonal = RodV2LoadBand(bank, lane, 1u);
        right = RodV2LoadRight(bank, lane);
        if (lane >= stride)
        {
            uint neighbor = lane - stride;
            RodV2Matrix inverseMatrix;
            if (!RodV2Inverse(RodV2LoadBand(bank, neighbor, 1u), inverseMatrix))
            {
                atomicMax(rodV2LinearStatus, 4u);
                inverseMatrix = RodV2Zero();
            }
            RodV2Matrix multiplier = RodV2Negative(RodV2Multiply(
                RodV2LoadBand(bank, lane, 0u), inverseMatrix));
            lower = RodV2Multiply(multiplier, RodV2LoadBand(bank, neighbor, 0u));
            diagonal = RodV2Add(diagonal, RodV2Multiply(multiplier,
                RodV2LoadBand(bank, neighbor, 2u)));
            right = RodV2Add(right, RodV2Multiply(multiplier, RodV2LoadRight(bank, neighbor)));
        }
        if (lane + stride < 32u)
        {
            uint neighbor = lane + stride;
            RodV2Matrix inverseMatrix;
            if (!RodV2Inverse(RodV2LoadBand(bank, neighbor, 1u), inverseMatrix))
            {
                atomicMax(rodV2LinearStatus, 4u);
                inverseMatrix = RodV2Zero();
            }
            RodV2Matrix multiplier = RodV2Negative(RodV2Multiply(
                RodV2LoadBand(bank, lane, 2u), inverseMatrix));
            upper = RodV2Multiply(multiplier, RodV2LoadBand(bank, neighbor, 2u));
            diagonal = RodV2Add(diagonal, RodV2Multiply(multiplier,
                RodV2LoadBand(bank, neighbor, 0u)));
            right = RodV2Add(right, RodV2Multiply(multiplier, RodV2LoadRight(bank, neighbor)));
        }
        barrier();
        uint nextBank = 1u - bank;
        RodV2StoreBand(nextBank, lane, 0u, lower);
        RodV2StoreBand(nextBank, lane, 1u, diagonal);
        RodV2StoreBand(nextBank, lane, 2u, upper);
        rodV2RightLinear[nextBank * 32u + lane] = right.linear;
        rodV2RightAngular[nextBank * 32u + lane] = right.angular;
        barrier();
        bank = nextBank;
    }
    RodV2Matrix inverseMatrix;
    if (!RodV2Inverse(RodV2LoadBand(bank, lane, 1u), inverseMatrix))
    {
        atomicMax(rodV2LinearStatus, 4u);
        inverseMatrix = RodV2Zero();
    }
    barrier();
    return RodV2Multiply(inverseMatrix, RodV2LoadRight(bank, lane));
}

#endif
