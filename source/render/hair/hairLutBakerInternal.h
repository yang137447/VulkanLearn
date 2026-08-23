#pragma once

#include <memory>
#include <string_view>

class PipelineFactory;
class Texture;

namespace VL
{

class RendererBackendVulkan;
struct HairLutBakeInput;

// GPU-only Hair LUT baker 的实现入口；独立声明便于资源 loader 与测试保持边界清晰。
std::shared_ptr<Texture> GenerateHairAzimuthalLutTexture(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    const HairLutBakeInput& input,
    std::string_view sourceIdentity);

} // namespace VL
