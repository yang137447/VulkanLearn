#pragma once

// TerrainCompositedService private state and method declarations.
// Bodies are split into terrainCompositedServiceImpl{Candidate,Production,Cook}.cpp.

#include "render/resource/terrainCompositedServiceDetail.h"

namespace VL
{
using namespace TerrainServiceDetail;

struct TerrainCompositedService::Impl : TerrainFaultOwnerRegistration
{
    struct Buffer
    {
        RendererBackendVulkan* backend = nullptr;
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        void* mapped = nullptr;
        uint64_t bytes = 0;
        ~Buffer()
        {
            if (mapped)
                backend->UnmapMemory(memory);
            if (buffer)
                backend->DestroyBuffer(buffer, memory);
        }
    };

    struct InputAllocation
    {
        TerrainRvtBudgetLease lease;
        std::shared_ptr<Texture> texture;
    };

    struct CpuAllocation
    {
        TerrainRvtBudgetLease lease;
        HostImage image;
    };

    struct PendingImage
    {
        std::shared_ptr<Texture> texture;
        std::shared_ptr<Buffer> staging;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t layers = 1;
    };

    struct ProductionTask
    {
        struct StaleHold
        {
            std::optional<TerrainRvtBudgetLease> metadata;
            TerrainFaultRequest request;
        };
        std::unique_ptr<StaleHold> staleHold;
        std::optional<TerrainRvtBudgetLease> lease;
        struct Timing
        {
            RendererBackendVulkan* backend = nullptr;
            vk::QueryPool pool;
            ~Timing()
            {
                if (pool)
                    backend->DestroyQueryPool(pool);
            }
        };
        std::shared_ptr<Timing> timing;
        std::optional<TerrainRvtBudgetLease> executionLease;
        std::shared_ptr<Buffer> output;
        std::shared_ptr<Buffer> scratch;
        std::shared_ptr<Buffer> status;
        std::optional<TerrainCompositedPageUpload> page;
        uint64_t epoch = 0;
        bool recorded = false;
        std::vector<std::shared_ptr<Texture>> retained;
        std::shared_ptr<const TerrainCompositedProductionGpuResources> owner;
        std::shared_ptr<const Baker::AdjacencySnapshot> snapshot;
        std::shared_ptr<void> inputOwner;
        uint64_t candidateRevision = 0;
        Baker::ProducerSnapshot producer;
    };

    struct HeightCopy
    {
        std::shared_ptr<Texture> source;
        std::shared_ptr<Texture> destination;
        std::vector<vk::ImageCopy> regions;
        bool clear = true;
    };

    struct Surface
    {
        TerrainBuildPlan plan;
        Eigen::Matrix4f model;
        TerrainCompositedVTBuildDesc desc;
        nlohmann::json authorManifest;
        Baker::InputSnapshot input;
        std::vector<std::shared_ptr<Texture>> retained;
        std::vector<std::shared_ptr<MaterialInstance>> materials;
        std::vector<std::shared_ptr<const MaterialInstanceStateSnapshot>> materialStates;
        TerrainHeightAtlasResources height;
        uint64_t revision = 0;
        uint64_t sourceRevision = 0;
        uint64_t inputBytes = 0;
        uint64_t generation = 0;
        bool inputPending = false;
        std::array<std::shared_ptr<Texture>, 3> coverage;
        std::array<std::shared_ptr<Texture>, 4> sources;
        std::shared_ptr<Texture> sourceMap;
        std::shared_ptr<Texture> localWeights;
        std::shared_ptr<Texture> authorWeights;
        std::vector<std::optional<std::string>> authoredWeightIdentities;
        std::shared_ptr<const HostImage> importedWeights;
        std::vector<PendingImage> pendingImages;
        std::vector<HeightCopy> pendingHeight;
        std::vector<std::shared_ptr<Texture>> previousImages;
        uint32_t previewFailures = 0;
    };

    struct Update
    {
        TerrainBuildPlan plan;
        TerrainCompositedVTUVRect dirty;
        uint64_t revision = 0;
        uint64_t sourceRevision = 0;
        bool force = false;
        uint32_t attempts = 0;
    };

