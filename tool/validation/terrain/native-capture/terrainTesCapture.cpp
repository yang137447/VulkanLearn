#include "terrainTesCapture.h"
#include "terrainTesDiagnostic.h"
#include "pipeline/pipelineBase.h"
#include "render/backend/rendererBackendVulkan.h"
#include "shader/build/contentHash.h"
#include "shaderReflect.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <nlohmann/json.hpp>
#include <ostream>
#include <stdexcept>

namespace VL
{
namespace
{
std::mutex captureMutex;
std::weak_ptr<const void> captureGate;

void Require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

void Put64(std::array<uint32_t, 16>& header, uint32_t word, uint64_t value)
{
    header[word] = uint32_t(value);
    header[word + 1] = uint32_t(value >> 32);
}

void Barrier(vk::CommandBuffer command, vk::PipelineStageFlags sourceStage,
    vk::PipelineStageFlags destinationStage, vk::AccessFlags sourceAccess,
    vk::AccessFlags destinationAccess)
{
    vk::MemoryBarrier barrier(sourceAccess, destinationAccess);
    command.pipelineBarrier(sourceStage, destinationStage, {}, barrier, {}, {});
}

}

struct TerrainTesCapture::Impl
{
    RendererBackendVulkan& backend;
    std::shared_ptr<TerrainRvtBudgetOwner> budget;
    std::shared_ptr<PipelineBase> pipeline;
    TerrainTesCaptureLimits limits;
    TerrainTesCaptureQuote quote;
    std::optional<TerrainRvtBudgetLease> lease;
    std::shared_ptr<const void> gate;
    vk::Buffer deviceBuffer, hostBuffer;
    vk::DeviceMemory deviceMemory, hostMemory;
    vk::DescriptorPool descriptorPool;
    vk::DescriptorSet descriptorSet;
    void* mapped = nullptr;
    std::shared_ptr<const TerrainTesDrawWitness> draw;
    std::weak_ptr<const TerrainTesCaptureReadback> outstanding;
    std::array<uint32_t, 16> expectedHeader{};
    uint64_t submittedEpoch = 0;
    bool bound = false;
    bool copied = false;
    uint32_t attempts = 0;
    uint32_t requiredDescriptorSetMask = 0;

