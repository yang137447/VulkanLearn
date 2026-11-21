#pragma once

#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <Eigen/Core>
#include <vulkan/vulkan.hpp>


class PipelineFactory;
class MaterialInstance;
class Texture;

namespace VL
{
class RendererBackendVulkan;
class ShaderReloadCoordinator;
class RendererResourceCache;
struct TerrainStreamingState;
struct TerrainBuildPlan;
struct RendererResourceLoadContext;

struct TerrainCompositedFrozenConfig;
struct TerrainCompositedFeedbackRecord;
struct TerrainCompositedVTUVRect;
struct TerrainCompositedRefreshCommand;
struct TerrainCompositedRefreshTicket;
struct TerrainCompositedRefreshStatus;
struct TerrainCompositedAuthorBaseline;
struct TerrainCompositedQualitySnapshot;
struct TerrainCompositedQualityReadback;
class TerrainRvtBudgetLease;
class TerrainRvtBudgetOwner;
enum class TerrainRvtResourcePhase;

class TerrainCompositedService
{
public:
    void SetShaderReloadCoordinator(ShaderReloadCoordinator* coordinator);
    void ConfigureWorldDescriptorBudget(RendererResourceCache& cache);
    TerrainCompositedService(
        RendererBackendVulkan& backend, PipelineFactory& pipelineFactory,
        TerrainStreamingState& streaming);
    ~TerrainCompositedService();
    TerrainCompositedService(const TerrainCompositedService&) = delete;
    TerrainCompositedService& operator=(const TerrainCompositedService&) = delete;

    static void SetCookMode(bool enabled);
    static bool CookMode();
    static void SetAuthoringMode(bool enabled);
    static bool IsAuthoringMode();
    static void SetFrozenConfig(TerrainCompositedFrozenConfig config);
    static void InitializeFrozenConfig(RendererBackendVulkan& backend);
    void SetWorldGeneration(uint64_t generation);
    void SetPhase(TerrainRvtResourcePhase phase);
    std::shared_ptr<TerrainRvtBudgetOwner> BudgetOwner() const;
    std::shared_ptr<TerrainRvtBudgetOwner> DiagnosticReferencePoolOwner() const;
    std::shared_ptr<TerrainCompositedQualitySnapshot> CaptureQualitySnapshot(
        uint64_t frame, uint64_t poseId, uint64_t drawEpoch,
        uint32_t renderWidth, uint32_t renderHeight);
    void SetQualityPose(uint64_t worldGeneration, uint64_t poseId, double monotonicMs);
    bool ObserveQuality(const TerrainCompositedQualityReadback& readback);
    bool IsInteractiveReady() const noexcept;
    bool QualityConvergenceFailed() const noexcept;
    TerrainRvtBudgetLease AdmitCpuWorkspace(uint64_t bytes);
    TerrainRvtBudgetLease AdmitGeometry(uint64_t vertexBytes, uint64_t indexBytes,
        uint64_t patchBytes, bool dynamicVertices,
        std::optional<TerrainRvtBudgetLease>& uploadLease);
    void PrepareWorld(const std::vector<TerrainBuildPlan>& plans);
    void PrepareMaterialInputs(
        const TerrainBuildPlan& plan, RendererResourceLoadContext& context);
    void RegisterSurface(
        const TerrainBuildPlan& plan, const Eigen::Matrix4f& model,
        const std::shared_ptr<MaterialInstance>& baseMI,
        const std::shared_ptr<MaterialInstance>& detailMI = {});
    bool RecordUploads(vk::CommandBuffer command, uint32_t frameIndex, uint64_t completedEpoch,
        std::optional<uint64_t> renderFrame = std::nullopt);
    void Commit(uint64_t epoch);
    void Abort();
    void ConsumeFeedback(const std::vector<TerrainCompositedFeedbackRecord>& requests);
    bool HasRequestDiagnostics() const noexcept;
    void ObserveFeedbackReadback(const uint8_t* bytes, size_t byteCount, uint8_t frameTag,
        uint64_t copiedEpoch, uint64_t completedEpoch);
    void RefreshPlacement(
        const TerrainBuildPlan& plan, const TerrainCompositedVTUVRect& dirtyRect,
        uint64_t revision);
    TerrainCompositedRefreshTicket QueueRefresh(TerrainCompositedRefreshCommand command);
    TerrainCompositedRefreshStatus PollRefresh(const TerrainCompositedRefreshTicket& ticket) const;
    uint64_t RefreshRevision(uint64_t worldGeneration, const std::string& surfaceId) const;
    TerrainCompositedAuthorBaseline LoadAuthorBaseline(
        uint64_t worldGeneration, const std::string& surfaceId) const;
    void NotifyMaterialChanged(
        const std::string& materialInstancePath,
        const std::shared_ptr<MaterialInstance>& materialInstance);
    std::shared_ptr<Texture> SurfaceMap();
    void FinaliseSurfaceMap();
    void FinalizeSurfaceMap() { FinaliseSurfaceMap(); }
    bool Enabled() const;
    bool HasSurfaces() const;
    void ReportStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
