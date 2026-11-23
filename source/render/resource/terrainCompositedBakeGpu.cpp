#include "render/resource/terrainCompositedBakeGpu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/terrainCompositedBakeAdjacency.h"
#include "terrain/terrainRvtBudget.h"
#include "render/resource/terrainCompositedComputeReloadParticipant.h"
#include "shaderCompiler.h"

namespace VL
{
namespace
{

using Baker = TerrainCompositedBakeGpu;

struct ProducerDescriptorPool
{
    RendererBackendVulkan* backend = nullptr;
    vk::DescriptorPool pool{};
    ~ProducerDescriptorPool()
    {
        if (pool)
            backend->DestroyDescriptorPool(pool);
    }
};

TerrainCompositedProducerEntry CaptureClosure(
    const ComputeShaderArtifact& artifact,
    const std::shared_ptr<ComputePipeline>& pipeline,
    const ShaderBuildArtifact& closure, const ShaderBuildRequest& request)
{
    const auto actual = BuildComputeShaderArtifact(closure, artifact.shaderName);
    if (actual.artifactGenerationKey != artifact.artifactGenerationKey ||
        actual.sourceFingerprint != artifact.sourceFingerprint ||
        actual.runtimeSpirv != artifact.runtimeSpirv || actual.debugSpirv != artifact.debugSpirv ||
        actual.abiSignature != artifact.abiSignature)
        throw std::runtime_error("Terrain producer closure changed during reload prepare");
    TerrainCompositedProducerEntry entry{artifact, closure,
        ShaderCompiler::GetCompilePolicy(), ShaderBuildTargetEnvironment, pipeline, {}};
    for (const auto& primary : entry.closure.primarySources)
    {
        const auto found = request.sourceSnapshot.find(primary.identity);
        if (found != request.sourceSnapshot.end())
            entry.sourceBytes.emplace(primary.identity, found->second);
        else
        {
            const auto stage = std::find_if(request.stages.begin(), request.stages.end(),
                [&](const auto& value) { return value.sourceIdentity == primary.identity; });
            if (stage == request.stages.end())
                throw std::runtime_error("Missing terrain producer frozen primary source");
            entry.sourceBytes.emplace(primary.identity, stage->sourceCode);
        }
    }
    for (const auto& dependency : entry.closure.dependencies)
        entry.sourceBytes.emplace(dependency.path, request.sourceSnapshot.at(dependency.path));
    return entry;
}

TerrainCompositedProducerEntry CaptureInitialClosure(
    ShaderCompiler& compiler, const ComputeShaderArtifact& artifact,
    const std::shared_ptr<ComputePipeline>& pipeline)
{
    auto request = compiler.CreateComputeStageBuildRequestForShaderName(artifact.shaderName);
    compiler.FreezeSourceSnapshot(request);
    const auto closure = compiler.PrepareCandidate(request);
    if (!compiler.ValidateCandidateSourcesStillCurrent(request, closure).current)
        throw std::runtime_error("Terrain producer initial source closure changed");
    return CaptureClosure(artifact, pipeline, closure, request);
}

uint32_t MipExtent(uint32_t extent, uint32_t mip)
{
    return std::max(1u, extent >> mip);
}

bool HasTexture(const Baker::SampledTexture& texture)
{
    return texture.descriptor.sampler && texture.descriptor.imageView &&
        (texture.descriptor.imageLayout == vk::ImageLayout::eShaderReadOnlyOptimal ||
         texture.descriptor.imageLayout == vk::ImageLayout::eGeneral) &&
        texture.width > 0 && texture.height > 0 &&
        texture.layers > 0 && texture.mipLevels > 0;
}

template<size_t Count>
bool Finite(const std::array<float, Count>& values)
{
    return std::all_of(values.begin(), values.end(),
        [](float value) { return std::isfinite(value); });
}

}

struct TerrainCompositedBakeGpu::GpuParams
{
    std::array<uint32_t, 4> page{};
    std::array<uint32_t, 4> coverage{};
    std::array<uint32_t, 4> counts{};
    std::array<float, 4> surfaceUv{};
    std::array<float, 4> coverageUv{};
    std::array<float, 4> frameUv{};
    std::array<float, 4> material{};
    std::array<uint32_t, 4> adjacency{};
    std::array<float, 16> normalToSource{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<uint32_t, 256> globalToSlice{};
    std::array<uint32_t, 256> localToGlobal{};
};

struct TerrainCompositedBakeGpu::GpuWorldParams
{
    std::array<uint32_t, 4> page{};
    std::array<uint32_t, 4> control{};
    std::array<GpuParams, MaxSurfaces> surfaces{};
};

TerrainCompositedBakeGpu::TerrainCompositedBakeGpu(
    RendererBackendVulkan& backend, PipelineFactory& pipelineFactory,
    uint32_t slotCount, uint32_t adjacencySurfaceCapacity)
    : TerrainCompositedBakeGpu(backend, pipelineFactory, slotCount, adjacencySurfaceCapacity, nullptr)
{
}

TerrainCompositedBakeGpu::TerrainCompositedBakeGpu(
    RendererBackendVulkan& backend,
    PipelineFactory& pipelineFactory,
    uint32_t slotCount, uint32_t adjacencySurfaceCapacity,
    const vk::PhysicalDeviceLimits* selectedDeviceLimits)
    : backend(&backend), surfaceCapacity(adjacencySurfaceCapacity)
{
    static_assert(sizeof(GpuParams) == 2240, "Composited bake std430 ABI");
    static_assert(sizeof(GpuWorldParams) == WorldParameterBytes, "Composited world bake std430 ABI");
    try
    {
        if (slotCount == 0 || slotCount > 256)
            throw std::runtime_error("Composited bake slotCount must be 1..256");
        if (surfaceCapacity == 0 || surfaceCapacity > MaxSurfaces)
            throw std::runtime_error("Composited world bake surface capacity must be 1..16");
        if (!selectedDeviceLimits)
            throw std::runtime_error("World terrain baker requires the selected device's physical limits before pipeline creation");
        std::string limitError;
        if (!ValidateDeviceLimits(*selectedDeviceLimits, limitError))
            throw std::runtime_error(limitError);
        ComputeShaderArtifact worldArtifact;
        ComputeShaderArtifact paddingArtifact;
        pipeline = pipelineFactory.CreateComputePipelineCandidate(
            "terrain/terrainCompositedBakeWorld", &worldArtifact);
        if (!pipeline || pipeline->GetDescriptorSetLayouts().empty())
            throw std::runtime_error("Composited bake pipeline has no descriptor layout");
        paddingPipeline = pipelineFactory.CreateComputePipelineCandidate(
            "terrain/terrainCompositedBakeWorldPadding", &paddingArtifact);
        if (!paddingPipeline || paddingPipeline->GetDescriptorSetLayouts().empty())
            throw std::runtime_error("Composited world padding pipeline unavailable");
        slots.resize(slotCount + 1);
        const uint32_t taskCount = slotCount + 1;
        const uint32_t setCount = taskCount * 2;
        const std::array<vk::DescriptorPoolSize, 2> poolSizes{
            vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, taskCount * 5),
            vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, taskCount * MaxSurfaces * 11)};
        vk::DescriptorPoolCreateInfo poolInfo;
        poolInfo.setPoolSizes(poolSizes).setMaxSets(setCount);
        descriptorPool = backend.CreateDescriptorPool(
            poolInfo, "TerrainCompositedBakeDescriptors");
        std::vector<vk::DescriptorSetLayout> layouts(
            taskCount, pipeline->GetDescriptorSetLayouts()[0]);
        layouts.insert(layouts.end(), taskCount, paddingPipeline->GetDescriptorSetLayouts()[0]);
        vk::DescriptorSetAllocateInfo allocateInfo;
        allocateInfo.setDescriptorPool(descriptorPool).setSetLayouts(layouts);
        std::vector<vk::DescriptorSet> sets(setCount);
        backend.AllocateDescriptorSets(allocateInfo, sets);
        for (uint32_t index = 0; index <= slotCount; ++index)
        {
            Slot& slot = slots[index];
            slot.descriptors = sets[index];
            slot.paddingDescriptors = sets[taskCount + index];
            const auto buffer = backend.CreateBuffer(
                sizeof(GpuWorldParams), vk::BufferUsageFlagBits::eStorageBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                "TerrainCompositedBakeParams" + std::to_string(index));
            slot.params = buffer.first;
            slot.memory = buffer.second;
            slot.mapped = backend.MapMemory(slot.memory, sizeof(GpuWorldParams));
            const auto charts = backend.CreateBuffer(
                64 * surfaceCapacity, vk::BufferUsageFlagBits::eStorageBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                "TerrainCompositedBakeCharts" + std::to_string(index));
            slot.charts = charts.first;
            slot.chartsMemory = charts.second;
            slot.chartsMapped = backend.MapMemory(slot.chartsMemory, 64 * surfaceCapacity);
        }
        auto& compiler = pipelineFactory.GetShaderCompiler();
        TerrainCompositedProducerSnapshot::Entries entries{
            CaptureInitialClosure(compiler, worldArtifact, pipeline),
            CaptureInitialClosure(compiler, paddingArtifact, paddingPipeline)};
        TerrainCompositedProducerSnapshot::Descriptors descriptors;
        auto poolOwner = std::make_shared<ProducerDescriptorPool>();
        poolOwner->backend = &backend;
        poolOwner->pool = descriptorPool;
        descriptorPool = nullptr;
        descriptors.owner = poolOwner;
        descriptors.world.assign(sets.begin(), sets.begin() + taskCount);
        descriptors.padding.assign(sets.begin() + taskCount, sets.end());
        producerState = std::make_unique<TerrainCompositedProducerReloadState>(
            TerrainCompositedProducerSnapshot::Create(std::move(entries), std::move(descriptors)));
        reloadParticipants = std::make_unique<TerrainCompositedComputeReloadParticipants>(
            *producerState,
            [](const auto& artifact, const auto& selectedPipeline,
                const auto& closure, const auto& request) {
                return CaptureClosure(artifact, selectedPipeline, closure, request);
            },
            [this, taskCount](const auto& selectedEntries) {
                TerrainCompositedProducerSnapshot::Descriptors replacement;
                auto owner = std::make_shared<ProducerDescriptorPool>();
                owner->backend = this->backend;
                const std::array<vk::DescriptorPoolSize, 2> sizes{
                    vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, taskCount * 5),
                    vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler,
                        taskCount * MaxSurfaces * 11)};
                vk::DescriptorPoolCreateInfo info;
                info.setPoolSizes(sizes).setMaxSets(taskCount * 2);
                owner->pool = this->backend->CreateDescriptorPool(info, "TerrainProducerReload");
                std::vector<vk::DescriptorSetLayout> selectedLayouts(
                    taskCount, selectedEntries[0].pipeline->GetDescriptorSetLayouts().at(0));
                selectedLayouts.insert(selectedLayouts.end(), taskCount,
                    selectedEntries[1].pipeline->GetDescriptorSetLayouts().at(0));
                vk::DescriptorSetAllocateInfo allocate;
                allocate.setDescriptorPool(owner->pool).setSetLayouts(selectedLayouts);
                std::vector<vk::DescriptorSet> selectedSets(taskCount * 2);
                this->backend->AllocateDescriptorSets(allocate, selectedSets);
                replacement.owner = std::move(owner);
                replacement.world.assign(selectedSets.begin(), selectedSets.begin() + taskCount);
                replacement.padding.assign(selectedSets.begin() + taskCount, selectedSets.end());
                return replacement;
            });
        pipeline.reset();
        paddingPipeline.reset();
        available = true;
    }
    catch (const std::exception& exception)
    {
        unavailableReason = exception.what();
        Release();
    }
}

