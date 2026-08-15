#pragma once

#include <memory>
#include <string_view>

class MaterialInstance;
class PipelineFactory;
struct Renderpass;

namespace VL
{
class RendererBackendVulkan;
struct RendererResourceLoadContext;

// Creates current Material/MaterialInstance GPU objects and records them in
// RendererResourceCache. It owns material JSON resolution and texture binding
// setup; scene/world assembly stays outside this loader.
class RendererMaterialLoader
{
public:
    RendererMaterialLoader(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        RendererResourceLoadContext& loadContext);

    void LoadPassMaterials() const;

    // Scene surface materials select their pass from the effective RenderMode:
    // opaque variants use geometry, transparent variants use forwardTransparent.
    std::shared_ptr<MaterialInstance> LoadSceneMaterialInstance(
        std::string_view materialInstancePath) const;

    std::shared_ptr<MaterialInstance> LoadMaterialInstance(
        std::string_view materialInstancePath,
        Renderpass& renderPass) const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
    RendererResourceLoadContext& loadContext;
};

} // namespace VL
