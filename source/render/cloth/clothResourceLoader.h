#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ComputePipeline;
class PipelineFactory;

namespace VL
{

class RendererBackendVulkan;
class ClothComputeReloadParticipant;
class ClothResourceSet;
struct RendererResourceLoadContext;

std::string BuildClothResourceSourceDigest(
    std::string_view artifactGenerationKey);

std::shared_ptr<ClothResourceSet> BuildClothResourceSet(
    RendererBackendVulkan& rendererBackend,
    std::shared_ptr<ComputePipeline> computePipeline,
    std::string sourceDigest);

// 在 pass/mesh material 之前构建当前候选 World 的 Cloth LUT/resource package。
// 生成失败直接拒绝 candidate，避免 active World 看到半成品资源。
class ClothResourceLoader
{
public:
    ClothResourceLoader(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        RendererResourceLoadContext& loadContext,
        ClothComputeReloadParticipant* reloadParticipant = nullptr);

    void Load() const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
    RendererResourceLoadContext& loadContext;
    ClothComputeReloadParticipant* reloadParticipant = nullptr;
};

} // namespace VL