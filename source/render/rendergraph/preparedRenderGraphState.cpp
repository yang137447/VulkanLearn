#include "render/rendergraph/preparedRenderGraphState.h"

#include "render/backend/rendererBackendVulkan.h"

namespace VL
{

PreparedRenderGraphState::PreparedRenderGraphState(
    RendererBackendVulkan& rendererBackend)
    : rendererBackend(rendererBackend)
{
}

PreparedRenderGraphState::~PreparedRenderGraphState()
{
    if (graph.HasState())
    {
        graph.Shutdown(
            rendererBackend,
            RenderGraphReleaseMode::Immediate);
    }
}

void PreparedRenderGraphState::Load(
    const nlohmann::json& renderGraphJson)
{
    graph.LoadRenderGraph(
        renderGraphJson,
        rendererBackend);
}

} // namespace VL
