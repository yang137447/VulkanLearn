#include "render/eye/eyePerformanceBudget.h"

#include <limits>

namespace VL
{

size_t CalculateEyeLutMemoryBytes(
    const EyePerformanceBudget& budget) noexcept
{
    const size_t texelCount = static_cast<size_t>(budget.lutWidth) *
        budget.lutHeight * budget.lutLayers;
    return texelCount * budget.lutChannels * budget.bytesPerChannel;
}

bool ValidateEyePerformanceFrame(
    const EyePerformanceBudget& budget,
    const EyePerformanceFrameStats& stats,
    std::string* error) noexcept
{
    const size_t maximumSamples =
        stats.eyeDrawCount * budget.maximumEyeLutSamplesPerPixel;
    const bool valid =
        stats.eyeDrawCount <= budget.maximumEyeDrawsPerFrame &&
        stats.eyeDescriptorBindCount <= budget.maximumEyeDescriptorBindsPerFrame &&
        stats.eyeLutSampleCount <= maximumSamples;
    if (!valid && error != nullptr)
    {
        *error = "Eye frame exceeded draw/descriptor/LUT sample budget";
    }
    return valid;
}

} // namespace VL