    struct InputPatch
    {
        std::shared_ptr<Texture> destination;
        uint32_t x = 0, y = 0, layer = 0, width = 0, height = 0;
        std::vector<uint8_t> bytes;
    };

    struct WorldLeafOwner
    {
        std::optional<TerrainRvtBudgetLease> metadata;
        std::map<const void*, TerrainRvtBudgetLease> exclusiveLeases;
        std::vector<std::shared_ptr<Texture>> textures;
        std::vector<std::shared_ptr<const HostImage>> cpuImages;
        std::map<const Texture*, std::shared_ptr<TerrainCompositedProductionGpuResources::PhaseUse>> uses;
    };

    struct WorldCandidate
    {
        WorldCandidate(uint64_t world, uint64_t revision, std::vector<std::string> ids)
            : transaction(world, revision, std::move(ids)) {}
        TerrainCompositedResourceSnapshot transaction;
        std::shared_ptr<TerrainRvtBudgetOwner> budget;
        std::vector<std::unique_ptr<Surface>> surfaces;
        std::array<std::shared_ptr<Texture>, 3> coverage;
        std::vector<TerrainRvtBudgetLease> cpuLeases;
        std::vector<InputPatch> patches;
        std::vector<HeightCopy> heightCopies;
        std::optional<TerrainRvtBudgetLease> stagingLease;
        std::shared_ptr<Buffer> staging;
        std::set<const Texture*> initialized;
        size_t nextPatch = 0;
        size_t recordedEnd = 0;
        uint64_t uploadEpoch = 0;
        bool cloned = false;
        bool recordedClone = false;
        bool inputsRecorded = false;
        bool runtimeStarted = false;
        std::vector<const Texture*> recordedInitialized;
        std::map<std::string, Update> changes;
        std::shared_ptr<WorldLeafOwner> inputs;
        Baker::ProducerSnapshot producer;
        double lastTelemetryMs = 0;
        TerrainCompositedSnapshotPhase lastTelemetryPhase = TerrainCompositedSnapshotPhase::Cancelled;
        size_t requiredPageCount = 0;
        uint64_t completedProductionCount = 0;
        const char* switchBlocker = "inputs";
    };

    RendererBackendVulkan& backend;

    PipelineFactory& pipelineFactory;

    TerrainStreamingState& streaming;

    uint64_t generation = ++NextGeneration;

    uint64_t worldGeneration = 0;

    ShaderReloadCoordinator* shaderReloadCoordinator = nullptr;

    Baker::ProducerSnapshot currentProducer;

    std::atomic<bool> interactiveReady{true};

    std::atomic<bool> qualityConvergenceFailed{false};

    uint64_t qualityPose = 0;

    uint64_t lastQualityEpoch = 0;

    double qualityPoseStartedMs = 0;

    TerrainCompositedQualityClock qualityClock;

    std::unique_ptr<Baker> baker;

    std::unique_ptr<TerrainCompositedRuntime> runtime;

    std::unique_ptr<TerrainCompositedServiceRequestDiagnostics> requestDiagnostics;

    bool requestDiagnosticsConfigured = false;

    std::optional<TerrainCompositedFrozenConfig> frozen;

    std::shared_ptr<TerrainCompositedVTStore> store;

    std::shared_ptr<TerrainCompositedProductionGpuResources> productionOwner;

    TerrainCompositedProductionGpuResources::WorldScope worldScope = std::make_shared<const uint64_t>(generation);

    std::shared_ptr<TerrainRvtBudgetOwner> peakOwner;

    std::shared_ptr<TerrainRvtBudgetOwner> allocationOverride;

    std::shared_ptr<WorldCandidate> worldCandidate;

    std::shared_ptr<WorldLeafOwner> currentInputs;

    std::vector<std::pair<uint64_t, std::shared_ptr<WorldLeafOwner>>> retiredInputs;

    uint64_t steadyWorkspaceQuote = 0;

    std::vector<std::pair<uint64_t, std::shared_ptr<WorldCandidate>>> retiredCandidates;

    std::vector<std::pair<uint64_t, std::vector<std::unique_ptr<Surface>>>> retiredSurfaces;

