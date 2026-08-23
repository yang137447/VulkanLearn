#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class PipelineFactory;

namespace VL
{

class RendererBackendVulkan;
class EyeComputeReloadParticipant;
struct EyeComputePipelineCandidate;
struct EyeLutReadbackReport;
struct EyeProfileAsset;
class EyeResourceSet;
struct RendererResourceLoadContext;

std::string BuildEyeResourceSourceDigest(
    const std::filesystem::path& resourceRoot,
    const std::vector<EyeProfileAsset>& profiles,
    std::string_view artifactGenerationKey);

std::shared_ptr<EyeResourceSet> BuildEyeResourceSet(
    RendererBackendVulkan& rendererBackend,
    std::vector<EyeProfileAsset> profiles,
    std::string sourceDigest,
    EyeComputePipelineCandidate pipelineCandidate,
    EyeLutReadbackReport* readbackReport = nullptr);

// 在 pass/mesh material 之前构建当前候选 World 的 Eye profile/LUT 资源包。
// profile 解析、Compute 生成和 worldTexture 绑定都只写入 candidate cache。
class EyeResourceLoader
{
public:
    EyeResourceLoader(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        RendererResourceLoadContext& loadContext,
        EyeComputeReloadParticipant* reloadParticipant = nullptr);

    void Load() const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
    RendererResourceLoadContext& loadContext;
    EyeComputeReloadParticipant* reloadParticipant = nullptr;
};

} // namespace VL