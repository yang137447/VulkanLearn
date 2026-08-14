#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "pipeline/pipelineFactory.h"
#include "render/resource/rendererResourceCache.h"

class RenderGraph;
class MaterialInstance;

namespace VL
{

struct MaterialDefinitionReloadBatch;

// Explicit staging context for one renderer-side world load. Loaders may only
// mutate resourceCache/renderGraph supplied here; active singletons are not
// part of candidate construction.
struct RendererResourceLoadContext
{
    RendererResourceCache& resourceCache;
    RenderGraph& renderGraph;
    PipelineFactory::GraphicsCandidateState* graphicsCandidateState = nullptr;
    RendererResourceCache::ImmutableWorldLocalResourceRefs
        previousWorldResources;
    const MaterialDefinitionReloadBatch* materialDefinitionReload = nullptr;
    std::unordered_map<
        std::string,
        std::shared_ptr<MaterialInstance>>*
        passMaterialBindings = nullptr;
};

} // namespace VL
