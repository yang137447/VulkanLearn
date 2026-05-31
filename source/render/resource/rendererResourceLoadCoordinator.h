#pragma once

#include <cstdint>
#include <string>

class PipelineFactory;

namespace VL
{
class RendererBackendVulkan;
struct WorldBuildPlan;

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
    void LoadRendererResources(const WorldBuildPlan& worldBuildPlan, uint64_t ownerGeneration);

private:
    RendererResourceLoadCoordinator();

    PipelineFactory* pipelineFactory = nullptr;
    RendererBackendVulkan* rendererBackend = nullptr;
};

} // namespace VL
