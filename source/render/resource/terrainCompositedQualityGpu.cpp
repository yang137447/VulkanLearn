#include "render/resource/terrainCompositedQualityGpu.h"

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "texture.h"

#include <limits>
#include <cstdlib>
#include <mutex>
#include <ostream>
#include <utility>
#include <nlohmann/json.hpp>

namespace VL
{
namespace
{
namespace Abi = TerrainQualityAbi;
constexpr uint32_t SelectorBytes = 4;
std::mutex qualityCaptureMutex;
std::weak_ptr<const void> qualityCaptureGate;
double qualityLastCaptureMs = -std::numeric_limits<double>::infinity();
uint64_t qualityRecordedCaptures = 0;
uint64_t qualityExportedCaptures = 0;
bool qualityExportFailed = false;

void RequireQuality(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void QualityBarrier(vk::CommandBuffer command, vk::PipelineStageFlags from,
    vk::PipelineStageFlags to, vk::AccessFlags source, vk::AccessFlags destination)
{
    vk::MemoryBarrier barrier;
    barrier.setSrcAccessMask(source).setDstAccessMask(destination);
    command.pipelineBarrier(from, to, {}, barrier, nullptr, nullptr);
}

void Write64(std::array<uint32_t, Abi::HeaderWords>& header, uint32_t offset, uint64_t value)
{
    header[offset] = uint32_t(value);
    header[offset + 1] = uint32_t(value >> 32);
}
}

struct TerrainCompositedQualityGpu::Impl
{
    struct Allocation
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
    };
    RendererBackendVulkan& backend;
    std::shared_ptr<TerrainRvtBudgetOwner> budget;
    TerrainCompositedQualityLimits limits;
    std::optional<TerrainRvtBudgetLease> lease;
    std::shared_ptr<ComputePipeline> pipeline;
    vk::DescriptorPool descriptorPool;
    std::vector<vk::DescriptorSet> descriptors;
    Allocation output;
    Allocation host;
    std::array<Allocation, Abi::Surfaces> selectors{};
    void* mapped = nullptr;
    std::vector<uint32_t> cpuReadback;
    vk::QueryPool queries;
    std::shared_ptr<const TerrainCompositedQualitySnapshot> snapshot;
    std::array<TerrainCompositedQualityDepthBinding, 2> winnerAttachments;
    std::weak_ptr<const TerrainCompositedQualityReadback> outstanding;
    std::shared_ptr<const void> captureGate;
    uint64_t handle = 0;
    uint64_t submittedEpoch = 0;
    uint64_t requested = 0;
    uint64_t deferred = 0;
    uint64_t completed = 0;
    uint64_t lastExportedHandle = 0;
    uint64_t exportedCaptures = 0;
    uint64_t gpuBytes = 0;
    uint64_t hostBytes = 0;
    TerrainCompositedQualityAllocationQuote allocationQuote;
    uint64_t workspaceObservedPeakBytes = 0;
    double captureMs = 0;
    double lastCaptureMs = -std::numeric_limits<double>::infinity();
    bool copied = false;
    bool exportFailed = false;

    Impl(RendererBackendVulkan& backend, std::shared_ptr<TerrainRvtBudgetOwner> budget,
        TerrainCompositedQualityLimits limits)
        : backend(backend), budget(std::move(budget)), limits(limits) {}

    ~Impl()
    {
        if (mapped)
            backend.UnmapMemory(host.memory);
        backend.DestroyBuffer(output.buffer, output.memory);
        backend.DestroyBuffer(host.buffer, host.memory);
        for (auto& selector : selectors)
            backend.DestroyBuffer(selector.buffer, selector.memory);
        if (descriptorPool)
            backend.DestroyDescriptorPool(descriptorPool);
        if (queries)
            backend.DestroyQueryPool(queries);
    }

    void Check(Handle expected) const
    {
        RequireQuality(snapshot && expected == handle, "Quality handle is not current");
    }

