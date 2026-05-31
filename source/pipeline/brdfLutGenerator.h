#pragma once

#include <memory>

class Texture;
class PipelineFactory;
namespace VL
{
class RendererBackendVulkan;
}

class BrdfLutGenerator
{
public:
    static std::shared_ptr<Texture> Generate(
        PipelineFactory& pipelineFactory,
        VL::RendererBackendVulkan& rendererBackend);
};
