#pragma once

#include "render/resource/terrainCompositedBakeGpu.h"
#include "resource/image/hostImage.h"
#include "terrain/terrainCompositedProduction.h"
#include "terrain/terrainRvtBudget.h"
#include "shader/build/atomicFile.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

class Texture;

namespace VL
{
class TerrainCompositedVTStore;
using TerrainProductionObjectWriter = std::function<void(
    const TerrainCompositedProductionObject&, const std::vector<uint8_t>&)>;
using TerrainProductionObjectReader = std::function<std::vector<uint8_t>(
    const TerrainCompositedProductionObject&)>;

// backend 预查询实际 memoryRequirements，另按 P0 口径计入 view/sampler/driver 成本。
// 回调不得分配/bind device memory；image 还须包含该对象持久 CPU 元数据，
// transfer 包含 staging allocation 与映射内存，须覆盖同尺寸 TransferSrc/TransferDst
// coherent buffer 的最大需求。缺失查询时 GPU 入口拒绝分配。
struct TerrainCompositedProductionAllocationQuery
{
    std::function<TerrainRvtCost(const TerrainCompositedProductionImageDesc&, bool array)> image;
    std::function<TerrainRvtCost(uint64_t payloadBytes)> transfer;
};

// Vulkan 不公开 view/sampler/driver 元数据的 allocation size；调用方显式传入
// P0 冻结的逐对象预留成本，不能把这些项暗置零或当成查询出的 device memory。
struct TerrainCompositedProductionAllocationOverheads
{
    TerrainRvtCost imageViewAndSampler;
    TerrainRvtCost imageMetadata;
    TerrainRvtCost transferMetadata;
    TerrainRvtCost queryMetadata;
};

// Snapshot 持叶用途与独立 CPU metadata lease；Texture alias 只保叶及其实际预算。
// prior 仅在 BudgetState 与 WorldScope 控制块各自相同时共享；WorldScope 必须逐 World 新建。
class TerrainCompositedProductionGpuResources
{
public:
    using WorldScope = std::shared_ptr<const void>;
    class PhaseUse;
    class PhaseTransitionTicket;
    struct PhaseTransition
    {
        std::shared_ptr<PhaseUse> use;
        TerrainRvtResourcePhase phase;
    };
    static std::optional<PhaseTransitionTicket> PrepareTransitions(
        const std::vector<PhaseTransition>& changes, TerrainRvtBudgetOwner& metadataOwner);
    struct UploadedImage
    {
        std::shared_ptr<Texture> texture;
        vk::Image image{};
        vk::ImageView view{};
        vk::Sampler sampler{};
    };
    struct Allocation
    {
        uint64_t allocationIdentity = 0;
        TerrainRvtCost actualCost;
        vk::Image image{};
        vk::ImageView view{};
        vk::Sampler sampler{};
    };
    using ImageUploader = std::function<UploadedImage(
        const HostImage&, const TerrainCompositedProductionImageDesc&, bool)>;
    ~TerrainCompositedProductionGpuResources();
    TerrainCompositedProductionGpuResources(const TerrainCompositedProductionGpuResources&) = delete;
    TerrainCompositedProductionGpuResources& operator=(const TerrainCompositedProductionGpuResources&) = delete;
    const TerrainCompositedProductionClosure& Closure() const { return closure; }
    std::shared_ptr<Texture> Image(const std::string& objectId) const;
    std::shared_ptr<Texture> SourceMap(const std::string& surfaceId) const;
    std::shared_ptr<PhaseUse> RetainImageUse(
        const std::string& objectId, TerrainRvtResourcePhase phase) const;
    std::shared_ptr<PhaseUse> RetainSourceMapUse(
        const std::string& surfaceId, TerrainRvtResourcePhase phase) const;
    Allocation ImageAllocation(const std::string& objectId) const;
    Allocation SourceMapAllocation(const std::string& surfaceId) const;
    TerrainRvtCost SnapshotMetadataCost() const;
    void InvalidateImageReuse(const std::string& objectId) const;
    void InvalidateSourceMapReuse(const std::string& surfaceId) const;
    void SetPhase(TerrainRvtResourcePhase phase);
    static uint64_t MetadataBytes(const TerrainCompositedProductionClosure& closure);
    static std::shared_ptr<TerrainCompositedProductionGpuResources> RestoreImages(
        const TerrainCompositedProductionClosure& closure,
        const TerrainProductionObjectReader& reader, TerrainRvtBudgetOwner& budget,
        const TerrainCompositedProductionAllocationQuery& allocations,
        const ImageUploader& upload,
        std::shared_ptr<const TerrainCompositedProductionGpuResources> prior = {},
        WorldScope worldScope = {});

private:
    friend class TerrainCompositedProductionGpu;
    struct Use;
    struct Leaf
    {
        Leaf(TerrainRvtBudgetLease charge, TerrainCompositedProductionObject metadata, bool array)
            : lease(std::move(charge)), phaseFloor(lease.Phase()),
              object(std::move(metadata)), array(array) {}
        void RefreshPhase();
        TerrainRvtBudgetLease lease;
        TerrainRvtResourcePhase phaseFloor;
        inline static std::atomic<uint64_t> nextIdentity{1};
        const uint64_t allocationIdentity = nextIdentity.fetch_add(1, std::memory_order_relaxed);
        std::atomic<bool> reusable{true};
        TerrainCompositedProductionObject object;
        bool array;
        std::mutex mutex;
        Use* uses = nullptr;
        UploadedImage uploaded;
    };
    struct Use
    {
        explicit Use(std::shared_ptr<Leaf> owner, const Use* source = nullptr);
        ~Use();
        void SetPhase(TerrainRvtResourcePhase value);
        std::shared_ptr<Leaf> leaf;
        TerrainRvtResourcePhase phase = TerrainRvtResourcePhase::Candidate;
        Use* previous = nullptr;
        Use* next = nullptr;
    };
    TerrainCompositedProductionGpuResources() = default;
    std::optional<TerrainRvtBudgetLease> metadataLease;
    TerrainCompositedProductionClosure closure;
    WorldScope worldScope;
    std::shared_ptr<const void> budgetIdentity;
    TerrainRvtResourcePhase phase = TerrainRvtResourcePhase::Candidate;
    std::map<std::string, std::unique_ptr<Use>> images;
    std::map<std::string, std::unique_ptr<Use>> sourceMaps;
};

class TerrainCompositedProductionGpuResources::PhaseUse
{
public:
    PhaseUse(const PhaseUse&) = delete;
    PhaseUse& operator=(const PhaseUse&) = delete;
    void SetPhase(TerrainRvtResourcePhase phase) { use.SetPhase(phase); }
    TerrainRvtResourcePhase Phase() const;
    std::shared_ptr<PhaseUse> Retain(TerrainRvtResourcePhase phase) const;
    std::shared_ptr<Texture> GetTexture() const;
    Allocation GetAllocation() const;
    static constexpr uint64_t MetadataBytes() { return sizeof(PhaseUse) + 128; }

private:
    friend class TerrainCompositedProductionGpuResources;
    friend class PhaseTransitionTicket;
    PhaseUse(std::shared_ptr<Leaf> leaf, TerrainRvtResourcePhase phase, const Use* source = nullptr)
        : use(std::move(leaf), source) { use.SetPhase(phase); }
    Use use;
};

class TerrainCompositedProductionGpuResources::PhaseTransitionTicket
{
public:
    PhaseTransitionTicket(PhaseTransitionTicket&&) noexcept = default;
    PhaseTransitionTicket& operator=(PhaseTransitionTicket&&) noexcept = delete;
    PhaseTransitionTicket(const PhaseTransitionTicket&) = delete;
    PhaseTransitionTicket& operator=(const PhaseTransitionTicket&) = delete;
    bool TryLockLeavesNoThrow() noexcept;
    const std::vector<TerrainRvtBudgetPhaseChange>& BudgetChanges() const noexcept { return changes; }
    void FinalizeNoThrow() noexcept;
    void CancelNoThrow() noexcept;

private:
    friend class TerrainCompositedProductionGpuResources;
    explicit PhaseTransitionTicket(TerrainRvtBudgetLease charge) : metadata(std::move(charge)) {}
    std::optional<TerrainRvtBudgetLease> metadata;
    std::vector<PhaseTransition> transitions;
    std::vector<std::shared_ptr<Leaf>> leaves;
    std::vector<TerrainRvtResourcePhase> observedPhases;
    std::vector<TerrainRvtBudgetPhaseChange> changes;
    std::vector<std::unique_lock<std::mutex>> locks;
    bool locked = false;
    bool finalized = false;
};

// InputSnapshot 中的 descriptor 借用 owner；提交完成前必须保留整个返回值。
struct TerrainCompositedOwnedBakeInput
{
    std::shared_ptr<const TerrainCompositedProductionGpuResources> owner;
    TerrainCompositedBakeGpu::InputSnapshot input;
};

struct TerrainCompositedProductionTransferChunk
{
    uint64_t rawOffset = 0;
    uint64_t bytes = 0;
    vk::BufferImageCopy copy{};
};

// 只转换 P1 对象与实际像素；Writer 由发布调用方绑定 WriteInputObject，
// Store 的读取重载适配 ReadInput，无独立 JSON。
// GPU 捕获/恢复为安全点同步操作，不可用于每帧生产；backend 必须晚于 owner 销毁。
class TerrainCompositedProductionGpu
{
public:
    static constexpr uint64_t MaxTransferBytes = 1024 * 1024;
    static constexpr uint64_t TransferScratchCpuBytes = 64 * 1024 + sizeof(HostImage) +
        sizeof(TerrainCompositedProductionTransferChunk) + sizeof(vk::MemoryBarrier) * 2 +
        sizeof(vk::ImageMemoryBarrier) * 2 + sizeof(vk::Buffer) + sizeof(vk::DeviceMemory) +
        sizeof(vk::CommandBuffer) + sizeof(void*);
    static void VisitTransferChunks(
        const TerrainCompositedProductionImageDesc& desc,
        const std::function<void(const TerrainCompositedProductionTransferChunk&)>& visit);
    static TerrainCompositedProductionAllocationQuery MakeAllocationQuery(
        RendererBackendVulkan& backend, TerrainRvtBudgetOwner& budget,
        const TerrainCompositedProductionAllocationOverheads& overheads);
    static TerrainCompositedProductionObject CaptureCpuImage(
        TerrainCompositedProductionObject object, const HostImage& image,
        TerrainRvtBudgetOwner& budget, const TerrainProductionObjectWriter& writer);
    // 调用者提供不可变、已完成上传且处于 shader-read layout 的纹理强引用。
    static TerrainCompositedProductionObject CaptureGpuImage(
        RendererBackendVulkan& backend, TerrainCompositedProductionObject object,
        std::shared_ptr<Texture> texture, TerrainRvtBudgetOwner& budget,
        const TerrainProductionObjectWriter& writer,
        const TerrainCompositedProductionAllocationQuery& allocations = {});
    static std::shared_ptr<TerrainCompositedProductionGpuResources> Restore(
        RendererBackendVulkan& backend, const TerrainCompositedProductionClosure& closure,
        const TerrainProductionObjectReader& reader, TerrainRvtBudgetOwner& budget,
        const TerrainCompositedProductionAllocationQuery& allocations = {},
        std::shared_ptr<const TerrainCompositedProductionGpuResources> prior = {},
        TerrainCompositedProductionGpuResources::WorldScope worldScope = {});
    static std::shared_ptr<TerrainCompositedProductionGpuResources> Restore(
        RendererBackendVulkan& backend, const TerrainCompositedVTStore& store,
        TerrainRvtBudgetOwner& budget,
        const TerrainCompositedProductionAllocationQuery& allocations = {},
        std::shared_ptr<const TerrainCompositedProductionGpuResources> prior = {},
        TerrainCompositedProductionGpuResources::WorldScope worldScope = {});
    static TerrainCompositedOwnedBakeInput MakeBakeInput(
        std::shared_ptr<const TerrainCompositedProductionGpuResources> owner,
        const std::string& surfaceId);