TerrainCompositedBakeGpu::~TerrainCompositedBakeGpu()
{
    Release();
}

void TerrainCompositedBakeGpu::Release()
{
    reloadParticipants.reset();
    if (!backend)
        return;
    if (descriptorPool)
        backend->DestroyDescriptorPool(descriptorPool);
    descriptorPool = nullptr;
    for (Slot& slot : slots)
    {
        if (slot.mapped)
            backend->UnmapMemory(slot.memory);
        if (slot.params)
            backend->DestroyBuffer(slot.params, slot.memory);
        if (slot.chartsMapped)
            backend->UnmapMemory(slot.chartsMemory);
        if (slot.charts)
            backend->DestroyBuffer(slot.charts, slot.chartsMemory);
    }
    slots.clear();
    producerState.reset();
    pipeline.reset();
    paddingPipeline.reset();
    available = false;
}

TerrainCompositedBakeGpu::ProducerSnapshot TerrainCompositedBakeGpu::CaptureProducer() const noexcept
{
    return producerState ? producerState->CaptureProducer() : ProducerSnapshot{};
}

TerrainCompositedBakeGpu::ProducerSnapshot TerrainCompositedBakeGpu::PendingProducer() const noexcept
{
    return producerState ? producerState->PendingProducer() : ProducerSnapshot{};
}

