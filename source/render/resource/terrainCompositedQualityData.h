#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace VL
{
namespace TerrainQualityAbi
{
constexpr uint32_t Magic = 0x31515654;
constexpr uint32_t Version = 2;
constexpr uint32_t Extent = 128;
constexpr uint32_t Capacity = Extent * Extent;
constexpr uint32_t Slots = 512;
constexpr uint32_t Surfaces = 16;
constexpr uint32_t HeaderWords = 64;
constexpr uint32_t SampleWord = HeaderWords;
constexpr uint32_t ResultWord = SampleWord + Capacity * 8;
constexpr uint32_t OwnerWord = ResultWord + Capacity * 16;
constexpr uint32_t SurfaceWord = OwnerWord + Slots * 16;
constexpr uint32_t DuplicateWord = SurfaceWord + Surfaces * 16;
constexpr uint32_t DuplicateCapacity = 256;
constexpr uint32_t DuplicateStrideWords = 12;
constexpr uint32_t UsedBytes =
    (DuplicateWord + DuplicateCapacity * DuplicateStrideWords) * 4;
constexpr uint32_t Bytes = UsedBytes;
constexpr uint64_t CaptureMetadataGpuBytes = 128 * 1024;
constexpr uint64_t LoadingMetadataEntryGpuBytes = 128 * 1024;
constexpr uint64_t LoadingMetadataEntryCpuBytes = 32 * 1024;
constexpr uint64_t FrameMetadataGpuBytes =
    CaptureMetadataGpuBytes + 8 * LoadingMetadataEntryGpuBytes;
static_assert(Bytes == 1619200, "Quality fixed payload with bounded duplicate witnesses");

enum Header : uint32_t
{
    MagicWord, VersionWord, CapacityWord, VisibleCount, DuplicateCount,
    InvalidCount, ResolvedCount, UninitializedDraws,
    FrameLow, FrameHigh, PoseLow, PoseHigh, GenerationLow, GenerationHigh,
    EpochLow, EpochHigh, RenderWidth, RenderHeight, SurfaceCount, SequenceLow,
    SequenceHigh, DroppedTerrainSamples, OverflowCount,
    DuplicateWitnessCount, DuplicateWitnessOverflow
};

struct Sample
{
    float uv[2];
    float majorUv[2];
    float targetMip;
    float effectiveMipBias;
    uint32_t surfaceAndTaps;
    uint32_t claims;
};

struct Owner
{
    uint32_t valid;
    uint32_t surface;
    uint32_t mip;
    uint32_t xy;
    uint32_t generationLow;
    uint32_t generationHigh;
    uint32_t revisionLow;
    uint32_t revisionHigh;
    std::array<uint32_t, 8> identity;
};

struct Surface
{
    uint32_t index;
    uint32_t width;
    uint32_t height;
    uint32_t maxMipAndRepeat;
    uint32_t generationLow;
    uint32_t generationHigh;
    uint32_t revisionLow;
    uint32_t revisionHigh;
    std::array<uint32_t, 8> identity;
};

struct Counts
{
    uint32_t visibleSamples = 0;
    uint32_t validAtTargetPlusBias = 0;
    uint32_t unresolved = 0;
    uint32_t incorrectIdentity = 0;
    uint32_t rootOnlySamples = 0;
    uint32_t uninitializedDraws = 0;
    uint32_t effectiveMipBias = 0;
    bool complete = false;

    bool MeetsQualityThreshold() const
    {
        return complete && visibleSamples && !unresolved && !incorrectIdentity &&
            !rootOnlySamples && !uninitializedDraws && effectiveMipBias <= 1 &&
            uint64_t(validAtTargetPlusBias) * 100 >= uint64_t(visibleSamples) * 99;
    }
};

static_assert(sizeof(Sample) == 32 && sizeof(Owner) == 64 && sizeof(Surface) == 64,
    "Quality std430 ABI");

inline uint64_t Join(uint32_t low, uint32_t high)
{
    return uint64_t(low) | (uint64_t(high) << 32);
}

inline Counts Recompute(const uint32_t* words, size_t bytes)
{
    if (!words || bytes != Bytes || words[MagicWord] != Magic ||
        words[VersionWord] != Version || words[CapacityWord] != Capacity ||
        !words[SurfaceCount] || words[SurfaceCount] > Surfaces ||
        !Join(words[EpochLow], words[EpochHigh]) ||
        !words[RenderWidth] || !words[RenderHeight])
        throw std::runtime_error("Invalid quality binary ABI");
    Counts counts;
    counts.uninitializedDraws = words[UninitializedDraws];
    bool malformed = false;
    for (uint32_t pixel = 0; pixel < Capacity; ++pixel)
    {
        Sample sample;
        std::memcpy(&sample, words + SampleWord + pixel * 8, sizeof(sample));
        if (!sample.claims)
            continue;
        ++counts.visibleSamples;
        const uint32_t surfaceIndex = sample.surfaceAndTaps & 65535u;
        const uint32_t taps = sample.surfaceAndTaps >> 16;
        if (sample.claims != 1 || surfaceIndex >= words[SurfaceCount] ||
            !taps || taps > 8 || !std::isfinite(sample.targetMip) ||
            !std::isfinite(sample.effectiveMipBias) || sample.effectiveMipBias < 0 ||
            sample.effectiveMipBias > 1 ||
            std::floor(sample.effectiveMipBias) != sample.effectiveMipBias ||
            !std::isfinite(sample.uv[0]) || !std::isfinite(sample.uv[1]) ||
            !std::isfinite(sample.majorUv[0]) || !std::isfinite(sample.majorUv[1]))
        {
            malformed = true;
            continue;
        }
        Surface surface;
        std::memcpy(&surface, words + SurfaceWord + surfaceIndex * 16, sizeof(surface));
        const uint32_t maximum = surface.maxMipAndRepeat & 255u;
        uint32_t layoutMaximum = 0;
        for (uint32_t dimension = std::max(surface.width, surface.height);
            dimension > 1; dimension >>= 1)
            ++layoutMaximum;
        if (surface.index != surfaceIndex || maximum > 13 || maximum != layoutMaximum ||
            (surface.maxMipAndRepeat & ~511u) || !surface.width ||
            !surface.height || surface.width > 8192 || surface.height > 8192 ||
            Join(surface.generationLow, surface.generationHigh) !=
                Join(words[GenerationLow], words[GenerationHigh]) ||
            sample.targetMip < 0 || sample.targetMip > float(maximum))
        {
            malformed = true;
            continue;
        }
        const auto bias = uint32_t(sample.effectiveMipBias);
        counts.effectiveMipBias = std::max(counts.effectiveMipBias, bias);
        const uint32_t low = std::min(maximum, uint32_t(sample.targetMip) + bias);
        bool missing = false;
        bool incorrect = false;
        bool good = true;
        bool root = sample.targetMip < 12.f;
        for (uint32_t tap = 0; tap < taps; ++tap)
            for (uint32_t branch = 0; branch < 2; ++branch)
            {
                const auto packed = words[ResultWord + pixel * 16 + tap * 2 + branch];
                const uint32_t entry = packed & 1023u;
                const uint32_t mip = (packed >> 10) & 15u;
                const uint32_t pageX = (packed >> 14) & 63u;
                const uint32_t pageY = (packed >> 20) & 63u;
                const uint32_t requested = std::min(maximum, low + branch);
                if (!(packed & (1u << 26)) || (packed >> 28) || mip < requested || mip > maximum ||
                    pageX >= (std::max(1u, surface.width >> mip) + 127) / 128 ||
                    pageY >= (std::max(1u, surface.height >> mip) + 127) / 128)
                {
                    malformed = true;
                    missing = true;
                    good = root = false;
                    continue;
                }
                if (!entry || entry > Slots)
                {
                    malformed |= entry > Slots;
                    missing = true;
                    good = root = false;
                    continue;
                }
                Owner owner;
                std::memcpy(&owner, words + OwnerWord + (entry - 1) * 16, sizeof(owner));
                if (owner.valid > 1)
                    malformed = true;
                if (!owner.valid)
                {
                    missing = true;
                    good = root = false;
                    continue;
                }
                const bool matches = owner.surface == surfaceIndex && owner.mip == mip &&
                    owner.xy == (pageX | (pageY << 16)) &&
                    owner.generationLow == surface.generationLow &&
                    owner.generationHigh == surface.generationHigh &&
                    owner.revisionLow == surface.revisionLow &&
                    owner.revisionHigh == surface.revisionHigh &&
                    owner.identity == surface.identity;
                incorrect |= !matches;
                good &= matches && mip <= std::min(maximum, requested - bias + 1);
                root &= mip == maximum;
            }
        counts.unresolved += missing;
        counts.incorrectIdentity += incorrect;
        counts.rootOnlySamples += root;
        counts.validAtTargetPlusBias += good && !missing && !incorrect;
    }
    counts.complete = !malformed && counts.visibleSamples == words[VisibleCount] &&
        counts.visibleSamples == words[ResolvedCount] && !words[DuplicateCount] &&
        !words[InvalidCount] && !words[DroppedTerrainSamples] && !words[OverflowCount] &&
        counts.visibleSamples <= Capacity;
    return counts;
}
}
}
