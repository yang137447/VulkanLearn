#pragma once

#include "render/camera/cameraExposure.h"
#include "render/offline/offlineRenderCoordinator.h"
#include "world/worldSnapshot.h"

namespace VL
{

// Offline display uses the film's resolved exposure scale when the manifest
// carries one; otherwise it falls back to the frozen camera exposure.
inline float ResolveOfflineDisplayExposure(
    const OfflinePublishedResult* result,
    float manualExposure,
    const CameraSnapshot& fallbackCamera)
{
    if (result &&
        result->manifest.contains("camera") &&
        result->manifest["camera"].is_object() &&
        result->manifest["camera"].contains("exposureScale") &&
        result->manifest["camera"]["exposureScale"].is_number())
    {
        return manualExposure *
            result->manifest["camera"]["exposureScale"].get<float>();
    }
    return manualExposure * ExposureScale(fallbackCamera.exposure);
}

} // namespace VL
