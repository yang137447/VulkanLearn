#pragma once

class PipelineFactory;

namespace VL
{

class RendererBackendVulkan;
struct RendererResourceLoadContext;

// 在 pass/mesh material 之前构建当前候选 World 的 SSS profile/LUT 资源包。
// 这里负责 JSON 校验、相同 source digest 的跨 generation 复用和 Compute 生成，
// prepare 阶段不直接修改 active World。
class SubsurfaceResourceLoader
{
public:
    SubsurfaceResourceLoader(
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
