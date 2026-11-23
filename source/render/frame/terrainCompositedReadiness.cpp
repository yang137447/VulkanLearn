#include "render/frame/terrainCompositedReadiness.h"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace VL
{
namespace
{
std::mutex invalidationMutex;
TerrainQualityInvalidation invalidation;
}

double TerrainCompositedQualityMonotonicMs()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void RequestTerrainCompositedQualityRevalidation(uint64_t worldGeneration)
{
    if (!worldGeneration)
        throw std::invalid_argument("Quality revalidation requires the current World generation");
    TerrainQualityInvalidation event;
    {
        std::lock_guard<std::mutex> lock(invalidationMutex);
        invalidation.worldGeneration = worldGeneration;
        ++invalidation.sequence;
        invalidation.arrivedMonotonicMs = TerrainCompositedQualityMonotonicMs();
        event = invalidation;
    }
    std::ostringstream encoded;
    encoded << std::setprecision(17)
        << "[RVT_QUALITY_EVENT] {\"event\":\"pose_arrived\",\"worldGeneration\":"
        << event.worldGeneration << ",\"sequence\":" << event.sequence
        << ",\"arrivedMonotonicMs\":" << event.arrivedMonotonicMs << "}\n";
    std::cout << encoded.str();
}

TerrainQualityInvalidation TerrainCompositedQualityInvalidation()
{
    std::lock_guard<std::mutex> lock(invalidationMutex);
    return invalidation;
}
}
