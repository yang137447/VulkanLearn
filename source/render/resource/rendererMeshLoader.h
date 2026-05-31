#pragma once

#include <nlohmann/json.hpp>

class PipelineFactory;
struct MeshAssetLoadRequest;

namespace VL
{
class RendererBackendVulkan;

// Creates renderable sections and current SceneObject wrappers for mesh scene
// nodes. Material creation is delegated to RendererMaterialLoader, while the
// created CPU/GPU resources are registered in RendererResourceCache.
class RendererMeshLoader
{
public:
    RendererMeshLoader(PipelineFactory& pipelineFactory, RendererBackendVulkan& rendererBackend);

    void LoadMeshObject(
        const nlohmann::basic_json<>& node,
        const MeshAssetLoadRequest& meshLoadRequest) const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
};

} // namespace VL