    void ObserveWorkspace()
    {
        workspaceObservedPeakBytes = std::max(workspaceObservedPeakBytes,
            budget->Statistics().byClass[static_cast<size_t>(TerrainRvtCostClass::Workspace)].gpuBytes);
    }
};

TerrainCompositedQualityGpu::TerrainCompositedQualityGpu(
    RendererBackendVulkan& backend, PipelineFactory& factory,
    std::shared_ptr<TerrainRvtBudgetOwner> budget, TerrainCompositedQualityLimits limits)
    : impl(std::make_shared<Impl>(backend, std::move(budget), limits))
{
    auto& state = *impl;
    if (const char* captureLimit = std::getenv("VULKANLEARN_RVT_QUALITY_MAX_CAPTURES"))
    {
        char* end = nullptr;
        const auto requestedLimit = std::strtoul(captureLimit, &end, 10);
        RequireQuality(end != captureLimit && *end == '\0' && requestedLimit > 0 &&
            requestedLimit <= limits.maxCaptures, "Quality capture limit must reduce the admitted run cap");
        limits.maxCaptures = uint32_t(requestedLimit);
        limits.maxReadbackBytes = std::min(limits.maxReadbackBytes,
            uint64_t(Abi::Bytes) * requestedLimit);
        state.limits = limits;
    }
    RequireQuality(bool(state.budget) && limits.maxCaptures &&
        limits.maxReadbackBytes >= Abi::Bytes &&
        limits.metadataOverhead.gpuBytes && limits.metadataOverhead.cpuBytes >=
            sizeof(Impl) + sizeof(TerrainCompositedQualitySnapshot) + 65536 &&
        !limits.metadataOverhead.physicalSlots && !limits.metadataOverhead.productionTasks,
        "Quality requires explicit frozen allocation/metadata admission");
    RequireQuality(backend.SupportsTimestampQueries(), "Quality needs actual GPU timestamps");
    const auto deviceLimits = backend.GetSelectedDeviceLimits();
    RequireQuality(deviceLimits.maxColorAttachments >= 7 &&
        deviceLimits.maxFragmentOutputAttachments >= 7,
        "Quality winner requires seven device color outputs");
    RequireQuality(limits.importedWinnerGpuBytes && !limits.importedWinnerOwners.empty(),
        "Quality winner images require queried imported Workspace admission");
    const auto outputUsage = vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
    const auto selectorUsage = vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst;
    const auto quote = QueryAllocation(backend, limits);
    state.allocationQuote = quote;
    state.hostBytes = quote.hostBytes;
    state.gpuBytes = quote.outputBytes + quote.hostBytes + Abi::Surfaces * quote.selectorBytes;
    state.lease = state.budget->TryReserve(quote.total,
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    RequireQuality(bool(state.lease), "Quality capture exceeds frozen Workspace/CPU budget");
    state.ObserveWorkspace();
    state.cpuReadback.resize(Abi::Bytes / sizeof(uint32_t));
    state.pipeline = factory.CreateComputePipelineCandidate("terrain/terrainCompositedQuality");
    RequireQuality(state.pipeline && !state.pipeline->GetDescriptorSetLayouts().empty(),
        "Quality compute pipeline unavailable");
    auto create = [&](vk::DeviceSize bytes, vk::BufferUsageFlags usage,
        vk::MemoryPropertyFlags properties, const char* name)
    {
        const auto allocation = backend.CreateBuffer(bytes, usage, properties, name);
        return Impl::Allocation{allocation.first, allocation.second};
    };
    state.output = create(Abi::Bytes, outputUsage, vk::MemoryPropertyFlagBits::eDeviceLocal,
        "TerrainQualityOutput");
    state.host = create(Abi::Bytes, vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        "TerrainQualityReadback");
    state.mapped = backend.MapMemory(state.host.memory, Abi::Bytes);
    for (auto& selector : state.selectors)
        selector = create(SelectorBytes, selectorUsage, vk::MemoryPropertyFlagBits::eDeviceLocal,
            "TerrainQualitySelector");
    const std::array<vk::DescriptorPoolSize, 2> sizes{
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, Abi::Surfaces * 2),
        vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, Abi::Surfaces * 4)};
    vk::DescriptorPoolCreateInfo pool;
    pool.setPoolSizes(sizes).setMaxSets(Abi::Surfaces)
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
    state.descriptorPool = backend.CreateDescriptorPool(pool, "TerrainQualityDescriptors");
    const std::vector<vk::DescriptorSetLayout> layouts(
        Abi::Surfaces, state.pipeline->GetDescriptorSetLayouts()[0]);
    vk::DescriptorSetAllocateInfo allocate;
    allocate.setDescriptorPool(state.descriptorPool).setSetLayouts(layouts);
    state.descriptors.resize(Abi::Surfaces);
    backend.AllocateDescriptorSets(allocate, state.descriptors);
    state.queries = backend.CreateTimestampQueryPool(2, "TerrainQualityTimestamps");
}

