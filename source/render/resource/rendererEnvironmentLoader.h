#pragma once

#include <nlohmann/json.hpp>

class PipelineFactory;

namespace VL
{
class RendererBackendVulkan;

// Creates global and environment render textures during the transition phase.
// The resulting Texture objects are registered in RendererResourceCache by
// binding name so descriptor code can stay independent from scene loading.
class RendererEnvironmentLoader
{
public:
    RendererEnvironmentLoader(PipelineFactory& pipelineFactory, RendererBackendVulkan& rendererBackend);

    void LoadGlobalResources() const;
    void LoadEnvironmentObject(const nlohmann::basic_json<>& node) const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
};

} // namespace VL
