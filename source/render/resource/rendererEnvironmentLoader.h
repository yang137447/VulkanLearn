#pragma once

#include <nlohmann/json.hpp>

class PipelineFactory;

namespace VL
{
class RendererBackendVulkan;
struct RendererResourceLoadContext;

// Creates global and environment render textures for the current Vulkan
// renderer resource cache. The resulting Texture objects are registered by
// binding name so descriptor code can stay independent from scene loading.
class RendererEnvironmentLoader
{
public:
    RendererEnvironmentLoader(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        RendererResourceLoadContext& loadContext);

    void LoadGlobalResources() const;
    void LoadEnvironmentObject(const nlohmann::basic_json<>& node) const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
    RendererResourceLoadContext& loadContext;
};

} // namespace VL
