#pragma once

#include <string>

#include "core/runtimeResult.h"
#include "world/worldManager.h"

namespace VL
{

class RendererResourceLoadCoordinator;

enum class WorldTransitionState
{
    Idle,
    Validating,
    Loading,
    Active,
    Failed
};

struct WorldTransitionResult
{
    WorldHandle world;
    WorldTransitionState finalState = WorldTransitionState::Idle;
};

// Coordinates the world-load transaction boundary. Renderer resource loaders
// prepare GPU-facing data first; WorldBuilder then assembles the gameplay-facing
// World from the validated plan and cache.
class WorldTransitionCoordinator
{
public:
    WorldTransitionCoordinator(
        WorldManager& worldManager,
        RendererResourceLoadCoordinator& resourceLoadCoordinator,
        float initialCameraAspectRatio);

    RuntimeResult<WorldTransitionResult> LoadInitialWorld(const std::string& scenePath);
    RuntimeResult<WorldTransitionResult> RequestWorldLoad(const std::string& scenePath);

private:
    RuntimeResult<WorldTransitionResult> LoadWorldThroughResourceCoordinator(const std::string& scenePath);

    WorldManager& worldManager;
    RendererResourceLoadCoordinator& resourceLoadCoordinator;
    float initialCameraAspectRatio = 1.0f;
    WorldTransitionState state = WorldTransitionState::Idle;
};

} // namespace VL
