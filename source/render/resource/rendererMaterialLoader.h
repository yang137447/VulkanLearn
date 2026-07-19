#pragma once

#include <memory>
#include <string_view>

class MaterialInstance;
class PipelineFactory;
struct Renderpass;

namespace VL
{
class RendererBackendVulkan;

// Creates current Material/MaterialInstance GPU objects and records them in
// RendererResourceCache. It owns material JSON resolution and texture binding
// setup; scene/world assembly stays outside this loader.
class RendererMaterialLoader
{
public:
    RendererMaterialLoader(PipelineFactory& pipelineFactory, RendererBackendVulkan& rendererBackend);

    void LoadPassMaterials() const;

    std::shared_ptr<MaterialInstance> LoadMaterialInstance(
        std::string_view materialInstancePath,
        Renderpass& renderPass) const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
};

} // namespace VL
