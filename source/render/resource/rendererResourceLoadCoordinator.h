#pragma once

#include <string>
#include <vector>

#include "scene/sceneAssetTypes.h"

class PipelineFactory;

namespace VL
{
class RendererBackendVulkan;
struct RendererResourceLoadContext;
class EyeComputeReloadParticipant;
class ClothComputeReloadParticipant;
struct WorldBuildPlan;

struct RendererWorldResourceLoadResult
{
    std::vector<MeshObjectBuildPlan> meshObjectPlans;
    std::vector<SpeedTreeWindProfile> speedTreeWindProfiles;
};

// Coordinates renderer resource loading for one validated WorldBuildPlan.
// WorldLoader/WorldBuilder own scene IO, validation, camera, lights, and World
// identity; concrete mesh/material/environment creation stays in the renderer
// loaders and is registered in RendererResourceCache.
class RendererResourceLoadCoordinator
{
public:
    static RendererResourceLoadCoordinator& GetInstance()
    {
        static RendererResourceLoadCoordinator instance;
        return instance;
    }

    void SetPipelineFactory(PipelineFactory* pipelineFactory);
    void SetRendererBackend(RendererBackendVulkan* rendererBackend);
    void SetEyeComputeReloadParticipant(
        EyeComputeReloadParticipant* participant);
    void SetClothComputeReloadParticipant(
        ClothComputeReloadParticipant* participant);
    RendererWorldResourceLoadResult LoadRendererResources(
        const WorldBuildPlan& worldBuildPlan,
        RendererResourceLoadContext& loadContext);

private:
    RendererResourceLoadCoordinator();

    PipelineFactory* pipelineFactory = nullptr;
    RendererBackendVulkan* rendererBackend = nullptr;
    EyeComputeReloadParticipant* eyeComputeReloadParticipant = nullptr;
    ClothComputeReloadParticipant* clothComputeReloadParticipant = nullptr;
};

} // namespace VL