bool TerrainCompositedBakeGpu::PublishProducer(const ProducerSnapshot& selected) noexcept
{
    return producerState && producerState->PublishProducer(selected);
}

void TerrainCompositedBakeGpu::DiscardPendingProducer(const ProducerSnapshot& selected) noexcept
{
    if (producerState)
        producerState->DiscardPendingProducer(selected);
}

void TerrainCompositedBakeGpu::SetShaderReloadCoordinator(ShaderReloadCoordinator* coordinator)
{
    if (reloadParticipants)
        reloadParticipants->SetShaderReloadCoordinator(coordinator);
}

bool TerrainCompositedBakeGpu::Validate(
    const InputSnapshot& input,
    const PageRequest& page,
    std::string& error)
{
    error.clear();
    const auto fail = [&error](const char* message) {
        error = message;
        return false;
    };
    if (page.virtualWidth == 0 || page.virtualHeight == 0 ||
        page.virtualWidth > (1u << 24) || page.virtualHeight > (1u << 24) ||
        page.virtualMip > 31 || page.areaSamplesPerAxis < 1 ||
        page.areaSamplesPerAxis > 8 ||
        (page.areaSamplesPerAxis == 1 && page.virtualMip != 0))
        return fail("Invalid virtual extent/mip or filter (1 center sample only at mip0; area filter requires 2..8 samples per axis)");
    const uint32_t width = MipExtent(page.virtualWidth, page.virtualMip);
    const uint32_t height = MipExtent(page.virtualHeight, page.virtualMip);
    if (page.pageX > (width - 1) / InteriorExtent ||
        page.pageY > (height - 1) / InteriorExtent)
        return fail("Composited page lies outside the requested virtual mip");
    if (input.adjacency)
    {
        try
        {
            std::vector<std::string> ids;
            for (const auto& surface : input.adjacency->surfaces)
                ids.push_back(surface.surfaceId);
            input.adjacency->graph.Validate(ids);
            TerrainBakeAdjacency::ValidateAddressMode(input.adjacency->graph,
                input.surfaceId.empty() ? input.adjacency->sourceSurfaceId : input.surfaceId,
                page.repeatSurface);
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }
    if (!input.retainFragmentPostprocess)
        return fail("Base-only bake requires explicit retained fragment postprocess acknowledgement");
    if (!std::isfinite(input.retainedFragment.heightBlendStrength) ||
        input.retainedFragment.heightBlendStrength < 0.0f)
        return fail("Height blend strength must be finite and non-negative");
    if (input.retainedFragment.heightBlendStrength > 0.0f)
    {
        if (input.frozenHeightBlendFade != 1.0f)
            return fail("Static composited height blend requires frozenHeightBlendFade=1");
        if (!HasTexture(input.layerHeight) || input.layerHeight.mipLevels < 4 ||
            input.layerHeight.encoding != Encoding::Linear ||
            input.layerHeight.layers != input.layerAlbedo.layers)
            return fail("Height blend requires matching linear source height array with mip3");
    }
    if (input.hasUnsupportedViewTimeOrTransparentTerms)
        return fail("View/time dependent or transparent custom material terms cannot be baked");
    if (input.contentIdentity.empty())
        return fail("Composited bake requires immutable contentIdentity");
    if (input.sourceSetCount != 1)
        return fail("S1 requires one explicitly mapped source array set; merge source sets before baking");
    if (!Finite(input.surfaceUvOffset) || !Finite(input.surfaceUvScale) ||
        !Finite(input.coverageUvOffset) || !Finite(input.coverageUvScale) ||
        !Finite(input.frameUvOffset) || !Finite(input.frameUvScale) ||
        !Finite(input.localToWorld) || !std::isfinite(input.uvScale))
        return fail("Non-finite terrain surface transform or material UV scale");
    // 静态局部法线页在消费端只做一次 normal matrix。非均匀缩放/剪切时，
    // 逐层先在 world 归一化再混合不与此操作交换；S1 明确拒绝近似替代。
    const auto dotColumn = [&input](uint32_t first, uint32_t second) {
        float result = 0.0f;
        for (uint32_t row = 0; row < 3; ++row)
            result += input.localToWorld[first * 4 + row] *
                input.localToWorld[second * 4 + row];
        return result;
    };
    const float scaleSquared = dotColumn(0, 0);
    const float tolerance = std::max(scaleSquared, 1.0e-20f) * 1.0e-4f;
    if (scaleSquared <= 1.0e-20f ||
        std::abs(dotColumn(1, 1) - scaleSquared) > tolerance ||
        std::abs(dotColumn(2, 2) - scaleSquared) > tolerance ||
        std::abs(dotColumn(0, 1)) > tolerance ||
        std::abs(dotColumn(0, 2)) > tolerance ||
        std::abs(dotColumn(1, 2)) > tolerance ||
        std::abs(input.localToWorld[3]) > 1.0e-6f ||
        std::abs(input.localToWorld[7]) > 1.0e-6f ||
        std::abs(input.localToWorld[11]) > 1.0e-6f ||
        std::abs(input.localToWorld[15] - 1.0f) > 1.0e-6f)
        return fail("S1 local normal pages require an affine placement with uniform scale and no shear");
    const auto& transform = input.localToWorld;
    const float determinant =
        transform[0] * (transform[5] * transform[10] - transform[6] * transform[9]) -
        transform[4] * (transform[1] * transform[10] - transform[2] * transform[9]) +
        transform[8] * (transform[1] * transform[6] - transform[2] * transform[5]);
    if (determinant <= 0.0f)
        return fail("S1 local normal pages do not support mirrored placements");
    const std::array<const SampledTexture*, 9> textures{
        &input.coveragePageTable, &input.coverageIds, &input.coverageWeights,
        &input.layerCatalog, &input.layerAlbedo, &input.layerNormal,
        &input.layerOrm, &input.surfaceLocalNormal, &input.surfaceLocalTangent};
    for (const SampledTexture* texture : textures)
    {
        if (!HasTexture(*texture))
            return fail("Missing sampled texture metadata or readable Vulkan descriptor");
    }
    if (input.layerAlbedo.encoding != Encoding::SrgbView ||
        input.layerNormal.encoding != Encoding::Linear ||
        input.layerOrm.encoding != Encoding::Linear ||
        input.layerCatalog.encoding != Encoding::Linear ||
        input.surfaceLocalNormal.encoding != Encoding::Linear ||
        input.surfaceLocalTangent.encoding != Encoding::Linear ||
        input.coveragePageTable.encoding != Encoding::Linear ||
        input.coverageIds.encoding != Encoding::Linear ||
        input.coverageWeights.encoding != Encoding::Linear)
        return fail("Albedo requires sRGB image view; normals/ORM/catalog/coverage require linear views");
    if (input.layerCatalog.width != 256 || input.layerCatalog.height != 4 ||
        input.layerCatalog.layers != 1 ||
        input.coveragePageTable.layers != 1 ||
        input.coverageIds.layers != 1 || input.coverageWeights.layers != 1 ||
        input.surfaceLocalNormal.layers != 1 || input.surfaceLocalTangent.layers != 1)
        return fail("Invalid catalog LUT or non-array surface/coverage texture shape");
    if (input.layerAlbedo.layers != input.layerNormal.layers ||
        input.layerAlbedo.layers != input.layerOrm.layers)
        return fail("Source albedo/normal/ORM slice counts differ");
    if (input.coveragePageResolution == 0 ||
        input.coverageIds.width != input.coverageWeights.width ||
        input.coverageIds.height != input.coverageWeights.height ||
        input.coverageIds.width % input.coveragePageResolution != 0 ||
        input.coverageIds.height % input.coveragePageResolution != 0)
        return fail("Invalid coverage atlas resolution");
    if (input.weightSource == WeightSource::LocalLayerArray)
    {
        if (!HasTexture(input.localLayerWeights) ||
            input.localLayerWeights.encoding != Encoding::Linear ||
            input.localLayerWeights.width < 4 || input.localLayerWeights.height < 4 ||
            input.localLayerCount == 0 || input.localLayerCount > 256 ||
            input.localLayerCount > input.localLayerWeights.layers)
            return fail("Invalid local weight array with one-texel margin");
        for (uint32_t localLayer = 0; localLayer < input.localLayerCount; ++localLayer)
        {
            const uint32_t globalLayer = input.localToGlobalLayer[localLayer];
            if (globalLayer >= 256 ||
                input.globalToSourceSlice[globalLayer] >= input.layerAlbedo.layers)
                return fail("Unmapped local layer in weight array");
        }
    }
    if (input.weightSource != WeightSource::CoverageAtlas &&
        input.weightSource != WeightSource::LocalLayerArray)
        return fail("Unknown weight source");
    bool hasMapping = false;
    for (uint32_t slice : input.globalToSourceSlice)
    {
        if (slice == InvalidSlice)
            continue;
        if (slice >= input.layerAlbedo.layers)
            return fail("Global layer references a missing source slice");
        hasMapping = true;
    }
    if (!hasMapping)
        return fail("No mapped source layers");
    return true;
}

bool TerrainCompositedBakeGpu::Record(
    vk::CommandBuffer commandBuffer,
    uint32_t slot,
    const InputSnapshot& input,
    const PageRequest& page,
    const vk::DescriptorBufferInfo& output,
    std::string& error)
{
    return Record(commandBuffer, slot, input, page, output, CaptureProducer(), error);
}

bool TerrainCompositedBakeGpu::Record(
    vk::CommandBuffer commandBuffer, uint32_t slot, const InputSnapshot& input,
    const PageRequest& page, const vk::DescriptorBufferInfo& output,
    const ProducerSnapshot& selectedProducer, std::string& error)
{
    if (slots.empty() || slot >= slots.size() - 1)
    {
        error = available ? "Invalid asynchronous bake slot" : unavailableReason;
        return false;
    }
    try
    {
        return RecordPage(commandBuffer, slot, input, page, output, selectedProducer, error);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool TerrainCompositedBakeGpu::RecordPage(
    vk::CommandBuffer commandBuffer,
    uint32_t slotIndex,
    const InputSnapshot& input,
    const PageRequest& page,
    const vk::DescriptorBufferInfo& output,
    const ProducerSnapshot& selectedProducer, std::string& error)
{
    if (!available)
    {
        error = unavailableReason;
        return false;
    }
    if (input.adjacency)
        return RecordAdjacency(commandBuffer, slotIndex, *input.adjacency,
            input.surfaceId.empty() ? input.adjacency->sourceSurfaceId : input.surfaceId,
            page, output, input.adjacencyScratch, selectedProducer, error);
    if (!Validate(input, page, error))
        return false;
    AdjacencySnapshot isolated;
    isolated.sourceSurfaceId = input.surfaceId.empty() ? "terrain|isolated-bake" : input.surfaceId;
    isolated.graph = TerrainCompositedAdjacencyGraph({isolated.sourceSurfaceId}, {});
    isolated.surfaces.push_back({isolated.sourceSurfaceId, input});
    return RecordAdjacency(commandBuffer, slotIndex, isolated, isolated.sourceSurfaceId,
        page, output, {}, selectedProducer, error);
}

TerrainCompositedBakeGpu::GpuParams TerrainCompositedBakeGpu::MakeParams(
    const InputSnapshot& input, const PageRequest& page)
{
    GpuParams params;
    params.page = {MipExtent(page.virtualWidth, page.virtualMip),
        MipExtent(page.virtualHeight, page.virtualMip), page.pageX, page.pageY};
    params.coverage = {input.coveragePageTable.width, input.coveragePageTable.height,
        input.coveragePageResolution,
        input.weightSource == WeightSource::LocalLayerArray ? 1u : 0u};
    params.counts = {input.localLayerCount, input.layerAlbedo.layers,
        page.areaSamplesPerAxis, page.repeatSurface ? 1u : 0u};
    params.surfaceUv = {input.surfaceUvOffset[0], input.surfaceUvOffset[1],
        input.surfaceUvScale[0], input.surfaceUvScale[1]};
    params.coverageUv = {input.coverageUvOffset[0], input.coverageUvOffset[1],
        input.coverageUvScale[0], input.coverageUvScale[1]};
    params.frameUv = {input.frameUvOffset[0], input.frameUvOffset[1],
        input.frameUvScale[0], input.frameUvScale[1]};
    params.material[0] = input.uvScale;
    params.material[1] = input.retainedFragment.heightBlendStrength;
    params.material[2] = std::max(0.0f, input.frozenHeightBlendFade);
    params.globalToSlice = input.globalToSourceSlice;
    params.localToGlobal = input.localToGlobalLayer;
    return params;
}

void TerrainCompositedBakeGpu::WriteDescriptors(
    vk::DescriptorSet descriptors, const std::vector<const InputSnapshot*>& inputs,
    const vk::DescriptorBufferInfo& params, const vk::DescriptorBufferInfo& output,
    const vk::DescriptorBufferInfo& charts)
{
    const std::array<vk::DescriptorBufferInfo, 3> buffers{params, output, charts};
    std::array<std::array<vk::DescriptorImageInfo, MaxSurfaces>, 11> images;
    for (uint32_t index = 0; index < MaxSurfaces; ++index)
    {
        const auto& input = *inputs[std::min<size_t>(index, inputs.size() - 1)];
        const std::array<const vk::DescriptorImageInfo*, 11> surfaceImages{
            &input.coveragePageTable.descriptor, &input.coverageIds.descriptor,
            &input.coverageWeights.descriptor,
            input.weightSource == WeightSource::LocalLayerArray
                ? &input.localLayerWeights.descriptor : &input.layerOrm.descriptor,
            &input.layerCatalog.descriptor, &input.layerAlbedo.descriptor,
            &input.layerNormal.descriptor, &input.layerOrm.descriptor,
            &input.surfaceLocalNormal.descriptor, &input.surfaceLocalTangent.descriptor,
            input.retainedFragment.heightBlendStrength > 0.0f
                ? &input.layerHeight.descriptor : &input.layerOrm.descriptor};
        for (uint32_t role = 0; role < images.size(); ++role)
            images[role][index] = *surfaceImages[role];
    }
    std::vector<vk::WriteDescriptorSet> writes(14);
    for (uint32_t binding = 0; binding < writes.size(); ++binding)
    {
        writes[binding].setDstSet(descriptors).setDstBinding(binding)
            .setDescriptorCount(1);
        if (binding < 2 || binding == 13)
            writes[binding].setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(buffers[binding < 2 ? binding : 2]);
        else
            writes[binding].setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(images[binding - 2]);
    }
    backend->UpdateDescriptorSets(writes);
}

std::string TerrainCompositedBakeGpu::DescribeStatus(uint32_t status)
{
    std::string result;
    if (status & 1u)
        result += "missing coverage page; ";
    if (status & 2u)
        result += "coverage mapping outside atlas; ";
    if (status & 4u)
        result += "unmapped global layer/source slice; ";
    if (status & 8u)
        result += "invalid local surface normal/tangent; ";
    if (status & 16u)
        result += "non-finite material or weight input; ";
    if (status & 32u)
        result += "invalid world adjacency route; ";
    if (status & ~63u)
        result += "unknown GPU diagnostic; ";
    return result;
}

uint64_t TerrainCompositedBakeGpu::ParameterBytes(uint32_t slotCount, uint32_t capacity)
{
    return uint64_t(slotCount + 1) * (sizeof(GpuWorldParams) + capacity * 64);
}

uint32_t TerrainCompositedBakeGpu::RequiredScratchBytes(uint32_t samples)
{
    if (samples < 1 || samples > 8)
        throw std::runtime_error("Scratch sample count must be 1..8");
    return 0;
}

bool TerrainCompositedBakeGpu::ValidateDeviceLimits(
    const vk::PhysicalDeviceLimits& limits, std::string& error)
{
    error.clear();
    constexpr uint32_t Samplers = MaxSurfaces * 11;
    if (limits.maxPerStageDescriptorSamplers < Samplers ||
        limits.maxPerStageDescriptorSampledImages < Samplers ||
        limits.maxDescriptorSetSamplers < Samplers ||
        limits.maxDescriptorSetSampledImages < Samplers ||
        limits.maxPerStageResources < Samplers + 3 ||
        limits.maxPerStageDescriptorStorageBuffers < 3 ||
        limits.maxDescriptorSetStorageBuffers < 3 ||
        limits.maxStorageBufferRange < sizeof(GpuWorldParams))
    {
        error = "World terrain baker requires 176 combined samplers, 3 storage buffers and 35872-byte storage range";
        return false;
    }
    return true;
}
bool TerrainCompositedBakeGpu::Record(
    vk::CommandBuffer commandBuffer, uint32_t slot, const AdjacencySnapshot& input,
    const PageRequest& page, const vk::DescriptorBufferInfo& output,
    const vk::DescriptorBufferInfo& scratch, std::string& error)
{
    return Record(commandBuffer, slot, input, page, output, scratch, CaptureProducer(), error);
}

bool TerrainCompositedBakeGpu::Record(
    vk::CommandBuffer commandBuffer, uint32_t slot, const AdjacencySnapshot& input,
    const PageRequest& page, const vk::DescriptorBufferInfo& output,
    const vk::DescriptorBufferInfo& scratch, const ProducerSnapshot& selectedProducer,
    std::string& error)
{
    if (slots.empty() || slot >= slots.size() - 1)
    {
        error = "Invalid asynchronous adjacency bake slot";
        return false;
    }
    return RecordAdjacency(commandBuffer, slot, input, input.sourceSurfaceId,
        page, output, scratch, selectedProducer, error);
}

bool TerrainCompositedBakeGpu::RecordAdjacency(
    vk::CommandBuffer commandBuffer, uint32_t slotIndex, const AdjacencySnapshot& snapshot,
    const std::string& sourceId, const PageRequest& page,
    const vk::DescriptorBufferInfo& output, const vk::DescriptorBufferInfo& scratch,
    const ProducerSnapshot& selectedProducer, std::string& error)
{
    error.clear();
    try
    {
        (void)scratch;
        if (!available || slotIndex >= slots.size())
            throw std::runtime_error("Adjacency baker unavailable or invalid slot");
        if (!selectedProducer || !selectedProducer->DescriptorResources().owner ||
            selectedProducer->DescriptorResources().world.size() != slots.size() ||
            selectedProducer->DescriptorResources().padding.size() != slots.size())
            throw std::runtime_error("Missing terrain producer descriptor snapshot");
        const auto& pipeline = selectedProducer->Pipelines()[0].pipeline;
        const auto& paddingPipeline = selectedProducer->Pipelines()[1].pipeline;
        const auto descriptors = selectedProducer->DescriptorResources().world[slotIndex];
        const auto paddingDescriptors = selectedProducer->DescriptorResources().padding[slotIndex];
        if (!pipeline || !paddingPipeline)
            throw std::runtime_error("Missing terrain producer pipeline pair");
        if (!commandBuffer || !output.buffer || output.offset != 0 ||
            output.range == VK_WHOLE_SIZE || output.range < OutputBytes)
            throw std::runtime_error("Adjacency requires explicit output range at offset zero");
        std::vector<std::string> ids;
        for (const auto& surface : snapshot.surfaces)
            ids.push_back(surface.surfaceId);
        snapshot.graph.Validate(ids);
        const auto charts = TerrainBakeAdjacency::Prepare(snapshot.graph, sourceId);
        if (charts.size() > surfaceCapacity)
            throw std::runtime_error("Adjacency component exceeds allocated surface capacity");
        const auto findInput = [&](const std::string& id) -> const InputSnapshot& {
            const auto found = std::find_if(snapshot.surfaces.begin(), snapshot.surfaces.end(),
                [&](const SurfaceInput& surface) { return surface.surfaceId == id; });
            if (found == snapshot.surfaces.end())
                throw std::runtime_error("Missing adjacency input: " + id);
            return found->input;
        };
        const auto& source = findInput(sourceId);
        TerrainBakeAdjacency::ValidateAddressMode(snapshot.graph, sourceId, page.repeatSurface);
        GpuWorldParams params;
        std::vector<const InputSnapshot*> inputs;
        std::vector<std::array<float, 16>> gpuCharts;
        uint32_t sourceIndex = 0;
        for (size_t index = 0; index < charts.size(); ++index)
        {
            const auto& chart = charts[index];
            const auto& input = findInput(chart.surfaceId);
            if (input.adjacency)
                throw std::runtime_error("World surface table must contain leaf snapshots without adjacency owners");
            if (!Validate(input, page, error))
                return false;
            if (chart.surfaceId == sourceId)
                sourceIndex = static_cast<uint32_t>(index);
            auto value = MakeParams(input, page);
            value.adjacency = {1, static_cast<uint32_t>(index),
                static_cast<uint32_t>(charts.size()), 0};
            value.normalToSource = TerrainBakeAdjacency::NormalToSource(
                source.localToWorld, input.localToWorld);
            params.surfaces[index] = value;
            inputs.push_back(&input);
            const auto packed = TerrainBakeAdjacency::Pack(chart);
            if (!Finite(packed) || !Finite(value.normalToSource))
                throw std::runtime_error("Adjacency chart or normal transform exceeds float range");
            gpuCharts.push_back(packed);
        }
        Slot& slot = slots[slotIndex];
        if (TerrainBakeAdjacency::RectangularBounds(charts, gpuCharts))
            params.surfaces[0].adjacency[0] = 2;
        params.page = {MipExtent(page.virtualWidth, page.virtualMip),
            MipExtent(page.virtualHeight, page.virtualMip), page.pageX, page.pageY};
        params.control = {page.areaSamplesPerAxis, page.repeatSurface ? 1u : 0u,
            sourceIndex, static_cast<uint32_t>(charts.size())};
        std::memcpy(slot.mapped, &params, sizeof(params));
        std::memcpy(slot.chartsMapped, gpuCharts.data(), gpuCharts.size() * 64);
        const vk::DescriptorBufferInfo chartBuffer(slot.charts, 0, gpuCharts.size() * 64);
        const vk::DescriptorBufferInfo parameterBuffer(slot.params, 0, sizeof(params));
        slot.producer = selectedProducer;
        WriteDescriptors(descriptors, inputs, parameterBuffer, output, chartBuffer);
        const std::array<vk::DescriptorBufferInfo, 2> paddingBuffers{parameterBuffer, output};
        std::vector<vk::WriteDescriptorSet> finalWrites(2);
        for (uint32_t binding = 0; binding < 2; ++binding)
            finalWrites[binding].setDstSet(paddingDescriptors).setDstBinding(binding)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer).setDescriptorCount(1)
                .setBufferInfo(paddingBuffers[binding]);
        backend->UpdateDescriptorSets(finalWrites);
        const auto diagnosticPool = source.diagnosticQueries;
        const uint32_t diagnosticFirst = source.diagnosticFirstQuery;
        const auto timestamp = [&](uint32_t offset) {
            if (diagnosticPool)
                commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                    diagnosticPool, diagnosticFirst + offset);
        };
        timestamp(0);
        commandBuffer.fillBuffer(output.buffer, StatusOffset, 4, 0);
        vk::MemoryBarrier barrier;
        barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eComputeShader, {}, barrier, nullptr, nullptr);
        pipeline->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
            pipeline->GetPipelineLayout(), 0, descriptors, nullptr);
        const uint32_t activeWidth = page.repeatSurface ? StoredExtent :
            std::min(128u, params.page[0] - page.pageX * 128) + 8;
        const uint32_t activeHeight = page.repeatSurface ? StoredExtent :
            std::min(128u, params.page[1] - page.pageY * 128) + 8;
        const uint32_t sampleCount = page.areaSamplesPerAxis * page.areaSamplesPerAxis;
        const uint32_t lanesPerTexel = sampleCount <= 1 ? 1 : sampleCount <= 4 ? 4 :
            sampleCount <= 16 ? 8 : 64;
        const uint32_t texelsPerGroup = 64 / lanesPerTexel;
        commandBuffer.dispatch((activeWidth + texelsPerGroup - 1) / texelsPerGroup, activeHeight, 1);
        timestamp(1);
        barrier.setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
        if (activeWidth != StoredExtent || activeHeight != StoredExtent)
        {
            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                vk::PipelineStageFlagBits::eComputeShader, {}, barrier, nullptr, nullptr);
            paddingPipeline->Bind(commandBuffer);
            commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                paddingPipeline->GetPipelineLayout(), 0, paddingDescriptors, nullptr);
            commandBuffer.dispatch(17, 17, 1);
        }
        timestamp(2);
        barrier.setDstAccessMask(vk::AccessFlagBits::eTransferRead);
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eTransfer, {}, barrier, nullptr, nullptr);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool TerrainCompositedBakeGpu::BakePageSync(
    const AdjacencySnapshot& snapshot, const PageRequest& page, PageBytes& result,
    std::string& error)
{
    const auto found = std::find_if(snapshot.surfaces.begin(), snapshot.surfaces.end(),
        [&](const SurfaceInput& surface) { return surface.surfaceId == snapshot.sourceSurfaceId; });
    if (found == snapshot.surfaces.end())
    {
        error = "Missing synchronous adjacency source";
        return false;
    }
    InputSnapshot input = found->input;
    input.surfaceId = snapshot.sourceSurfaceId;
    input.adjacency = std::make_shared<AdjacencySnapshot>(snapshot);
    input.cookWorkspaceLease = snapshot.cookWorkspaceLease;
    return BakePageSync(input, page, result, error);
}

