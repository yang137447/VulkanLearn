#pragma once

#include <cstdint>
#include <memory>

class PipelineFactory;
class Texture;
namespace VL
{
class RendererBackendVulkan;
}

class EnvironmentPrefilterGenerator
{
public:
    // 从场景环境 cubemap 生成带 roughness mip 链的预过滤环境图，用于 specular IBL。
    static std::shared_ptr<Texture> Generate(
        const Texture& environmentCube,
        uint32_t cubeSize,
        PipelineFactory& pipelineFactory,
        VL::RendererBackendVulkan& rendererBackend);
};