    // 纯 CPU 边界：布局为紧凑 layer-major/mip-major，BC blocks 不解码或重编码。
    static HostImage ImageLayout(const TerrainCompositedProductionImageDesc& desc);
    static HostImage DecodeImage(
        const TerrainCompositedProductionObject& object, std::vector<uint8_t> bytes);
    static void ValidatePayload(
        const TerrainCompositedProductionObject& object, const std::vector<uint8_t>& bytes);
    static HostImage SourceMapImage(const TerrainCompositedSurfaceProduction& surface);
    static void WriteInputObject(
        const std::filesystem::path& manifestDirectory,
        const TerrainCompositedProductionObject& object, const std::vector<uint8_t>& bytes);
    static TerrainRvtCost TransferCost(
        uint64_t payloadBytes, const TerrainCompositedProductionAllocationQuery& allocations);
    static TerrainCompositedSurfaceProduction FreezeSurface(
        const std::string& surfaceId, uint32_t sourceSet,
        const TerrainCompositedBakeGpu::InputSnapshot& snapshot,
        TerrainCompositedProductionBindings bindings, const std::string& runtimeMaterialInstancePath,
        const std::string& runtimeDetailMaterialInstancePath = {});
    static void PopulateCapturedImageMetadata(
        TerrainCompositedProductionObject& object, const std::vector<uint8_t>& bytes);
    static TerrainRvtCost AddAllocationCosts(TerrainRvtCost allocation, TerrainRvtCost overhead);
};

inline void TerrainCompositedProductionGpuResources::Leaf::RefreshPhase()
{
    auto target = TerrainRvtResourcePhase::Retiring;
    for (auto use = uses; use; use = use->next)
        if (use->phase < target)
            target = use->phase;
    const auto current = lease.Phase();
    if (target > current)
        lease.SetPhase(target);
    phaseFloor = std::max(target, current);
}

inline std::optional<TerrainCompositedProductionGpuResources::PhaseTransitionTicket>
TerrainCompositedProductionGpuResources::PrepareTransitions(
    const std::vector<PhaseTransition>& changes, TerrainRvtBudgetOwner& metadataOwner)
{
    if (changes.size() > TerrainRvtBudgetPhaseBatch::MaxCharges)
        return std::nullopt;
    for (const auto& change : changes)
        if (!change.use || change.phase < TerrainRvtResourcePhase::Candidate ||
            change.phase >= TerrainRvtResourcePhase::Count)
            return std::nullopt;
    const uint64_t bytes = sizeof(PhaseTransitionTicket) + 256 +
        changes.size() * (sizeof(PhaseTransition) + sizeof(std::shared_ptr<Leaf>) +
            sizeof(TerrainRvtResourcePhase) + sizeof(TerrainRvtBudgetPhaseChange) +
            sizeof(std::unique_lock<std::mutex>));
    auto charge = metadataOwner.TryReserve({0, bytes, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    if (!charge)
        return std::nullopt;
    PhaseTransitionTicket ticket(std::move(*charge));
    ticket.transitions = changes;
    std::sort(ticket.transitions.begin(), ticket.transitions.end(), [](const auto& first, const auto& second)
    { return std::less<const Use*>{}(&first.use->use, &second.use->use); });
    for (size_t index = 1; index < ticket.transitions.size(); ++index)
        if (ticket.transitions[index - 1].use == ticket.transitions[index].use &&
            ticket.transitions[index - 1].phase != ticket.transitions[index].phase)
            return std::nullopt;
    ticket.transitions.erase(std::unique(ticket.transitions.begin(), ticket.transitions.end(),
        [](const auto& first, const auto& second) { return first.use == second.use; }),
        ticket.transitions.end());
    ticket.leaves.reserve(ticket.transitions.size());
    for (const auto& change : ticket.transitions)
        ticket.leaves.push_back(change.use->use.leaf);
    std::sort(ticket.leaves.begin(), ticket.leaves.end(), [](const auto& first, const auto& second)
    { return first->allocationIdentity < second->allocationIdentity; });
    ticket.leaves.erase(std::unique(ticket.leaves.begin(), ticket.leaves.end()), ticket.leaves.end());
    ticket.changes.reserve(ticket.leaves.size());
    ticket.observedPhases.reserve(ticket.leaves.size());
    ticket.locks.reserve(ticket.leaves.size());
    for (const auto& leaf : ticket.leaves)
    {
        ticket.observedPhases.push_back(leaf->lease.Phase());
        ticket.locks.emplace_back(leaf->mutex, std::defer_lock);
    }
    return std::optional<PhaseTransitionTicket>(std::move(ticket));
}

inline void TerrainCompositedProductionGpuResources::PhaseTransitionTicket::CancelNoThrow() noexcept
{
    for (auto iterator = locks.rbegin(); iterator != locks.rend(); ++iterator)
        if (iterator->owns_lock())
            iterator->unlock();
    locked = false;
    changes.clear();
}

inline bool TerrainCompositedProductionGpuResources::PhaseTransitionTicket::TryLockLeavesNoThrow() noexcept
{
    if (finalized)
        return false;
    if (locked)
        return true;
    try
    {
        for (auto& lock : locks)
            if (!lock.try_lock())
            {
                CancelNoThrow();
                return false;
            }
        for (const auto& transition : transitions)
            if (transition.phase < transition.use->use.phase)
            {
                CancelNoThrow();
                return false;
            }
        for (size_t index = 0; index < leaves.size(); ++index)
        {
            const auto& leaf = leaves[index];
            auto target = TerrainRvtResourcePhase::Retiring;
            for (auto use = leaf->uses; use; use = use->next)
            {
                const auto found = std::lower_bound(transitions.begin(), transitions.end(), use,
                    [](const auto& transition, const Use* value)
                    { return std::less<const Use*>{}(&transition.use->use, value); });
                const auto phase = found != transitions.end() && &found->use->use == use
                    ? found->phase : use->phase;
                target = std::min(target, phase);
            }
            changes.push_back({leaf->lease,
                std::max({target, leaf->phaseFloor, observedPhases[index]})});
        }
        locked = true;
        return true;
    }
    catch (...)
    {
        CancelNoThrow();
        return false;
    }
}

inline void TerrainCompositedProductionGpuResources::PhaseTransitionTicket::FinalizeNoThrow() noexcept
{
    if (!locked || finalized)
        return;
    for (const auto& transition : transitions)
        transition.use->use.phase = transition.phase;
    for (size_t index = 0; index < leaves.size(); ++index)
        leaves[index]->phaseFloor = changes[index].phase;
    finalized = true;
    CancelNoThrow();
}

inline TerrainCompositedProductionGpuResources::Use::Use(std::shared_ptr<Leaf> owner, const Use* source)
    : leaf(std::move(owner))
{
    std::lock_guard<std::mutex> lock(leaf->mutex);
    if ((source && source->phase == TerrainRvtResourcePhase::Retiring) ||
        leaf->lease.Phase() == TerrainRvtResourcePhase::Retiring)
        throw std::logic_error("Terrain production cannot attach retiring leaf");
    next = leaf->uses;
    if (next)
        next->previous = this;
    leaf->uses = this;
}

inline TerrainCompositedProductionGpuResources::Use::~Use()
{
    std::lock_guard<std::mutex> lock(leaf->mutex);
    if (previous)
        previous->next = next;
    else
        leaf->uses = next;
    if (next)
        next->previous = previous;
    leaf->RefreshPhase();
}

inline void TerrainCompositedProductionGpuResources::Use::SetPhase(TerrainRvtResourcePhase value)
{
    std::lock_guard<std::mutex> lock(leaf->mutex);
    if (value < phase || value >= TerrainRvtResourcePhase::Count)
        throw std::logic_error("Terrain production leaf use phase cannot move backwards");
    phase = value;
    leaf->RefreshPhase();
}

inline TerrainRvtResourcePhase TerrainCompositedProductionGpuResources::PhaseUse::Phase() const
{
    std::lock_guard<std::mutex> lock(use.leaf->mutex);
    return use.phase;
}

inline std::shared_ptr<TerrainCompositedProductionGpuResources::PhaseUse>
TerrainCompositedProductionGpuResources::PhaseUse::Retain(TerrainRvtResourcePhase phase) const
{
    return std::shared_ptr<PhaseUse>(new PhaseUse(use.leaf, phase, &use));
}

inline std::shared_ptr<Texture> TerrainCompositedProductionGpuResources::PhaseUse::GetTexture() const
{
    return std::shared_ptr<Texture>(use.leaf, use.leaf->uploaded.texture.get());
}

inline TerrainCompositedProductionGpuResources::Allocation
TerrainCompositedProductionGpuResources::PhaseUse::GetAllocation() const
{
    const auto& leaf = use.leaf;
    auto cost = leaf->lease.Cost();
    cost.cpuBytes += TerrainRvtBudgetOwner::MetadataBytesPerLease;
    return {leaf->allocationIdentity, cost, leaf->uploaded.image,
        leaf->uploaded.view, leaf->uploaded.sampler};
}

inline std::shared_ptr<TerrainCompositedProductionGpuResources::PhaseUse>
TerrainCompositedProductionGpuResources::RetainImageUse(
    const std::string& id, TerrainRvtResourcePhase phase) const
{
    if (this->phase == TerrainRvtResourcePhase::Retiring)
        throw std::logic_error("Terrain production cannot retain from retiring snapshot");
    return std::shared_ptr<PhaseUse>(new PhaseUse(images.at(id)->leaf, phase));
}

inline std::shared_ptr<TerrainCompositedProductionGpuResources::PhaseUse>
TerrainCompositedProductionGpuResources::RetainSourceMapUse(
    const std::string& id, TerrainRvtResourcePhase phase) const
{
    if (this->phase == TerrainRvtResourcePhase::Retiring)
        throw std::logic_error("Terrain production cannot retain from retiring snapshot");
    return std::shared_ptr<PhaseUse>(new PhaseUse(sourceMaps.at(id)->leaf, phase));
}

inline TerrainCompositedProductionGpuResources::~TerrainCompositedProductionGpuResources() = default;

inline std::shared_ptr<Texture> TerrainCompositedProductionGpuResources::Image(
    const std::string& id) const
{
    const auto& leaf = images.at(id)->leaf;
    return std::shared_ptr<Texture>(leaf, leaf->uploaded.texture.get());
}

inline std::shared_ptr<Texture> TerrainCompositedProductionGpuResources::SourceMap(
    const std::string& id) const
{
    const auto& leaf = sourceMaps.at(id)->leaf;
    return std::shared_ptr<Texture>(leaf, leaf->uploaded.texture.get());
}

inline TerrainCompositedProductionGpuResources::Allocation
TerrainCompositedProductionGpuResources::ImageAllocation(const std::string& id) const
{
    const auto& leaf = images.at(id)->leaf;
    auto cost = leaf->lease.Cost();
    cost.cpuBytes += TerrainRvtBudgetOwner::MetadataBytesPerLease;
    return {leaf->allocationIdentity, cost, leaf->uploaded.image,
        leaf->uploaded.view, leaf->uploaded.sampler};
}

inline TerrainCompositedProductionGpuResources::Allocation
TerrainCompositedProductionGpuResources::SourceMapAllocation(const std::string& id) const
{
    const auto& leaf = sourceMaps.at(id)->leaf;
    auto cost = leaf->lease.Cost();
    cost.cpuBytes += TerrainRvtBudgetOwner::MetadataBytesPerLease;
    return {leaf->allocationIdentity, cost, leaf->uploaded.image,
        leaf->uploaded.view, leaf->uploaded.sampler};
}

inline TerrainRvtCost TerrainCompositedProductionGpuResources::SnapshotMetadataCost() const
{
    auto cost = metadataLease->Cost();
    cost.cpuBytes += TerrainRvtBudgetOwner::MetadataBytesPerLease;
    return cost;
}

inline void TerrainCompositedProductionGpuResources::InvalidateImageReuse(const std::string& id) const
{
    images.at(id)->leaf->reusable.store(false, std::memory_order_release);
}

inline void TerrainCompositedProductionGpuResources::InvalidateSourceMapReuse(const std::string& id) const
{
    sourceMaps.at(id)->leaf->reusable.store(false, std::memory_order_release);
}

inline void TerrainCompositedProductionGpuResources::SetPhase(TerrainRvtResourcePhase value)
{
    if (value < phase || value >= TerrainRvtResourcePhase::Count)
        throw std::logic_error("Terrain production snapshot phase cannot move backwards");
    phase = value;
    if (metadataLease)
        metadataLease->SetPhase(value);
    for (auto& entry : images)
        entry.second->SetPhase(value);
    for (auto& entry : sourceMaps)
        entry.second->SetPhase(value);
}

inline uint64_t TerrainCompositedProductionGpuResources::MetadataBytes(
    const TerrainCompositedProductionClosure& value)
{
    uint64_t bytes = sizeof(TerrainCompositedProductionGpuResources) + 256;
    const auto add = [&](uint64_t count)
    {
        if (count > UINT64_MAX - bytes)
            throw std::overflow_error("Terrain production metadata size overflow");
        bytes += count;
    };
    const auto text = [&](const std::string& string) { add(string.capacity() + 1); };
    const auto texts = [&](const std::vector<std::string>& strings)
    {
        add(strings.capacity() * sizeof(std::string));
        for (const auto& string : strings) text(string);
    };
    const auto& generator = value.generator;
    for (const auto* string : {&generator.evaluator, &generator.filterRule, &generator.encoding,
        &generator.compilePolicy, &generator.computeSource}) text(*string);
    texts(generator.includeSources);
    texts(generator.compiledArtifacts);
    add(value.objects.capacity() * sizeof(TerrainCompositedProductionObject));
    for (const auto& object : value.objects)
    {
        text(object.objectId);
        text(object.relativePath);
        text(object.image.format);
        add(sizeof(Use) + 256 + object.objectId.capacity() + 1);
    }
    add(value.surfaces.capacity() * sizeof(TerrainCompositedSurfaceProduction));
    for (const auto& surface : value.surfaces)
    {
        for (const auto* string : {&surface.surfaceId, &surface.runtimeMaterialInstancePath,
            &surface.runtimeDetailMaterialInstancePath}) text(*string);
        add(surface.localToGlobal.capacity() * sizeof(uint32_t));
        const auto& inputs = surface.inputs;
        for (const auto* string : {&inputs.coveragePageTable, &inputs.coverageIds,
            &inputs.coverageWeights, &inputs.localLayerWeights, &inputs.layerCatalog,
            &inputs.layerAlbedo, &inputs.layerNormal, &inputs.layerOrm, &inputs.layerHeight,
            &inputs.surfaceLocalNormal, &inputs.surfaceLocalTangent, &inputs.heightAtlas,
            &inputs.heightLayerMap, &inputs.globalColour, &inputs.globalMasks}) text(*string);
        add(surface.authorDependencies.capacity() * sizeof(TerrainCompositedAuthorDependency));
        for (const auto& dependency : surface.authorDependencies)
        {
            text(dependency.root);
            text(dependency.path);
            text(dependency.mode);
            text(dependency.digest);
            texts(dependency.productionObjectIds);
        }
        add(sizeof(Use) + 256 + surface.surfaceId.capacity() + 1);
    }
    texts(value.adjacency.SurfaceIds());
    add(value.adjacency.Links().capacity() * sizeof(TerrainCompositedAdjacencyLink));
    for (const auto& link : value.adjacency.Links())
    {
        text(link.sourceSurfaceId);
        text(link.targetSurfaceId);
    }
    add(value.adjacency.StaticCharts().capacity() * sizeof(TerrainCompositedAdjacencyGraph::StaticChart));
    return bytes;
}

inline std::shared_ptr<TerrainCompositedProductionGpuResources>
TerrainCompositedProductionGpuResources::RestoreImages(
    const TerrainCompositedProductionClosure& closure, const TerrainProductionObjectReader& reader,
    TerrainRvtBudgetOwner& budget, const TerrainCompositedProductionAllocationQuery& allocations,
    const ImageUploader& upload, std::shared_ptr<const TerrainCompositedProductionGpuResources> prior,
    WorldScope worldScope)
{
    closure.Validate();
    if (!reader || !upload || !allocations.image || !allocations.transfer)
        throw std::runtime_error("Terrain production requires reader/uploader/actual allocation queries");
    auto metadata = budget.TryReserve({0, MetadataBytes(closure), 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Candidate);
    if (!metadata)
        throw std::runtime_error("Terrain production snapshot metadata budget exceeded");
    auto result = std::shared_ptr<TerrainCompositedProductionGpuResources>(
        new TerrainCompositedProductionGpuResources);
    result->metadataLease = std::move(metadata);
    result->closure = closure;
    result->budgetIdentity = budget.IdentityToken();
    result->worldScope = std::move(worldScope);
    const bool reuse = prior && result->worldScope.use_count() > 0 && prior->worldScope.use_count() > 0 &&
        !result->worldScope.owner_before(prior->worldScope) &&
        !prior->worldScope.owner_before(result->worldScope) &&
        result->budgetIdentity.use_count() > 0 && prior->budgetIdentity.use_count() > 0 &&
        !result->budgetIdentity.owner_before(prior->budgetIdentity) &&
        !prior->budgetIdentity.owner_before(result->budgetIdentity) &&
        prior->phase != TerrainRvtResourcePhase::Retiring;
    std::set<std::string> arrays;
    std::set<std::string> planes;
    for (const auto& surface : closure.surfaces)
    {
        const auto& bindings = surface.inputs;
        for (const auto& id : {bindings.layerAlbedo, bindings.layerNormal, bindings.layerOrm,
            bindings.layerHeight, bindings.localLayerWeights, bindings.heightAtlas})
            if (!id.empty()) arrays.insert(id);
        for (const auto& id : {bindings.coveragePageTable, bindings.coverageIds, bindings.coverageWeights,
            bindings.layerCatalog, bindings.surfaceLocalNormal, bindings.surfaceLocalTangent,
            bindings.heightLayerMap, bindings.globalColour, bindings.globalMasks})
            if (!id.empty()) planes.insert(id);
    }
    for (const auto& id : arrays)
        if (planes.count(id))
            throw std::runtime_error("Terrain production object bound as both 2D and array");
    const auto matches = [](const Leaf& leaf, const TerrainCompositedProductionObject& object, bool array)
    {
        const auto& old = leaf.object;
        const auto& before = old.image;
        const auto& after = object.image;
        return leaf.reusable.load(std::memory_order_acquire) &&
            leaf.lease.Phase() != TerrainRvtResourcePhase::Retiring &&
            leaf.array == array && old.objectId == object.objectId &&
            old.relativePath == object.relativePath && old.kind == object.kind &&
            old.bytes == object.bytes && old.digest == object.digest &&
            old.baseCatalogDigest == object.baseCatalogDigest &&
            old.heightBlendCatalogDigest == object.heightBlendCatalogDigest &&
            before.format == after.format && before.width == after.width &&
            before.height == after.height && before.layers == after.layers &&
            before.mipLevels == after.mipLevels && before.transfer == after.transfer &&
            before.address == after.address && before.filter == after.filter;
    };
    const auto admit = [&](auto& target, const auto& oldEntries, const std::string& key,
        const TerrainCompositedProductionObject& object, bool array)
    {
        if (!array && object.image.layers != 1)
            throw std::runtime_error("Terrain production 2D object has array layers");
        std::shared_ptr<Leaf> leaf;
        if (reuse)
        {
            const auto found = oldEntries.find(key);
            if (found != oldEntries.end() && matches(*found->second->leaf, object, array))
                leaf = found->second->leaf;
        }
        if (!leaf)
        {
            auto cost = allocations.image(object.image, array);
            if (cost.gpuBytes < object.image.ByteSize() || cost.physicalSlots || cost.productionTasks)
                throw std::runtime_error("Terrain production incomplete actual image allocation quote");
            cost = TerrainCompositedProductionGpu::AddAllocationCosts(cost,
                {0, sizeof(Leaf) + 128 + object.objectId.capacity() + 1 +
                    object.relativePath.capacity() + 1 + object.image.format.capacity() + 1, 0, 0});
            auto charge = budget.TryReserve(cost, TerrainRvtCostClass::Input,
                TerrainRvtResourcePhase::Candidate);
            if (!charge)
                throw std::runtime_error("Terrain production frozen World budget exceeded");
            leaf = std::make_shared<Leaf>(std::move(*charge), object, array);
        }
        target.emplace(key, std::make_unique<Use>(std::move(leaf)));
    };
    for (const auto& object : closure.objects)
        if (object.kind == TerrainCompositedProductionObjectKind::Image)
            admit(result->images, prior ? prior->images : result->images,
                object.objectId, object, arrays.count(object.objectId) != 0);
    TerrainCompositedProductionImageDesc sourceMapDesc;
    sourceMapDesc.format = "rgba32-float";
    sourceMapDesc.width = 256;
    sourceMapDesc.height = 2;
    const auto sourceMapObject = [&](const auto& surface)
    {
        TerrainCompositedProductionObject object;
        object.objectId = surface.surfaceId;
        object.image = sourceMapDesc;
        auto workspace = budget.TryReserve({0, sourceMapDesc.ByteSize(), 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        if (!workspace)
            throw std::runtime_error("Terrain production source map metadata budget exceeded");
        const auto image = TerrainCompositedProductionGpu::SourceMapImage(surface);
        TerrainCompositedProductionGpu::PopulateCapturedImageMetadata(object, image.data);
        return object;
    };
    for (const auto& surface : closure.surfaces)
        admit(result->sourceMaps, prior ? prior->sourceMaps : result->sourceMaps,
            surface.surfaceId, sourceMapObject(surface), false);
    const auto restore = [&](Leaf& leaf, const auto& read)
    {
        if (leaf.uploaded.texture)
            return;
        const auto cost = TerrainCompositedProductionGpu::TransferCost(leaf.object.bytes, allocations);
        auto workspace = budget.TryReserve(cost, TerrainRvtCostClass::Workspace,
            TerrainRvtResourcePhase::Candidate);
        if (!workspace)
            throw std::runtime_error("Terrain production transfer budget exceeded");
        const auto image = TerrainCompositedProductionGpu::DecodeImage(leaf.object, read());
        leaf.uploaded = upload(image, leaf.object.image, leaf.array);
        if (!leaf.uploaded.texture)
            throw std::runtime_error("Terrain production uploader returned null texture");
    };
    for (const auto& object : closure.objects)
        if (object.kind == TerrainCompositedProductionObjectKind::Image)
            restore(*result->images.at(object.objectId)->leaf, [&] { return reader(object); });
    for (const auto& surface : closure.surfaces)
        restore(*result->sourceMaps.at(surface.surfaceId)->leaf,
            [&] { return TerrainCompositedProductionGpu::SourceMapImage(surface).data; });
    return result;
}

// CPU 转换内联使模块测试无需链接 backend/Texture，GPU 实现仍在独立 cpp。
inline HostImage TerrainCompositedProductionGpu::ImageLayout(
    const TerrainCompositedProductionImageDesc& desc)
{
    const uint16_t endian = 1;
    if (*reinterpret_cast<const uint8_t*>(&endian) != 1 ||
        desc.ByteSize() > std::numeric_limits<size_t>::max())
        throw std::runtime_error("Terrain production image requires addressable little-endian storage");
    static const std::map<std::string, HostImage::PixelFormat> formats{
        {"r8-unorm", HostImage::PixelFormat::R8_UNORM},
        {"rg8-unorm", HostImage::PixelFormat::RG8_UNORM},
        {"rgba8-unorm", HostImage::PixelFormat::RGBA8_UNORM},
        {"rgba8-srgb", HostImage::PixelFormat::RGBA8_SRGB},
        {"r16-unorm", HostImage::PixelFormat::R16_UNORM},
        {"r32-float", HostImage::PixelFormat::R32_FLOAT},
        {"rgba32-float", HostImage::PixelFormat::RGBA32_FLOAT},
        {"bc5-unorm", HostImage::PixelFormat::BC5_UNORM},
        {"bc7-unorm", HostImage::PixelFormat::BC7_UNORM},
        {"bc7-srgb", HostImage::PixelFormat::BC7_SRGB}};
    HostImage image;
    image.width = desc.width;
    image.height = desc.height;
    image.arrayLayers = desc.layers;
    image.mipLevels = desc.mipLevels;
    image.format = formats.at(desc.format);
    image.channels = desc.format == "r8-unorm" || desc.format == "r16-unorm" ||
        desc.format == "r32-float" ? 1 : desc.format == "rg8-unorm" ? 2 : 4;
    image.semantic = HostImage::TextureSemantic::Data;
    if (image.GetTotalBytes() != desc.ByteSize())
        throw std::runtime_error("Terrain production CPU/P1 image layout mismatch");
    return image;
}

inline void TerrainCompositedProductionGpu::ValidatePayload(
    const TerrainCompositedProductionObject& object, const std::vector<uint8_t>& bytes)
{
    if (object.kind != TerrainCompositedProductionObjectKind::Image ||
        object.bytes != object.image.ByteSize() || object.bytes != bytes.size())
        throw std::runtime_error("Terrain production image kind/size/digest mismatch");
    ValidateTerrainCompositedProductionPayload(object, bytes);
}

inline HostImage TerrainCompositedProductionGpu::DecodeImage(
    const TerrainCompositedProductionObject& object, std::vector<uint8_t> bytes)
{
    ValidatePayload(object, bytes);
    auto image = ImageLayout(object.image);
    image.data = std::move(bytes);
    image.ValidateStorage();
    return image;
}

inline TerrainCompositedProductionObject TerrainCompositedProductionGpu::CaptureCpuImage(
    TerrainCompositedProductionObject object, const HostImage& image,
    TerrainRvtBudgetOwner& budget, const TerrainProductionObjectWriter& writer)
{
    if (!writer || object.kind != TerrainCompositedProductionObjectKind::Image)
        throw std::runtime_error("Terrain production requires image object and writer");
    auto packed = ImageLayout(object.image);
    image.ValidateStorage();
    if (image.data.empty() || image.width != packed.width || image.height != packed.height ||
        image.arrayLayers != packed.arrayLayers || image.mipLevels != packed.mipLevels ||
        image.format != packed.format || image.swizzle.r != 0 || image.swizzle.g != 1 ||
        image.swizzle.b != 2 || image.swizzle.a != 3)
        throw std::runtime_error("Terrain production CPU snapshot metadata mismatch");
    auto workspace = budget.TryReserve({0, object.image.ByteSize(), 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    if (!workspace)
        throw std::runtime_error("Terrain production frozen World budget exceeded");
    packed.data.resize(static_cast<size_t>(object.image.ByteSize()));
    // 去掉 CPU 行 padding；保持 GPU 行朝向，不重复材质入口的 flipY。
    for (uint32_t layer = 0; layer < image.arrayLayers; ++layer)
        for (uint32_t mip = 0; mip < image.mipLevels; ++mip)
            for (size_t row = 0; row < image.GetMipRowCount(mip); ++row)
                std::memcpy(static_cast<uint8_t*>(packed.GetMipData(mip, layer)) +
                    row * packed.GetMipRowStride(mip),
                    static_cast<const uint8_t*>(image.GetMipData(mip, layer)) +
                    row * image.GetMipRowStride(mip), packed.GetMipRowStride(mip));
    PopulateCapturedImageMetadata(object, packed.data);
    writer(object, packed.data);
    return object;
}

inline HostImage TerrainCompositedProductionGpu::SourceMapImage(
    const TerrainCompositedSurfaceProduction& surface)
{
    if (surface.localToGlobal.size() > 256)
        throw std::runtime_error("Terrain production local layer count exceeds source map");
    HostImage image;
    image.width = 256;
    image.height = 2;
    image.format = HostImage::PixelFormat::RGBA32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.data.resize(256 * 2 * 16, 0);
    for (uint32_t global = 0; global < 256; ++global)
    {
        const auto slice = surface.globalToSourceSlice[global];
        const std::array<float, 4> row{slice == UINT32_MAX ? 0.f : float(slice),
            slice == UINT32_MAX ? 0.f : 1.f, 0, 0};
        std::memcpy(image.data.data() + size_t(global) * 16, row.data(), 16);
    }
    for (size_t local = 0; local < surface.localToGlobal.size(); ++local)
    {
        const auto global = surface.localToGlobal[local];
        if (global >= 256)
            throw std::runtime_error("Terrain production invalid global layer in source map");
        const std::array<float, 4> row{float(global), 1.f, 0, 0};
        std::memcpy(image.data.data() + (256 + local) * 16, row.data(), 16);
    }
    return image;
}

inline void TerrainCompositedProductionGpu::WriteInputObject(
    const std::filesystem::path& manifestDirectory,
    const TerrainCompositedProductionObject& object, const std::vector<uint8_t>& bytes)
{
    try
    {
        if (object.kind == TerrainCompositedProductionObjectKind::Image)
            ValidatePayload(object, bytes);
        else
        {
            if ((object.kind != TerrainCompositedProductionObjectKind::ShaderSource &&
                object.kind != TerrainCompositedProductionObjectKind::ShaderArtifact &&
                object.kind != TerrainCompositedProductionObjectKind::RawAuthorSource) ||
                !object.image.format.empty() || object.image.width || object.image.height ||
                object.image.layers != 1 || object.image.mipLevels != 1 ||
                object.image.transfer != TerrainCompositedProductionTransfer::Linear ||
                object.image.address != TerrainCompositedProductionAddress::Clamp ||
                object.image.filter != TerrainCompositedProductionFilter::Nearest ||
                object.baseCatalogDigest != ContentDigest{} ||
                object.heightBlendCatalogDigest != ContentDigest{} || object.bytes == 0)
                throw std::runtime_error("unsupported kind or shader carries image/catalog metadata");
            ValidateTerrainCompositedProductionPayload(object, bytes);
        }
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Terrain production immutable object id=" + object.objectId +
            " path=" + object.relativePath + " kind=" + std::to_string(static_cast<uint32_t>(object.kind)) +
            " format=" + object.image.format + " dimensions=" + std::to_string(object.image.width) +
            "x" + std::to_string(object.image.height) + " layers=" + std::to_string(object.image.layers) +
            " mips=" + std::to_string(object.image.mipLevels) + " declaredBytes=" +
            std::to_string(object.bytes) + " actualBytes=" + std::to_string(bytes.size()) +
            " expectedDigest=" + object.digest.ToHex() + ": " + error.what());
    }
    const auto& name = object.relativePath;
    const std::filesystem::path relative(name);
    if (name.empty() || name.find(':') != std::string::npos ||
        name.find('\0') != std::string::npos || name.find('\\') != std::string::npos ||
        relative.is_absolute() || relative.has_root_path())
        throw std::runtime_error("Terrain production input path must be package relative");
    for (const auto& part : relative)
        if (part == ".." || part == ".")
            throw std::runtime_error("Terrain production input path traversal");
    const auto root = std::filesystem::canonical(manifestDirectory);
    const auto target = root / relative;
    // 发布只创建不可变对象；既有对象不覆盖，避免旧 manifest 被原地修改。
    auto parent = root;
    for (const auto& part : relative.parent_path())
    {
        parent /= part;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(parent)))
            throw std::runtime_error("Terrain production input parent is a link");
        const auto resolved = std::filesystem::weakly_canonical(parent).lexically_relative(root);
        if (resolved.empty() || resolved.is_absolute())
            throw std::runtime_error("Terrain production input parent escapes package");
        for (const auto& component : resolved)
            if (component == "..")
                throw std::runtime_error("Terrain production input parent escapes package");
    }
    const auto status = std::filesystem::symlink_status(target);
    if (std::filesystem::is_symlink(status))
        throw std::runtime_error("Terrain production input target is a link");
    if (std::filesystem::exists(status))
    {
        if (!std::filesystem::is_regular_file(status) ||
            std::filesystem::file_size(target) != bytes.size() ||
            ContentHasher::HashFile(target) != object.digest)
            throw std::runtime_error("Terrain production immutable object collision");
        return;
    }
    WriteBinaryFileAtomically(target, bytes);
}

inline TerrainRvtCost TerrainCompositedProductionGpu::TransferCost(
    uint64_t payloadBytes, const TerrainCompositedProductionAllocationQuery& allocations)
{
    if (!allocations.transfer || payloadBytes == 0)
        throw std::runtime_error("Terrain production requires actual staging allocation query");
    const auto stagingBytes = std::min(payloadBytes, MaxTransferBytes);
    auto cost = allocations.transfer(stagingBytes);
    if (cost.gpuBytes < stagingBytes || cost.cpuBytes < stagingBytes ||
        cost.physicalSlots || cost.productionTasks ||
        payloadBytes > UINT64_MAX - TransferScratchCpuBytes ||
        cost.cpuBytes > UINT64_MAX - payloadBytes - TransferScratchCpuBytes)
        throw std::runtime_error("Terrain production staging allocation quote is incomplete");
    cost.cpuBytes += payloadBytes + TransferScratchCpuBytes;
    return cost;
}

inline void TerrainCompositedProductionGpu::VisitTransferChunks(
    const TerrainCompositedProductionImageDesc& desc,
    const std::function<void(const TerrainCompositedProductionTransferChunk&)>& visit)
{
    if (!visit)
        throw std::runtime_error("Terrain production requires transfer chunk visitor");
    const auto image = ImageLayout(desc);
    const uint32_t blockHeight = HostImage::IsBlockCompressed(image.format) ? 4 : 1;
    uint64_t subresourceOffset = 0;
    for (uint32_t layer = 0; layer < image.arrayLayers; ++layer)
        for (uint32_t mip = 0; mip < image.mipLevels; ++mip)
        {
            const uint32_t stride = image.GetMipRowStride(mip);
            if (stride == 0 || stride > MaxTransferBytes)
                throw std::runtime_error("Terrain production block row exceeds transfer limit");
            const uint32_t rowsPerChunk = static_cast<uint32_t>(MaxTransferBytes / stride);
            const auto rows = image.GetMipRowCount(mip);
            const uint32_t height = image.GetMipHeight(mip);
            for (uint32_t firstRow = 0; firstRow < rows; )
            {
                const auto count = static_cast<uint32_t>(std::min<uint64_t>(rowsPerChunk, rows - firstRow));
                TerrainCompositedProductionTransferChunk chunk;
                chunk.rawOffset = subresourceOffset + uint64_t(firstRow) * stride;
                chunk.bytes = uint64_t(count) * stride;
                chunk.copy.setBufferOffset(0).setBufferRowLength(0).setBufferImageHeight(0)
                    .setImageSubresource({vk::ImageAspectFlagBits::eColor, mip, layer, 1})
                    .setImageOffset({0, static_cast<int32_t>(firstRow * blockHeight), 0})
                    .setImageExtent({image.GetMipWidth(mip),
                        std::min(count * blockHeight, height - firstRow * blockHeight), 1});
                visit(chunk);
                firstRow += count;
            }
            subresourceOffset += image.GetMipLayerBytes(mip);
        }
    if (subresourceOffset != desc.ByteSize())
        throw std::runtime_error("Terrain production transfer layout mismatch");
}

inline TerrainCompositedSurfaceProduction TerrainCompositedProductionGpu::FreezeSurface(
    const std::string& surfaceId, uint32_t sourceSet,
    const TerrainCompositedBakeGpu::InputSnapshot& snapshot,
    TerrainCompositedProductionBindings bindings, const std::string& runtimeMaterialInstancePath,
    const std::string& runtimeDetailMaterialInstancePath)
{
    if (snapshot.sourceSetCount != 1 || snapshot.localLayerCount == 0 ||
        snapshot.localLayerCount > 256 || snapshot.hasUnsupportedViewTimeOrTransparentTerms ||
        (snapshot.weightSource != TerrainCompositedBakeGpu::WeightSource::LocalLayerArray &&
            snapshot.weightSource != TerrainCompositedBakeGpu::WeightSource::CoverageAtlas))
        throw std::runtime_error("Terrain production unsupported author snapshot");
    TerrainCompositedSurfaceProduction surface;
    surface.surfaceId = surfaceId;
    surface.runtimeMaterialInstancePath = runtimeMaterialInstancePath;
    surface.runtimeDetailMaterialInstancePath = runtimeDetailMaterialInstancePath;
    surface.sourceSet = sourceSet;
    surface.inputs = std::move(bindings);
    surface.weightSource = snapshot.weightSource == TerrainCompositedBakeGpu::WeightSource::LocalLayerArray
        ? TerrainCompositedWeightSource::LocalLayerArray : TerrainCompositedWeightSource::CoverageAtlas;
    surface.localToGlobal.assign(snapshot.localToGlobalLayer.begin(),
        snapshot.localToGlobalLayer.begin() + snapshot.localLayerCount);
    surface.globalToSourceSlice = snapshot.globalToSourceSlice;
    for (size_t index = 0; index < 16; ++index)
        surface.localToWorld[index] = snapshot.localToWorld[index];
    surface.sampling.coveragePageResolution = snapshot.coveragePageResolution;
    for (size_t axis = 0; axis < 2; ++axis)
    {
        surface.sampling.surfaceUvOffset[axis] = snapshot.surfaceUvOffset[axis];
        surface.sampling.surfaceUvScale[axis] = snapshot.surfaceUvScale[axis];
        surface.sampling.coverageUvOffset[axis] = snapshot.coverageUvOffset[axis];
        surface.sampling.coverageUvScale[axis] = snapshot.coverageUvScale[axis];
        surface.sampling.frameUvOffset[axis] = snapshot.frameUvOffset[axis];
        surface.sampling.frameUvScale[axis] = snapshot.frameUvScale[axis];
    }
    surface.parameters.uvScale = snapshot.uvScale;
    surface.parameters.heightBlendStrength = snapshot.retainedFragment.heightBlendStrength;
    surface.parameters.frozenHeightBlendFade = snapshot.frozenHeightBlendFade;
    surface.parameters.retainFragmentPostprocess = snapshot.retainFragmentPostprocess;
    return surface;
}

inline void TerrainCompositedProductionGpu::PopulateCapturedImageMetadata(
    TerrainCompositedProductionObject& object, const std::vector<uint8_t>& bytes)
{
    object.bytes = bytes.size();
    object.digest = ContentHasher::HashBytes(bytes.data(), bytes.size());
    object.baseCatalogDigest = {};
    object.heightBlendCatalogDigest = {};
    const auto& image = object.image;
    if (image.format == "rgba32-float" && image.width == 256 && image.height == 4 &&
        image.layers == 1 && image.mipLevels == 1)
        PopulateTerrainCompositedCatalogDigests(object, bytes);
    ValidatePayload(object, bytes);
}

inline TerrainRvtCost TerrainCompositedProductionGpu::AddAllocationCosts(
    TerrainRvtCost allocation, TerrainRvtCost overhead)
{
    if (allocation.physicalSlots || allocation.productionTasks || overhead.physicalSlots ||
        overhead.productionTasks || allocation.gpuBytes > UINT64_MAX - overhead.gpuBytes ||
        allocation.cpuBytes > UINT64_MAX - overhead.cpuBytes)
        throw std::runtime_error("Terrain production invalid/overflowing allocation cost");
    allocation.gpuBytes += overhead.gpuBytes;
    allocation.cpuBytes += overhead.cpuBytes;
    return allocation;
}
}
