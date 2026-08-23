#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "render/eye/eyeAssets.h"

namespace VL
{

struct EyePerformanceBudget
{
    uint32_t lutWidth = EyeCausticLutWidth;
    uint32_t lutHeight = EyeCausticLutHeight;
    uint32_t lutLayers = EyeCausticLutLayerCount;
    uint32_t lutChannels = EyeCausticLutChannelCount;
    uint32_t bytesPerChannel = 2;
    size_t maximumEyeDrawsPerFrame = 4;
    size_t maximumEyeDescriptorBindsPerFrame = 8;
    size_t maximumEyeLutSamplesPerPixel = 2;
};

struct EyePerformanceFrameStats
{
    size_t eyeDrawCount = 0;
    size_t eyeDescriptorBindCount = 0;
    size_t eyeLutSampleCount = 0;
    double innerGpuTimeMicroseconds = 0.0;
    double corneaGpuTimeMicroseconds = 0.0;
    double deferredGpuTimeMicroseconds = 0.0;
    double sssFilterGpuTimeMicroseconds = 0.0;
};

size_t CalculateEyeLutMemoryBytes(
    const EyePerformanceBudget& budget) noexcept;

bool ValidateEyePerformanceFrame(
    const EyePerformanceBudget& budget,
    const EyePerformanceFrameStats& stats,
    std::string* error = nullptr) noexcept;

} // namespace VL
