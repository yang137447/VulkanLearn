#pragma once

#include <cstdlib>
#include <atomic>
#include <stdexcept>
#include <string_view>
#if __has_include("render/diagnostics/terrainCaptureHooks.h")
#include "render/diagnostics/terrainCaptureHooks.h"
#define VL_TES_SOURCE_OWNED_HOOKS 1
#endif

namespace VL::TerrainTesDiagnostic
{
inline std::atomic<bool> logicalFeatureEnabled{false};

inline void DeviceFeatureEnabled(bool enabled)
{
#if defined(VL_TES_SOURCE_OWNED_HOOKS)
    PublishTerrainCaptureDeviceFeature(enabled);
#endif
    logicalFeatureEnabled.store(enabled, std::memory_order_release);
}

inline bool DeviceFeatureEnabled()
{
#if defined(VL_TES_SOURCE_OWNED_HOOKS)
    return TerrainCaptureDeviceFeatureEnabled();
#else
    return logicalFeatureEnabled.load(std::memory_order_acquire);
#endif
}

inline bool Requested()
{
#if defined(VL_TES_SOURCE_OWNED_HOOKS)
    const auto hooks = AcquireTerrainCaptureHooks();
    return hooks && hooks->RequiresTesVertexStores();
#else
    const char* value = std::getenv("VULKANLEARN_TERRAIN_TES_CAPTURE");
    if (!value || std::string_view(value) == "0")
        return false;
    if (std::string_view(value) != "1")
        throw std::runtime_error("VULKANLEARN_TERRAIN_TES_CAPTURE accepts only 0 or 1");
    return true;
#endif
}
}
