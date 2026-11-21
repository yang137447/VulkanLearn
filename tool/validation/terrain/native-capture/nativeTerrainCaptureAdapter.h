#pragma once

#include "render/diagnostics/terrainCaptureHooks.h"
#include <filesystem>

namespace VL
{
std::shared_ptr<ITerrainCaptureHooks> LoadNativeTerrainCaptureAdapter(
    const std::filesystem::path& explicitTestCase);
}