    std::map<const Texture*, TerrainRvtCost> inputAllocationCosts;

    std::map<const Texture*, std::weak_ptr<Texture>> inputAllocations;

    std::map<const Texture*, std::weak_ptr<InputAllocation>> exclusiveInputOwners;

    std::map<const HostImage*, std::weak_ptr<CpuAllocation>> cpuInputOwners;

    std::filesystem::path worldManifest;

    nlohmann::json authorRecipe;

    TerrainCompositedAdjacencyGraph adjacency;

    std::set<std::string> expectedSurfaces;

    bool finalized = false;

    uint64_t WorldGpuLimit = 0;

    uint64_t WorldCpuLimit = 0;

    std::vector<TerrainRvtBudgetLease> inputLeases;

    std::shared_ptr<const TerrainRvtBudgetLease> descriptorMetadataReserve;

    std::shared_ptr<TerrainRvtBudgetOwner> geometryOwner;

    std::unique_ptr<TerrainCompositedSourceSnapshots> sourceSnapshots;

    std::map<std::string, TerrainBuildPlan> preparedSourcePlans;

    TerrainRvtResourcePhase resourcePhase = TerrainRvtResourcePhase::Candidate;

    std::vector<TerrainRvtBudgetLease> planLeases;

    std::map<std::string, std::shared_ptr<Texture>> sourceMaps;

    std::map<std::string, TerrainHeightAtlasResources> sharedHeightInputs;

    std::vector<std::unique_ptr<Surface>> surfaces;

    std::vector<float> mapPixels = std::vector<float>(32 * 32 * 4, 0);

    std::vector<uint8_t> coveragePageMap;

    std::shared_ptr<Texture> surfaceMap;

    bool mapDirty = false;

    bool recording = false;

    uint64_t gpuBytes = 0;

    uint64_t cpuBytes = 0;

    uint64_t importedWeightBytes = 0;

    std::vector<ProductionTask> productionTasks;

    std::optional<TerrainRvtBudgetLease> bakerLease;

    std::unique_ptr<TerrainCompositedProductionPriority> productionPriority;

    std::vector<std::pair<uint64_t, std::vector<std::shared_ptr<Texture>>>> retired;

    std::vector<std::pair<uint64_t, std::shared_ptr<Buffer>>> stagingRetired;

    uint64_t lastCommittedEpoch = 0;

    std::atomic<bool> budgetSnapshotRequested{false};

    uint64_t nextEditRevision = 0;

    uint64_t staleBakeResults = 0;

    uint64_t abortedProductionTasks = 0;

    uint64_t successfulFrameCommits = 0;

    uint64_t cpuWorkspacePeak = 0;

    uint64_t gpuTransientPeak = 0;

    size_t nextUploadSurface = 0;

    mutable std::mutex mutex;

    std::map<std::string, Update> updates;

    struct Refresh
    {
        TerrainCompositedRefreshStatus status;
        TerrainCompositedRefreshCommand command;
        uint64_t revision = 0;
        std::set<std::string> affected;
        bool dynamicCommitted = false;
    };

    uint64_t nextTicket = 0;

    std::map<uint64_t, Refresh> refreshes;

    std::map<std::string, uint64_t> publishedRevisions;

    std::map<std::string, TerrainCompositedAuthorBaseline> authorBaselines;

    std::shared_ptr<const TerrainCompositedVTStore> authorVerificationStore;

    uint64_t authorVerificationWorld = 0;

    ContentDigest authorVerificationIdentity;

    std::shared_ptr<TerrainCompositedVerifiedInputs> scopedAuthorBaselineSession;

    std::map<std::string, std::shared_ptr<MaterialInstance>> materialUpdates;

    std::map<std::string, std::shared_ptr<const MaterialInstanceStateSnapshot>> materialIntents;

    std::map<std::string, std::unique_ptr<Surface>> materialCandidates;

    std::map<std::string, uint32_t> materialFailures;

    std::string stats = "TerrainVT: no registered surfaces";

