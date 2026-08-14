#pragma once

#include <memory>

#include <nlohmann/json.hpp>

#include "renderGraph.h"

namespace VL
{

class RendererBackendVulkan;

// Owns one isolated RenderGraph candidate. Before commit it destroys failed
// candidate Vulkan objects immediately. After the live graph swaps state with
// it, the same package is queued by frame epoch and destroys the old graph only
// when the GPU can no longer reference it.
class PreparedRenderGraphState
{
public:
    explicit PreparedRenderGraphState(
        RendererBackendVulkan& rendererBackend);
    ~PreparedRenderGraphState();

    PreparedRenderGraphState(
        const PreparedRenderGraphState&) = delete;
    PreparedRenderGraphState& operator=(
        const PreparedRenderGraphState&) = delete;

    void Load(const nlohmann::json& renderGraphJson);
    RenderGraph& GetGraph() noexcept { return graph; }

private:
    RendererBackendVulkan& rendererBackend;
    RenderGraph graph;
};

} // namespace VL
