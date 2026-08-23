#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class PipelineFactory;
class Texture;

namespace VL
{

class HairResourceSet;
class RendererBackendVulkan;
struct RendererResourceLoadContext;

struct HairLutBakeInput
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 0;
    float ior = 0.0f;
    float fiberRadius = 0.0f;
    float longitudinalRoughness = 0.0f;
    float azimuthalRoughness = 0.0f;
};

// 只负责 Vulkan 资源、descriptor、dispatch、barrier 和候选 metadata；不在 CPU
// 生成生产 texel。metadata 由 World transaction 正式发布，旧资源由 World-local
// package 持有并交给已有 GPU epoch retirement。
class HairLutBaker
{
public:
    static std::shared_ptr<Texture> Generate(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        const HairLutBakeInput& input,
        std::string_view sourceIdentity);
};

class HairResourceLoader
{
public:
    HairResourceLoader(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        RendererResourceLoadContext& loadContext);

    void Load() const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
    RendererResourceLoadContext& loadContext;
};

} // namespace VL
