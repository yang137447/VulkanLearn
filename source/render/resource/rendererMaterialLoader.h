#pragma once

#include <memory>
#include <string_view>

#include <vulkan/vulkan.hpp>

#include "pipeline/graphicsPipelineBuilder.h"

class MaterialInstance;
class PipelineFactory;

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
        vk::SampleCountFlagBits sampleCount,
        std::string_view passName = "geometry",
        const GraphicsPipelineStateDesc& pipelineStateDesc = {}) const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
};

} // namespace VL
