#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pipeline/pipelineFactory.h"
#include "render/resource/rendererResourceCache.h"

#include <nlohmann/json.hpp>

class RenderGraph;
class MaterialInstance;

namespace VL
{

struct MaterialDefinitionReloadBatch;

struct PendingGeneratedResourceFile
{
    std::filesystem::path path;
    std::vector<uint8_t> bytes;
};

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
    // MI preview 使用内存草稿覆盖单个规范化资产路径，不提前写回磁盘。
    std::string materialInstanceOverridePath;
    std::optional<nlohmann::json> materialInstanceOverrideJson;
    std::unordered_map<
        std::string,
        std::shared_ptr<MaterialInstance>>*
        passMaterialBindings = nullptr;
    // candidate 阶段只收集生成文件；只有 World transaction 的正式发布批次才能落盘。
    std::vector<PendingGeneratedResourceFile> pendingGeneratedFiles;

    void QueueGeneratedFile(
        std::filesystem::path path,
        std::vector<uint8_t> bytes)
    {
        pendingGeneratedFiles.push_back({
            std::move(path),
            std::move(bytes)});
    }
};

} // namespace VL