bool TerrainCompositedBakeGpu::BakePageSync(
    const InputSnapshot& input,
    const PageRequest& page,
    PageBytes& result,
    std::string& error)
{
    error.clear();
    if (!available)
    {
        error = unavailableReason;
        return false;
    }
    if (!Validate(input, page, error))
        return false;
    if (input.adjacency && page.repeatSurface)
    {
        const std::string& sourceId = input.surfaceId.empty()
            ? input.adjacency->sourceSurfaceId : input.surfaceId;
        const auto found = std::find_if(input.adjacency->surfaces.begin(), input.adjacency->surfaces.end(),
            [&](const SurfaceInput& surface) { return surface.surfaceId == sourceId; });
        if (found == input.adjacency->surfaces.end() || found->input.adjacency)
        {
            error = "Isolated repeat Cook requires a leaf surface input";
            return false;
        }
        return BakePageSync(found->input, page, result, error);
    }
    if (input.adjacency && (!input.cookWorkspaceLease ||
        input.cookWorkspaceLease->Cost().gpuBytes <
            uint64_t(RequiredScratchBytes(page.areaSamplesPerAxis)) + 2 * OutputBytes ||
        input.cookWorkspaceLease->Cost().cpuBytes < OutputBytes))
    {
        error = "Synchronous adjacency Cook requires an external workspace lease covering actual output/readback/scratch allocations and metadata";
        return false;
    }
    vk::Buffer output{};
    vk::DeviceMemory outputMemory{};
    vk::Buffer readback{};
    vk::DeviceMemory readbackMemory{};
    void* mapped = nullptr;
    vk::CommandBuffer commandBuffer{};
    vk::QueryPool timing{};
    const auto cleanup = [&] {
        if (commandBuffer)
            backend->AbortSingleTimeCommands(commandBuffer);
        if (mapped)
            backend->UnmapMemory(readbackMemory);
        if (readback)
            backend->DestroyBuffer(readback, readbackMemory);
        if (output)
            backend->DestroyBuffer(output, outputMemory);
        if (timing)
            backend->DestroyQueryPool(timing);
    };
    try
    {
        const auto deviceBuffer = backend->CreateBuffer(OutputBytes,
            vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, "TerrainCompositedBakeOutput");
        output = deviceBuffer.first;
        outputMemory = deviceBuffer.second;
        const auto hostBuffer = backend->CreateBuffer(OutputBytes,
            vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TerrainCompositedBakeReadback");
        readback = hostBuffer.first;
        readbackMemory = hostBuffer.second;
        mapped = backend->MapMemory(readbackMemory, OutputBytes);
        InputSnapshot recordedInput = input;
        commandBuffer = backend->BeginSingleTimeCommands();
        const bool measure = std::getenv("VL_GPU_TIMING_RAW") != nullptr;
        if (measure)
        {
            if (!backend->SupportsTimestampQueries() || !backend->GetGraphicsTimestampValidBits())
                throw std::runtime_error("Bake GPU timestamps unavailable");
            timing = backend->CreateTimestampQueryPool(4, "TerrainP0BakeTiming");
            commandBuffer.resetQueryPool(timing, 0, 4);
            commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, timing, 0);
        }
        if (!RecordPage(commandBuffer, static_cast<uint32_t>(slots.size() - 1),
            recordedInput, page, vk::DescriptorBufferInfo(output, 0, OutputBytes), CaptureProducer(), error))
            throw std::runtime_error(error);
        if (measure)
        {
            commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, timing, 1);
            commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, timing, 2);
        }
        commandBuffer.copyBuffer(output, readback, vk::BufferCopy(0, 0, OutputBytes));
        if (measure)
            commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, timing, 3);
        vk::MemoryBarrier host;
        host.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setDstAccessMask(vk::AccessFlagBits::eHostRead);
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eHost, {}, host, nullptr, nullptr);
        backend->EndSingleTimeCommands(commandBuffer);
        commandBuffer = nullptr;
        if (measure)
        {
            std::array<uint64_t, 2> productionTicks{}, copyTicks{};
            backend->ReadTimestampQueryPair(timing, 0, productionTicks);
            backend->ReadTimestampQueryPair(timing, 2, copyTicks);
            const double scale = backend->GetTimestampPeriodNanoseconds() / 1000000.0;
            if (productionTicks[1] < productionTicks[0] || copyTicks[1] < copyTicks[0])
                throw std::runtime_error("Invalid bake GPU timestamp");
            std::cout << "[BAKE_GPU_RAW] {\"mip\":" << page.virtualMip
                << ",\"x\":" << page.pageX << ",\"y\":" << page.pageY
                << ",\"weightSource\":" << static_cast<uint32_t>(input.weightSource)
                << ",\"heightBlend\":" << input.retainedFragment.heightBlendStrength
                << ",\"productionMs\":" << (productionTicks[1] - productionTicks[0]) * scale
                << ",\"readbackCopyMs\":" << (copyTicks[1] - copyTicks[0]) * scale << "}\n";
        }
        uint32_t status = 0;
        const auto* bytes = static_cast<const uint8_t*>(mapped);
        std::memcpy(&status, bytes + StatusOffset, sizeof(status));
        if (status != 0)
            throw std::runtime_error("Composited GPU bake rejected: " + DescribeStatus(status));
        PageBytes candidate;
        candidate.albedoSrgb.assign(bytes, bytes + ChannelBytes);
        candidate.normalLocalUnorm.assign(bytes + ChannelBytes, bytes + 2 * ChannelBytes);
        candidate.ormLinear.assign(bytes + 2 * ChannelBytes, bytes + PackedBytes);
        candidate.contentIdentity = input.contentIdentity;
        candidate.page = page;
        result = std::move(candidate);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        cleanup();
        return false;
    }
    cleanup();
    return true;
}

}