TerrainCompositedQualityGpu::~TerrainCompositedQualityGpu() = default;

TerrainCompositedQualityAllocationQuote TerrainCompositedQualityGpu::QueryAllocation(
    RendererBackendVulkan& backend, const TerrainCompositedQualityLimits& limits)
{
    TerrainCompositedQualityAllocationQuote quote;
    quote.outputBytes = backend.QueryBufferMemoryRequirements(Abi::Bytes,
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
        vk::BufferUsageFlagBits::eTransferDst).size;
    quote.hostBytes = backend.QueryBufferMemoryRequirements(
        Abi::Bytes, vk::BufferUsageFlagBits::eTransferDst).size;
    quote.selectorBytes = backend.QueryBufferMemoryRequirements(SelectorBytes,
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst).size;
    auto add = [](uint64_t current, uint64_t extra)
    {
        RequireQuality(current <= UINT64_MAX - extra, "Quality allocation quote overflow");
        return current + extra;
    };
    uint64_t gpu = add(quote.outputBytes, quote.hostBytes);
    for (uint32_t index = 0; index < Abi::Surfaces; ++index)
        gpu = add(gpu, quote.selectorBytes);
    quote.total.gpuBytes = add(add(gpu, limits.metadataOverhead.gpuBytes),
        limits.importedWinnerGpuBytes);
    quote.total.cpuBytes = add(add(quote.hostBytes, limits.metadataOverhead.cpuBytes), Abi::Bytes);
    return quote;
}

