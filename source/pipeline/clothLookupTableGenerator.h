#pragma once

#include <memory>
#include <string>

class ComputePipeline;
class PipelineFactory;
class Texture;

namespace VL
{

class RendererBackendVulkan;

struct ClothLookupTableGenerationResult
{
    std::shared_ptr<Texture> directionalAlbedoLutTexture;
};

class ClothLookupTableGenerator
{
public:
    static ClothLookupTableGenerationResult Generate(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        const std::string& sourceDigest);

    static ClothLookupTableGenerationResult GenerateWithPipeline(
        const std::shared_ptr<ComputePipeline>& computePipeline,
        RendererBackendVulkan& rendererBackend,
        const std::string& sourceDigest);
};

} // namespace VL
