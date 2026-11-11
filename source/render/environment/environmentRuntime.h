#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <vulkan/vulkan.hpp>

#include "render/environment/environmentGpuTimer.h"
#include "render/environment/environmentUpdateDiagnostics.h"
#include "render/environment/environmentUpdateScheduler.h"
#include "render/environment/environmentUpdateState.h"

class Texture;

namespace VL
{

class RendererBackendVulkan;
class RendererResourceCache;
class ProceduralSkyCubeGenerator;
class EnvironmentIblBaker;
struct RenderScene;

// Owns the environment-derived resource schedule: generation state, per-frame
// GPU work plan, GPU timing and the diagnostics mirror published to the UI.
// The sky generator and IBL baker stay owned by RenderSystem; this module only
// sequences them and binds their outputs into the World texture cache.
class EnvironmentRuntime
{
public:
    void Initialize(RendererBackendVulkan& rendererBackend);
    void Shutdown(RendererBackendVulkan& rendererBackend);
    // Drop pending CPU work and the frozen source cube. Kept separate from
    // Shutdown() so renderer teardown keeps its original ordering.
    void ResetPendingWork();

    // World replacement / first scene initialize: drop pending work and the
    // frozen source cube so the next generation starts from the new snapshot.
    void Reset();

    EnvironmentUpdateDiagnostics GetDiagnostics() const;
    void RefreshDiagnostics();

    std::shared_ptr<Texture> GetEnvironmentCube(
        const RenderScene& renderScene,
        const RendererResourceCache& resourceCache,
        const ProceduralSkyCubeGenerator& proceduralSkyCubeGenerator) const;
    void PrepareResources(
        const RenderScene& renderScene,
        RendererResourceCache& resourceCache,
        const ProceduralSkyCubeGenerator& proceduralSkyCubeGenerator,
        const EnvironmentIblBaker& environmentIblBaker) const;

    void RecordIbl(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex,
        const RenderScene& renderScene,
        const RendererResourceCache& resourceCache,
        ProceduralSkyCubeGenerator& proceduralSkyCubeGenerator,
        EnvironmentIblBaker& environmentIblBaker);

private:
    EnvironmentGpuTimer environmentGpuTimer;
    EnvironmentUpdateScheduler environmentUpdateScheduler;
    EnvironmentUpdateState environmentUpdateState;
    std::shared_ptr<Texture> environmentUpdateSourceCube;
    mutable std::mutex environmentDiagnosticsMutex;
    EnvironmentUpdateDiagnostics environmentDiagnostics;
};

} // namespace VL