std::optional<TerrainCompositedQualityGpu::Handle> TerrainCompositedQualityGpu::Record(
    vk::CommandBuffer command, std::shared_ptr<const TerrainCompositedQualitySnapshot> snapshot,
    double monotonicMs,
    const std::array<TerrainCompositedQualityDepthBinding, 2>& winnerAttachments)
{
    auto& state = *impl;
    ++state.requested;
    state.ObserveWorkspace();
    RequireQuality(std::isfinite(monotonicMs) && monotonicMs >= 0, "Quality time is invalid");
    std::lock_guard<std::mutex> lock(qualityCaptureMutex);
    if (qualityExportFailed || state.exportFailed ||
        state.exportedCaptures >= state.limits.maxCaptures ||
        qualityExportedCaptures >= state.limits.maxCaptures ||
        state.snapshot || !state.outstanding.expired() ||
        monotonicMs - state.lastCaptureMs < 250 ||
        !qualityCaptureGate.expired() || monotonicMs - qualityLastCaptureMs < 250)
    {
        ++state.deferred;
        return std::nullopt;
    }
    RequireQuality(snapshot && snapshot->drawPageTableOwner &&
        !snapshot->surfaces.empty() && snapshot->surfaces.size() <= Abi::Surfaces &&
        snapshot->drawSubmissionEpoch && snapshot->renderWidth && snapshot->renderHeight,
        "Quality capture requires actual immutable draw/page-table/depth bindings");
    const auto& depth = snapshot->finalFeedbackDepth;
    RequireQuality(TerrainQualityDepthReadable(depth),
        "Quality requires borrowed sampled 128x128 D32 final depth and its strong graph package owner");
    for (const auto& winner : winnerAttachments)
        RequireQuality(winner.graphDepthOwner && winner.image && winner.descriptor.imageView &&
            winner.descriptor.sampler && winner.extent.width == Abi::Extent &&
            winner.extent.height == Abi::Extent &&
            winner.format == vk::Format::eR32G32B32A32Uint &&
            winner.samples == vk::SampleCountFlagBits::e1 &&
            bool(winner.usage & vk::ImageUsageFlagBits::eSampled) &&
            bool(winner.usage & vk::ImageUsageFlagBits::eColorAttachment) &&
            winner.descriptor.imageLayout == vk::ImageLayout::eShaderReadOnlyOptimal,
            "Quality requires exact graph-owned sampled RGBA32Uint winner attachments");
    for (uint32_t index = 0; index < snapshot->surfaces.size(); ++index)
    {
        const auto& surface = snapshot->surfaces[index];
        const auto& data = surface.data;
        uint32_t maximum = 0;
        for (uint32_t dimension = std::max(data.width, data.height); dimension > 1; dimension >>= 1)
            ++maximum;
        RequireQuality(surface.pageTable && !surface.surfaceId.empty() &&
            surface.surfaceId.size() <= 256 && data.index == index &&
            data.width && data.height && data.width <= 8192 && data.height <= 8192 &&
            (data.maxMipAndRepeat & 255u) == maximum &&
            Abi::Join(data.generationLow, data.generationHigh) == snapshot->worldGeneration,
            "Quality surface exceeds fixed ABI or differs from draw generation");
    }
    state.winnerAttachments = winnerAttachments;
    state.snapshot = std::move(snapshot);
    struct RecordingRollback
    {
        Impl& state;
        bool complete = false;
        ~RecordingRollback()
        {
            if (!complete)
            {
                state.snapshot.reset();
                state.captureGate.reset();
                state.copied = false;
            }
        }
    } rollback{state};
    state.captureMs = monotonicMs;
    state.lastCaptureMs = monotonicMs;
    state.copied = false;
    state.handle = ++qualityRecordedCaptures;
    qualityLastCaptureMs = monotonicMs;
    state.captureGate = std::make_shared<const uint64_t>(state.handle);
    qualityCaptureGate = state.captureGate;
    auto& source = *state.snapshot;
    std::array<uint32_t, Abi::HeaderWords> header{};
    header[Abi::MagicWord] = Abi::Magic;
    header[Abi::VersionWord] = Abi::Version;
    header[Abi::CapacityWord] = Abi::Capacity;
    Write64(header, Abi::FrameLow, source.frame);
    Write64(header, Abi::PoseLow, source.poseId);
    Write64(header, Abi::GenerationLow, source.worldGeneration);
    Write64(header, Abi::EpochLow, source.drawSubmissionEpoch);
    Write64(header, Abi::SequenceLow, state.handle);
    header[Abi::RenderWidth] = source.renderWidth;
    header[Abi::RenderHeight] = source.renderHeight;
    header[Abi::SurfaceCount] = uint32_t(source.surfaces.size());
    header[Abi::UninitializedDraws] = source.uninitializedDraws;
    command.resetQueryPool(state.queries, 0, 2);
    command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, state.queries, 0);
    command.fillBuffer(state.output.buffer, 0, Abi::Bytes, 0);
    QualityBarrier(command, vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
    command.updateBuffer(state.output.buffer, 0, sizeof(header), header.data());
    command.updateBuffer(state.output.buffer, Abi::OwnerWord * 4, sizeof(source.owners),
        source.owners.data());
    for (uint32_t index = 0; index < source.surfaces.size(); ++index)
    {
        const auto& surface = source.surfaces[index];
        command.updateBuffer(state.output.buffer, (Abi::SurfaceWord + index * 16) * 4,
            sizeof(surface.data), &surface.data);
        command.updateBuffer(state.selectors[index].buffer, 0, sizeof(index), &index);
        const vk::DescriptorBufferInfo output(state.output.buffer, 0, Abi::Bytes);
        const vk::DescriptorBufferInfo selector(state.selectors[index].buffer, 0, SelectorBytes);
        std::vector<vk::WriteDescriptorSet> writes(6);
        writes[0].setDstSet(state.descriptors[index]).setDstBinding(0).setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer).setBufferInfo(output);
        writes[1].setDstSet(state.descriptors[index]).setDstBinding(1).setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
            .setImageInfo(surface.pageTable->GetDescriptorInfo());
        writes[2].setDstSet(state.descriptors[index]).setDstBinding(2).setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer).setBufferInfo(selector);
        for (uint32_t binding = 3; binding <= 5; ++binding)
            writes[binding].setDstSet(state.descriptors[index]).setDstBinding(binding).setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler).setImageInfo(
                    binding == 5 ? source.finalFeedbackDepth.descriptor :
                        state.winnerAttachments[binding - 3].descriptor);
        state.backend.UpdateDescriptorSets(writes);
    }
    QualityBarrier(command, vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
        vk::AccessFlagBits::eTransferWrite,
        vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    rollback.complete = true;
    return state.handle;
}

