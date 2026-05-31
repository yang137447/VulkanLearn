#pragma once

#include <cstdint>
#include <memory>
#include <string>

class PipelineFactory;
class Texture;
namespace VL
{
class RendererBackendVulkan;
}

class EnvironmentCubemapGenerator
{
public:
    // 把场景里声明的经纬度 HDR/EXR 环境图转换成运行时 cubemap。
    // 调试导出仅在 debug 模式下执行，最终资源由调用方持有。
    static std::shared_ptr<Texture> Generate(
        const std::string& hdrPath,
        uint32_t cubeSize,
        PipelineFactory& pipelineFactory,
        VL::RendererBackendVulkan& rendererBackend);
};