    Surface Candidate(const Surface& surface);
    void BindPlanSources(const TerrainBuildPlan& plan, const TerrainBuildPlan* previous = nullptr);
    void Requeue(const std::string& identity, Update update);
    Impl(RendererBackendVulkan& renderer, PipelineFactory& factory, TerrainStreamingState& state);
    ~Impl();
    void ResetFaultHooks();
    void RegisterActiveFaultHooks();
    void CaptureRequestDiagnostics(std::optional<uint64_t> renderFrame, uint64_t completedEpoch);
    void InitializeProduction();
    void ValidateProductionGenerator() const;
    void PublishMap();
    void Bind(Surface& surface, const TerrainCompositedMaterialBinding* replacement = nullptr);
    MaterialInstanceStateBatch::Change MaterialChange(const Surface& surface,
        const std::shared_ptr<MaterialInstance>& material, const TerrainCompositedMaterialBinding& binding);
    std::shared_ptr<Buffer> HostBuffer(uint64_t bytes, vk::BufferUsageFlags usage, const char* name);
    std::shared_ptr<Texture> AllocateTexture(
        uint32_t width, uint32_t height, vk::Format format, vk::Filter filter, bool array,
        uint32_t layers = 1);
    std::shared_ptr<Texture> DeferredImage(
        Surface& surface, const HostImage& image, vk::Filter filter, bool array = false);
    void CaptureImportedWeights(Surface& surface, const MaterialInstance& material,
        const MaterialInstanceStateSnapshot* replacement = nullptr);
    void RefreshLocalWeights(Surface& surface, const TerrainBuildPlan& plan, bool deferred);
    void RecordImages(vk::CommandBuffer command, Surface& surface);
    void RefreshHeight(Surface& surface);
    void AddHeightCandidate(WorldCandidate& candidate, Surface& surface);
    void ConsumeMaterials();
    void ReserveCandidateCpu(WorldCandidate& candidate, uint64_t bytes);
    void AddCoveragePatches(WorldCandidate& candidate, const Surface& surface,
        const TerrainCompositedVTUVRect& dirty);
    std::shared_ptr<Texture> AddCpuImagePatches(WorldCandidate& candidate, const HostImage& image,
        vk::Format format, vk::Filter filter, bool array = false);
    void AddFramePatches(WorldCandidate& candidate, Surface& surface);
    std::shared_ptr<Texture> RetainExternalInput(const std::shared_ptr<Texture>& texture);
    void AddLocalWeightPatches(WorldCandidate& candidate, Surface& surface);
    void CheckCandidateSteadyInputs(WorldCandidate& candidate);
    void ConsumeCandidateBudgetAdmission(
        const std::shared_ptr<TerrainRvtBudgetOwner>& owner, uint64_t additionalGpuBytes);
    void QueuePendingProducer();
    void ConsumeUpdates();
    void CandidateImageBarrier(vk::CommandBuffer command, const Texture& texture,
        vk::ImageLayout before, vk::ImageLayout after,
        vk::AccessFlags source, vk::AccessFlags destination);
    uint64_t RecordWorldInputs(vk::CommandBuffer command, uint64_t completedEpoch);
    void DiscardWorldCandidate(const std::string& diagnostic, bool keepPendingProducer = false);
    void EmitCandidateTelemetry(WorldCandidate& candidate, uint64_t completedEpoch);
    void AdvanceWorldCandidate(uint64_t completedEpoch);
    void JointSwitchWorldCandidate(uint64_t completedEpoch);
    void EmitBudgetObservation(uint64_t completedEpoch);
    bool PrepareDynamicRefresh(Refresh& refresh);
    void PrepareRefreshes();
    void UpdateRefreshes(bool submitted);
    void CompleteProduction(uint64_t completedEpoch, bool loadingFenceComplete = false);
    void RecordProduction(vk::CommandBuffer command, uint64_t completedEpoch);
    void Capture(Surface& surface, const std::shared_ptr<MaterialInstance>& material);
    TerrainCompositedVTPublication CapturePublication();
    TerrainCompositedVTPageData Bake(const Surface& surface, const TerrainCompositedVTPageKey& key);
    void SnapshotStats();
};
} // namespace VL