void TerrainCompositedQualityGpu::RecordResolveAndCopy(vk::CommandBuffer command, Handle handle)
{
    auto& state = *impl;
    state.Check(handle);
    RequireQuality(!state.copied && !state.submittedEpoch,
        "Quality resolve requires one graph winner import and one copy");
    state.pipeline->Bind(command);
    for (uint32_t index = 0; index < state.snapshot->surfaces.size(); ++index)
    {
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
            state.pipeline->GetPipelineLayout(), 0, state.descriptors[index], nullptr);
        command.dispatch(Abi::Capacity / 64, 1, 1);
        QualityBarrier(command, vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderWrite,
            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    }
    QualityBarrier(command, vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eShaderWrite,
        vk::AccessFlagBits::eTransferRead);
    command.copyBuffer(state.output.buffer, state.host.buffer, vk::BufferCopy(0, 0, Abi::Bytes));
    QualityBarrier(command, vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eHost, vk::AccessFlagBits::eTransferWrite,
        vk::AccessFlagBits::eHostRead);
    command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, state.queries, 1);
    state.copied = true;
}

std::shared_ptr<const void> TerrainCompositedQualityGpu::Submitted(Handle handle, uint64_t epoch)
{
    impl->Check(handle);
    RequireQuality(impl->copied && !impl->submittedEpoch &&
        epoch == impl->snapshot->drawSubmissionEpoch, "Quality submission differs from sampled draw epoch");
    impl->submittedEpoch = epoch;
    return impl;
}

void TerrainCompositedQualityGpu::Abort(Handle handle)
{
    impl->Check(handle);
    RequireQuality(!impl->submittedEpoch, "Submitted quality capture must retire at fence");
    impl->snapshot.reset();
    impl->captureGate.reset();
    impl->copied = false;
}

std::shared_ptr<const TerrainCompositedQualityReadback> TerrainCompositedQualityGpu::TryConsume(
    uint64_t completedEpoch, double completedMonotonicMs)
{
    auto& state = *impl;
    if (!state.submittedEpoch || completedEpoch < state.submittedEpoch)
        return {};
    RequireQuality(std::isfinite(completedMonotonicMs) && completedMonotonicMs >= state.captureMs,
        "Quality completion observation time must not precede capture");
    std::array<uint64_t, 2> timestamps{};
    if (!state.backend.TryReadTimestampQueryPair(state.queries, 0, timestamps))
        return {};
    auto result = std::make_shared<TerrainCompositedQualityReadback>();
    std::memcpy(state.cpuReadback.data(), state.mapped, Abi::Bytes);
    result->words = state.cpuReadback.data();
    result->bytes = Abi::Bytes;
    result->submissionEpoch = state.submittedEpoch;
    result->completedEpoch = completedEpoch;
    result->capturedMonotonicMs = state.captureMs;
    result->completedMonotonicMs = completedMonotonicMs;
    const auto validBits = state.backend.GetGraphicsTimestampValidBits();
    const uint64_t mask = validBits >= 64 ? UINT64_MAX : (uint64_t(1) << validBits) - 1;
    result->gpuMilliseconds = double((timestamps[1] - timestamps[0]) & mask) *
        state.backend.GetTimestampPeriodNanoseconds() / 1000000.0;
    result->gpuAllocationBytes = state.gpuBytes;
    result->hostAllocationBytes = state.hostBytes;
    result->budget = state.budget->Statistics();
    state.ObserveWorkspace();
    result->workspaceObservedPeakBytes = state.workspaceObservedPeakBytes;
    result->counts = Abi::Recompute(result->words, result->bytes);
    RequireQuality(
        Abi::Join(result->words[Abi::FrameLow], result->words[Abi::FrameHigh]) == state.snapshot->frame &&
        Abi::Join(result->words[Abi::PoseLow], result->words[Abi::PoseHigh]) == state.snapshot->poseId &&
        Abi::Join(result->words[Abi::GenerationLow], result->words[Abi::GenerationHigh]) ==
            state.snapshot->worldGeneration &&
        Abi::Join(result->words[Abi::EpochLow], result->words[Abi::EpochHigh]) == state.submittedEpoch,
        "Quality binary frame/pose/generation/epoch differs from recorded draw");
    result->snapshot = std::move(state.snapshot);
    result->storage = impl;
    result->captureGate = std::move(state.captureGate);
    state.outstanding = result;
    state.submittedEpoch = 0;
    ++state.completed;
    return result;
}

