#pragma once
#include "render/diagnostics/terrainCaptureHooks.h"
#include "terrainLitReplaySession.h"
#include <functional>

namespace VL
{
using TerrainLitSessionFactory = std::function<std::shared_ptr<TerrainLitReplaySession>(
    PassRuntimeContext&, const TerrainCaptureFrameInfo&, std::shared_ptr<TerrainRvtBudgetOwner>)>;

TerrainLitSessionFactory LoadNativeTerrainLitCase(const std::filesystem::path& explicitCase);

std::shared_ptr<ITerrainCaptureHooks> AttachNativeTerrainLitReplay(
    std::shared_ptr<ITerrainCaptureHooks> actualTesCapture, TerrainLitSessionFactory prepareBeforeDraw);
}