    Impl(RendererBackendVulkan& backend, std::shared_ptr<TerrainRvtBudgetOwner> budget,
        std::shared_ptr<PipelineBase> pipeline, TerrainTesCaptureLimits limits)
        : backend(backend), budget(std::move(budget)), pipeline(std::move(pipeline)), limits(limits) {}
    ~Impl()
    {
        if (mapped) backend.UnmapMemory(hostMemory);
        if (descriptorSet) backend.FreeDescriptorSet(descriptorPool, descriptorSet);
        if (descriptorPool) backend.DestroyDescriptorPool(descriptorPool);
        backend.DestroyBuffer(deviceBuffer, deviceMemory);
        backend.DestroyBuffer(hostBuffer, hostMemory);
    }
};

TerrainTesCaptureQuote TerrainTesCapture::QueryAllocation(RendererBackendVulkan& backend,
    const TerrainTesCaptureLimits& limits)
{
    Require(limits.capacity >= 1 && limits.capacity <= 4096, "TES capture capacity outside 1..4096");
    const uint64_t bytes = HeaderBytes + uint64_t(limits.capacity) * RecordBytes;
    Require(bytes <= backend.GetSelectedDeviceLimits().maxStorageBufferRange,
        "TES capture exceeds device storage-buffer range");
    const auto deviceRequirement = backend.QueryBufferMemoryRequirements(bytes,
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
        vk::BufferUsageFlagBits::eTransferDst).size;
    const auto hostRequirement = backend.QueryBufferMemoryRequirements(bytes,
        vk::BufferUsageFlagBits::eTransferDst).size;
    return AllocationQuote(deviceRequirement, hostRequirement, limits);
}

TerrainTesCapture::TerrainTesCapture(RendererBackendVulkan& backend,
    std::shared_ptr<TerrainRvtBudgetOwner> actualWorldBudget,
    std::shared_ptr<PipelineBase> diagnosticPipeline, TerrainTesCaptureLimits limits)
{
    Require(TerrainTesDiagnostic::Requested() && TerrainTesDiagnostic::DeviceFeatureEnabled(),
        "TES diagnostic capture not enabled on the actual logical device");
    Require(bool(actualWorldBudget) && bool(diagnosticPipeline) && diagnosticPipeline->UsesTessellation(),
        "TES capture requires actual World budget and diagnostic patch pipeline");
    Require(actualWorldBudget->Caps().workspaceGpuBytes <= 32ull * 1024 * 1024,
        "TES capture cannot increase frozen Workspace cap");
    Require(backend.GetSelectedDeviceLimits().maxBoundDescriptorSets >= 5 &&
        diagnosticPipeline->GetDescriptorSetLayouts().size() == 5,
        "TES capture requires admitted diagnostic set4");
    const auto& bindings = diagnosticPipeline->GetShaderBindings();
    const auto binding = std::find_if(bindings.begin(), bindings.end(),
        [](const auto& item) { return item.set == 4 && item.binding == 0; });
    Require(binding != bindings.end() && binding->type == vk::DescriptorType::eStorageBuffer &&
        binding->descriptorCount == 1 && binding->stageFlags == vk::ShaderStageFlagBits::eTessellationEvaluation &&
        binding->name == "terrainTestTesCapture", "TES diagnostic reflected set4 binding differs from capture ABI");
    auto state = std::make_shared<Impl>(backend, std::move(actualWorldBudget), std::move(diagnosticPipeline), limits);
    for (const auto& item : bindings)
        if (item.set < 4)
            state->requiredDescriptorSetMask |= 1u << item.set;
    {
        std::lock_guard<std::mutex> lock(captureMutex);
        Require(captureGate.expired(), "Only one bounded TES helper may own capture storage globally");
        state->gate = std::make_shared<const uint32_t>(1);
        captureGate = state->gate;
    }
    state->quote = QueryAllocation(backend, limits);
    state->lease = state->budget->TryReserve(state->quote.total, TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
    Require(bool(state->lease), "TES capture rejected by actual World/outer Workspace admission");
    auto device = backend.CreateBuffer(state->quote.payloadBytes,
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal, "TestTerrainTesOutput");
    state->deviceBuffer = device.first;
    state->deviceMemory = device.second;
    auto host = backend.CreateBuffer(state->quote.payloadBytes, vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent, "TestTerrainTesReadback");
    state->hostBuffer = host.first;
    state->hostMemory = host.second;
    state->mapped = backend.MapMemory(state->hostMemory, state->quote.payloadBytes);
    const vk::DescriptorPoolSize size(vk::DescriptorType::eStorageBuffer, 1);
    vk::DescriptorPoolCreateInfo pool;
    pool.setMaxSets(1).setPoolSizes(size).setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
    state->descriptorPool = backend.CreateDescriptorPool(pool, "TestTerrainTesDescriptors");
    vk::DescriptorSetAllocateInfo allocate;
    allocate.setDescriptorPool(state->descriptorPool).setSetLayouts(state->pipeline->GetDescriptorSetLayouts()[4]);
    std::vector<vk::DescriptorSet> descriptors(1);
    backend.AllocateDescriptorSets(allocate, descriptors);
    state->descriptorSet = descriptors[0];
    vk::DescriptorBufferInfo info(state->deviceBuffer, 0, state->quote.payloadBytes);
    vk::WriteDescriptorSet write;
    write.setDstSet(state->descriptorSet).setDstBinding(0).setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eStorageBuffer).setBufferInfo(info);
    backend.UpdateDescriptorSets({write});
    impl = std::move(state);
}

TerrainTesCapture::~TerrainTesCapture() = default;

void TerrainTesCapture::BeginOutsideRenderPass(vk::CommandBuffer command,
    std::shared_ptr<const TerrainTesDrawWitness> actualDraw)
{
    auto& state = *impl;
    Require(bool(command) && bool(actualDraw) && !state.draw && state.outstanding.expired() &&
        !state.submittedEpoch && state.attempts < 256, "TES capture busy/invalid or run cap exhausted");
    ValidateActualDraw(*actualDraw);
    Require(actualDraw->actualDescriptorSetMask == state.requiredDescriptorSetMask,
        "TES actual bound descriptor mask differs from reflected pipeline");
    state.draw = std::move(actualDraw);
    state.bound = state.copied = false;
    state.expectedHeader = {};
    state.expectedHeader[2] = state.draw->firstPrimitive;
    state.expectedHeader[3] = state.draw->primitiveCount;
    Put64(state.expectedHeader, 4, state.draw->frame);
    Put64(state.expectedHeader, 6, state.draw->submissionEpoch);
    Put64(state.expectedHeader, 8, state.draw->worldGeneration);
    Put64(state.expectedHeader, 10, state.draw->poseId);
    state.expectedHeader[12] = state.draw->drawOrdinal;
    state.expectedHeader[13] = state.draw->surfaceIndex;
    state.expectedHeader[14] = state.draw->actualLod;
    state.expectedHeader[15] = uint32_t(state.draw->topology);
    command.fillBuffer(state.deviceBuffer, 0, state.quote.payloadBytes, 0);
    Barrier(command, vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
    command.updateBuffer(state.deviceBuffer, 0, HeaderBytes, state.expectedHeader.data());
    Barrier(command, vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTessellationEvaluationShader,
        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    ++state.attempts;
}

void TerrainTesCapture::BindInsideActualDraw(vk::CommandBuffer command, const PipelineBase& actuallyBoundPipeline)
{
    auto& state = *impl;
    Require(state.draw && !state.bound && !state.copied && &actuallyBoundPipeline == state.pipeline.get(),
        "TES capture must bind once at its actual diagnostic patch draw");
    command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, state.pipeline->GetPipelineLayout(),
        4, state.descriptorSet, {});
    state.bound = true;
}

void TerrainTesCapture::CopyOutsideRenderPass(vk::CommandBuffer command)
{
    auto& state = *impl;
    Require(state.draw && state.bound && !state.copied && !state.submittedEpoch, "TES copy has no unique actual draw");
    Barrier(command, vk::PipelineStageFlagBits::eTessellationEvaluationShader, vk::PipelineStageFlagBits::eTransfer,
        vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead);
    command.copyBuffer(state.deviceBuffer, state.hostBuffer, vk::BufferCopy(0, 0, state.quote.payloadBytes));
    Barrier(command, vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead);
    state.copied = true;
}

std::shared_ptr<const void> TerrainTesCapture::Submitted(uint64_t actualSubmissionEpoch)
{
    auto& state = *impl;
    Require(state.draw && state.copied && !state.submittedEpoch &&
        actualSubmissionEpoch == state.draw->submissionEpoch, "TES submission differs from captured native draw");
    state.submittedEpoch = actualSubmissionEpoch;
    return impl;
}

void TerrainTesCapture::AbortUnsubmitted()
{
    Require(!impl->submittedEpoch, "Submitted TES storage must retire at its actual fence");
    impl->draw.reset();
    impl->bound = impl->copied = false;
}

std::shared_ptr<const TerrainTesCaptureReadback> TerrainTesCapture::TryConsume(uint64_t actualCompletedEpoch)
{
    auto& state = *impl;
    if (!state.submittedEpoch || actualCompletedEpoch < state.submittedEpoch || !state.outstanding.expired())
        return {};
    const auto* header = static_cast<const uint32_t*>(state.mapped);
    std::array<uint32_t, 16> observed;
    std::memcpy(observed.data(), header, HeaderBytes);
    ValidateHeader(observed, state.expectedHeader, state.limits.capacity);
    auto result = std::make_shared<TerrainTesCaptureReadback>();
    result->data = state.mapped;
    result->bytes = state.quote.payloadBytes;
    result->completedEpoch = actualCompletedEpoch;
    result->recordCount = header[0];
    result->witness = std::move(state.draw);
    result->storageOwner = impl;
    result->quote = state.quote;
    state.submittedEpoch = 0;
    state.outstanding = result;
    return result;
}

void TerrainTesCapture::WriteReadback(const TerrainTesCaptureReadback& readback,
    std::ostream& binary, std::ostream& metadata)
{
    Require(readback.data && readback.storageOwner && readback.witness, "TES readback owner missing");
    const auto& draw = *readback.witness;
    const auto* header = static_cast<const uint32_t*>(readback.data);
    const auto bytes = static_cast<std::streamsize>(readback.bytes);
    binary.write(static_cast<const char*>(readback.data), bytes);
    Require(bool(binary), "TES binary writer failed");
    nlohmann::json document{
        {"protocol", 1}, {"stage", "tessellation-evaluation"}, {"gpuExecuted", true},
        {"primitiveTopology", "patch-list"}, {"actualLod", draw.actualLod},
        {"surfaceId", draw.surfaceId}, {"publicationSha256", draw.publicationSha256},
        {"instrumentedShaderSha256", draw.capturedShaderSha256}, {"inputClosureSha256", draw.inputClosureSha256},
        {"submissionEpoch", draw.submissionEpoch}, {"completedEpoch", readback.completedEpoch},
        {"frame", draw.frame}, {"worldGeneration", draw.worldGeneration}, {"poseId", draw.poseId},
        {"baseFineResidentPages", draw.baseFineResidentPages},
        {"frameEpochWords", {header[4], header[5], header[6], header[7]}},
        {"worldPoseWords", {header[8], header[9], header[10], header[11]}},
        {"drawWords", {header[12], header[13], header[14], header[15]}},
        {"records", readback.recordCount}, {"capacity", (readback.bytes-HeaderBytes)/RecordBytes},
        {"actualIndexCount", draw.actualIndexCount}, {"actualFirstIndex", draw.actualFirstIndex},
        {"actualVertexOffset", draw.actualVertexOffset}, {"actualInstanceCount", draw.actualInstanceCount},
        {"actualFirstInstance", draw.actualFirstInstance}, {"model", draw.model},
        {"viewProjection", draw.viewProjection}, {"displacementScale", draw.displacementScale},
        {"tessRadiusCm", draw.tessRadiusCm},
        {"nativePayloadBlake3", ContentHasher::HashBytes(readback.data, readback.bytes).ToHex()},
        {"payloadBytes", readback.quote.payloadBytes}, {"deviceRequirementBytes", readback.quote.deviceRequirementBytes},
        {"hostRequirementBytes", readback.quote.hostRequirementBytes},
        {"admittedGpuBytes", readback.quote.total.gpuBytes}, {"admittedCpuBytes", readback.quote.total.cpuBytes},
        {"scope", "Selected 1..4 actual normal LOD0 patches; no full-scene lit/geometry claim"}};
    metadata << document.dump(2) << '\n';
    Require(bool(metadata), "TES metadata writer failed");
}
}