void TerrainCompositedQualityGpu::WriteReadback(
    const TerrainCompositedQualityReadback& readback, std::ostream& binary, std::ostream& metadata)
{
    auto& state = *impl;
    RequireQuality(!state.exportFailed && readback.storage.get() == impl.get() &&
        readback.words == state.cpuReadback.data() &&
        readback.bytes == Abi::Bytes && readback.snapshot &&
        readback.completedEpoch >= readback.submissionEpoch, "Quality export requires owned completed readback");
    const auto sequence = Abi::Join(
        readback.words[Abi::SequenceLow], readback.words[Abi::SequenceHigh]);
    RequireQuality(sequence > state.lastExportedHandle, "Quality readback already exported");
    RequireQuality(state.exportedCaptures < state.limits.maxCaptures &&
        state.exportedCaptures < state.limits.maxReadbackBytes / Abi::Bytes,
        "Quality diagnostic export exceeds its bounded run byte cap");
    {
        std::lock_guard<std::mutex> lock(qualityCaptureMutex);
        RequireQuality(!qualityExportFailed && qualityExportedCaptures < state.limits.maxCaptures &&
            qualityExportedCaptures < state.limits.maxReadbackBytes / Abi::Bytes,
            "Quality diagnostic export exceeds the process run byte cap");
        ++qualityExportedCaptures;
    }
    try
    {
        nlohmann::json document{
            {"protocol", 1}, {"binaryAbi", "TVQ1-final-results-v2"},
            {"moduleOnly", std::getenv("VULKANLEARN_RVT_QUALITY_MODULE_ONLY") &&
                std::string(std::getenv("VULKANLEARN_RVT_QUALITY_MODULE_ONLY")) == "1"},
            {"duplicateDiagnostics", {
                {"wordOffset", Abi::DuplicateWord},
                {"strideWords", Abi::DuplicateStrideWords},
                {"capacity", Abi::DuplicateCapacity},
                {"attempted", readback.words[Abi::DuplicateWitnessCount]},
                {"overflow", readback.words[Abi::DuplicateWitnessOverflow]},
                {"firstPayload", "sample[pixel], read only after completed submission"},
                {"firstPrimitiveIdentityAvailable", false},
                {"secondPrimitiveIdentity", "optional gl_PrimitiveID; UINT32_MAX means unavailable"},
                {"instanceIdentityAvailable", false},
                {"depthWinnerIdentityAvailable", false}}},
            {"source", "gpu-page-table-readback"}, {"frame", readback.snapshot->frame},
            {"visibilityTest", "original-feedback-depth-tested-MRT-winner"},
            {"winnerPayloadAbi", "RGBA32Uint[2]:uv2-major2-target-bias-surfaceAndTaps-depthBitsWithValidSign"},
            {"visibilityMask", "nonzero sample.claims in the fixed 128x128 pixel domain"},
            {"poseId", readback.snapshot->poseId}, {"worldGeneration", readback.snapshot->worldGeneration},
            {"submissionEpoch", readback.submissionEpoch}, {"completedEpoch", readback.completedEpoch},
            {"cameraPosition", readback.snapshot->cameraPosition},
            {"cameraLookAt", readback.snapshot->cameraLookAt}, {"captureIntervalMs", 250},
            {"feedbackDepth", {
                {"extent", {readback.snapshot->finalFeedbackDepth.extent.width,
                    readback.snapshot->finalFeedbackDepth.extent.height}},
                {"format", uint32_t(readback.snapshot->finalFeedbackDepth.format)},
                {"layout", uint32_t(readback.snapshot->finalFeedbackDepth.descriptor.imageLayout)},
                {"usage", uint32_t(readback.snapshot->finalFeedbackDepth.usage)}}},
            {"capturedMonotonicMs", readback.capturedMonotonicMs},
            {"completedMonotonicMs", readback.completedMonotonicMs},
            {"captureGpuAllocationBytes", readback.gpuAllocationBytes},
            {"captureHostAllocationBytes", readback.hostAllocationBytes},
            {"allocationQuery", {
                {"payloadBytes", Abi::Bytes},
                {"duplicateAppendixBytes", Abi::DuplicateCapacity * Abi::DuplicateStrideWords * 4},
                {"outputRequirementBytes", state.allocationQuote.outputBytes},
                {"hostRequirementBytes", state.allocationQuote.hostBytes},
                {"selectorRequirementBytes", state.allocationQuote.selectorBytes},
                {"selectorCount", Abi::Surfaces},
                {"importedWinnerGpuBytes", state.limits.importedWinnerGpuBytes},
                {"importedWinnerImageCount", state.limits.importedWinnerOwners.size()},
                {"deviceMaxColorAttachments", state.backend.GetSelectedDeviceLimits().maxColorAttachments},
                {"admittedGpuBytes", state.allocationQuote.total.gpuBytes},
                {"admittedCpuBytes", state.allocationQuote.total.cpuBytes}}},
            {"workspacePeakBytes", readback.workspaceObservedPeakBytes},
            {"workspacePeakStatistic", "maximum owner Workspace bytes observed at allocation/Record/TryConsume"},
            {"cpuPeakBytes", readback.budget.peak.cpuBytes},
            {"captureGpuMilliseconds", readback.gpuMilliseconds},
            {"requestedCaptures", state.requested}, {"completedCaptures", state.completed},
            {"deferredCaptures", state.deferred}, {"surfaceIds", nlohmann::json::array()}
        };
        for (const auto& surface : readback.snapshot->surfaces)
            document["surfaceIds"].push_back(surface.surfaceId);
        const auto encoded = document.dump();
        RequireQuality(encoded.size() <= 65536, "Quality metadata exceeds admitted bound");
        binary.write(reinterpret_cast<const char*>(readback.words), readback.bytes);
        metadata.write(encoded.data(), encoded.size());
        metadata.put('\n');
        binary.flush();
        metadata.flush();
        RequireQuality(binary.good() && metadata.good(), "Quality binary/metadata export failed");
        state.lastExportedHandle = sequence;
        ++state.exportedCaptures;
    }
    catch (...)
    {
        state.exportFailed = true;
        std::lock_guard<std::mutex> lock(qualityCaptureMutex);
        qualityExportFailed = true;
        throw;
    }
}

bool TerrainCompositedQualityGpu::Pending() const { return bool(impl->snapshot); }
uint64_t TerrainCompositedQualityGpu::Requested() const { return impl->requested; }
uint64_t TerrainCompositedQualityGpu::Deferred() const { return impl->deferred; }
uint64_t TerrainCompositedQualityGpu::Completed() const { return impl->completed; }
bool TerrainCompositedQualityGpu::ExportAvailable() const
{
    std::lock_guard<std::mutex> lock(qualityCaptureMutex);
    return !qualityExportFailed && !impl->exportFailed &&
        qualityExportedCaptures < impl->limits.maxCaptures &&
        qualityExportedCaptures < impl->limits.maxReadbackBytes / Abi::Bytes;
}
}
