#pragma once

#include "terrain/terrainCompositedResidency.h"
#include "terrain/terrainRvtBudget.h"

#include <array>
#include <vulkan/vulkan.hpp>

class Texture;
namespace VL
{
class RendererBackendVulkan;

struct TerrainCompositedRuntimeBudget
{
    TerrainCompositedResidencyBudget surface;
    uint32_t frameRingSize = 0;
    uint64_t gpuBytes = 0;
    uint64_t cpuBytes = 0;
    uint64_t uploadBytesPerFrame = 0;
    uint32_t uploadsPerFrame = 0;
    uint64_t schedulingMicroseconds = 0;
    uint32_t freeSlotReserve = 0;
    std::shared_ptr<TerrainRvtBudgetOwner> owner;
};

struct TerrainCompositedMaterialBinding
{
    std::shared_ptr<Texture> compositedPageTable;
    std::shared_ptr<Texture> compositedAlbedo;
    std::shared_ptr<Texture> compositedNormal;
    std::shared_ptr<Texture> compositedOrm;
    std::array<float, 2> u_compositedResolution{};
    float u_compositedMaxMip = 0;
    float u_compositedEnabled = 0;
    float u_compositedRepeat = 0;
    float u_compositedNormalRG = 0;
};

struct TerrainCompositedFrameUploadBudget
{
    uint64_t remainingBytes = 4 * 1024 * 1024;
    uint32_t remainingPages = 16;
    uint64_t remainingSchedulingMicroseconds = 2000;
};

struct TerrainCompositedRuntimeStats
{
    uint64_t gpuBytes = 0;
    uint64_t reservedCpuBytes = 0;
    uint64_t stagingBytes = 0;
    uint64_t lastRecordMicroseconds = 0;
    bool budgetInsufficient = false;
    uint32_t effectiveMipBias = 0;
    std::map<std::string, TerrainCompositedResidencyStats> surfaces;
};

struct TerrainCompositedSurfaceProgressDiagnostics
{
    TerrainCompositedResidencyProgressDiagnostics pages;
    uint64_t tablePublishEpoch = 0;
    uint64_t retireEpoch = 0;
    bool present = false;
    bool initialized = false;
};

struct TerrainCompositedCandidateDiagnostics
{
    TerrainCompositedSurfaceProgressDiagnostics current;
    TerrainCompositedSurfaceProgressDiagnostics candidate;
    uint64_t completedEpoch = 0;
    bool jointSwitchPrepared = false;
};

// 主帧提交成功后 Commit，丢弃 command buffer 后 Abort；调用方以 World 退休包持有本类至 GPU 完成。
// 所有入口由 renderer 串行调用，worker 只读取 Store，不访问 Vulkan 或可变驻留状态。
class TerrainCompositedRuntime
{
public:
    explicit TerrainCompositedRuntime(
        RendererBackendVulkan& backend, TerrainCompositedRuntimeBudget budget);
    ~TerrainCompositedRuntime();
    TerrainCompositedRuntime(const TerrainCompositedRuntime&) = delete;
    TerrainCompositedRuntime& operator=(const TerrainCompositedRuntime&) = delete;
    void RegisterSurface(
        std::shared_ptr<const TerrainCompositedVTStore> store,
        uint64_t generation, uint64_t revision = 0);
    void RegisterSurface(
        std::shared_ptr<const TerrainCompositedVTStore> store,
        const TerrainCompositedVTBuildDesc& surface,
        uint64_t generation, uint64_t revision);
    void RegisterPreviewSurface(
        const TerrainCompositedVTBuildDesc& surface, uint64_t generation, uint64_t revision);
    // 加载安全点：只读 Store 同步 seed，随后提交并等待专用 fence 才允许首次 Commit(0)。
    void SeedReadOnlyBaseline();
    void ConsumeFeedback(const std::vector<TerrainCompositedVTTaskToken>& requests);
    void InvalidatePreview(const TerrainCompositedVTBuildDesc& surface,
        uint64_t generation, uint64_t revision,
        const std::vector<TerrainCompositedVTPageKey>& dirtyPages);
    void SubmitPreviewPages(const TerrainCompositedVTBuildDesc& surface,
        uint64_t generation, uint64_t revision,
        const std::vector<TerrainCompositedVTPageData>& pages);
    // 返回是否有 payload copy；即使返回 false，录制事务也必须 Commit/Abort。
    bool RecordUploads(vk::CommandBuffer commandBuffer, uint32_t frameIndex, uint64_t completedEpoch);
    bool RecordUploads(vk::CommandBuffer commandBuffer, uint32_t frameIndex,
        uint64_t completedEpoch, TerrainCompositedFrameUploadBudget& sharedBudget);
    void Commit(uint64_t submittedEpoch);
    void Abort();
    TerrainCompositedMaterialBinding Binding(const std::string& surfaceId) const;
    TerrainCompositedVTTaskToken Token(const TerrainCompositedVTPageKey& key) const;
    std::vector<TerrainCompositedVTTaskToken> PreviewRequests() const;
    std::vector<TerrainCompositedVTTaskToken> ProductionRequests() const;
    std::optional<TerrainCompositedPageUpload> ReserveProduction(
        const TerrainCompositedVTTaskToken& token, uint64_t completedEpoch);
    void RecordProductionCopy(vk::CommandBuffer command, vk::Buffer output,
        const TerrainCompositedPageUpload& page);
    bool CompleteProduction(const TerrainCompositedPageUpload& page, uint64_t epoch, bool success);
    std::optional<TerrainCompositedPageDiagnostics> ProductionDiagnostics(
        const TerrainCompositedVTTaskToken& token, uint64_t completedEpoch) const;
    std::optional<TerrainCompositedCandidateDiagnostics> CandidateDiagnostics(
        const std::string& logicalId, uint64_t completedEpoch) const;
    std::optional<TerrainCompositedSlotDiagnostics> SlotDiagnostics(
        uint32_t slot, uint64_t completedEpoch) const;
    bool ArmSlotReuseDiagnostics(uint32_t slot);
    void CancelSlotReuseDiagnostics() noexcept;
    std::optional<TerrainCompositedSlotReuseDiagnostics> SlotReuseDiagnostics(
        uint64_t completedEpoch) const;
    std::vector<TerrainCompositedSlotOwner> RecordedOwners() const;
    bool Ready() const;
    bool RootsCurrent() const;
    TerrainCompositedRuntimeStats Statistics() const;
    void BeginCandidate(const std::vector<TerrainCompositedVTBuildDesc>& surfaces,
        uint64_t generation, uint64_t revision,
        std::shared_ptr<TerrainRvtBudgetOwner> candidateOwner);
    bool CandidateReady(uint64_t completedEpoch) const;
    bool PrepareJointSwitch(uint64_t completedEpoch,
        const std::vector<TerrainRvtBudgetPhaseChange>& additionalPhaseChanges = {});
    bool CommitCandidate() noexcept;
    void CancelPreparedJointSwitch() noexcept;
    void CancelCandidate();
    bool CandidateToken(const TerrainCompositedVTTaskToken& token) const;
    std::string LogicalSurfaceId(const std::string& namespaceId) const;
    TerrainCompositedMaterialBinding CandidateBinding(const std::string& surfaceId) const;
    std::vector<TerrainCompositedVTPageKey> SelectedPages() const;

private:
    friend struct TerrainCompositedRuntimeTestAccess;
    friend struct TerrainCompositedRequestProbe;
    struct Surface
    {
        std::unique_ptr<TerrainCompositedResidency> residency;
        TerrainCompositedMaterialBinding binding;
        std::array<uint64_t, 3> channelBytes{};
        std::vector<TerrainCompositedVTFormat> formats;
        bool initialized = false;
        std::string logicalId;
        uint64_t publishEpoch = 0;
        uint64_t retireEpoch = 0;
        bool retired = false;
        uint64_t tableBytes = 0;
        std::vector<TerrainCompositedVTPageKey> requiredPages;
        std::optional<TerrainRvtBudgetLease> tableLease;
    };
    struct Frame
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        void* mapped = nullptr;
        uint64_t bytes = 0;
        uint64_t epoch = 0;
    };
    struct Pending
    {
        std::string surfaceId;
        TerrainCompositedResidencyBatch batch;
    };
    void ReleaseFrame(Frame& frame);
    void RegisterDescriptions(std::shared_ptr<const TerrainCompositedVTStore> store,
        const std::vector<TerrainCompositedVTBuildDesc>& descriptions,
        uint64_t generation, uint64_t revision,
        std::shared_ptr<TerrainRvtBudgetOwner> allocationOwner = {});
    void CollectRetired(uint64_t completedEpoch);
    RendererBackendVulkan& backend;
    TerrainCompositedRuntimeBudget budget;
    std::map<std::string, Surface> surfaces;
    std::map<std::string, std::string> activeNamespaces;
    std::map<std::string, std::string> candidateNamespaces;
    std::map<std::string, std::string> preparedNamespaces;
    std::vector<Surface*> preparedRetirements;
    bool jointSwitchPrepared = false;
    std::shared_ptr<TerrainRvtBudgetOwner> candidatePhaseOwner;
    std::optional<TerrainRvtBudgetPhaseBatch> preparedPhaseBatch;
    std::optional<TerrainRvtBudgetPhaseBatch> abandonedPhaseBatch;
    uint64_t nextNamespace = 0;
    std::shared_ptr<TerrainCompositedResidencyPool> sharedPool;
    std::array<std::shared_ptr<Texture>, 3> sharedChannels;
    std::vector<TerrainCompositedVTFormat> sharedFormats;
    std::vector<TerrainRvtBudgetLease> leases;
    std::vector<Frame> frames;
    std::vector<Pending> pending;
    std::optional<uint32_t> recordingFrame;
    uint64_t gpuBytes = 0;
    uint64_t reservedCpuBytes = 0;
    uint64_t lastSubmittedEpoch = 0;
    bool hasSubmitted = false;
    bool baselineSeeded = false;
    uint64_t scheduleCursor = 0;
    uint64_t lastRecordMicroseconds = 0;
    bool budgetInsufficient = false;
    uint32_t effectiveMipBias = 0;
    mutable uint64_t candidateCompletedEpoch = 0;
};
}
