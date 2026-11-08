#include "render/resource/terrainPageBakeGpu.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

#include "commonFunction.h"
#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererTerrainLoader.h"
#include "terrain/terrainHeightmap.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainWeightMap.h"
#include "texture.h"

namespace VL
{
namespace
{

constexpr uint32_t PageTexels =
    TerrainPageManager::PageResolution * TerrainPageManager::PageResolution;
constexpr vk::DeviceSize PageBytes =
    static_cast<vk::DeviceSize>(PageTexels) * 4;
constexpr vk::DeviceSize ParamsAlignment = 256;

struct GpuLayer
{
    float heightMin = 0.0f;
    float heightMax = 1.0f;
    float slopeMin = 0.0f;
    float slopeMax = 90.0f;
    float noise = 0.0f;
    uint32_t globalId = 0;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
};

static_assert(sizeof(GpuLayer) == 32, "GPU layer ABI must stay 32 bytes");

vk::DeviceSize AlignUp(vk::DeviceSize value, vk::DeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// .vlpaint 的 opacity 是 uint8；打包成每 4 个 texel 一个 uint，shader 按
// byte 抽取。这样 512²×4 层的 opacity 只占 256 KiB/placement。
std::vector<uint32_t> PackOpacity(const std::vector<uint8_t>& opacity)
{
    std::vector<uint32_t> packed(
        (opacity.size() + 3) / 4, 0u);
    for (size_t index = 0; index < opacity.size(); ++index)
    {
        packed[index >> 2] |=
            static_cast<uint32_t>(opacity[index]) << ((index & 3) * 8);
    }
    return packed;
}

} // namespace

// std430；字段顺序与 terrain/terrainPageBake.comp 的 BakeParams 完全一致。
struct TerrainPageBakeGpu::JobParams
{
    uint32_t tileX = 0;
    uint32_t tileZ = 0;
    uint32_t tilesPerSide = 0;
    uint32_t layerCount = 0;
    uint32_t resolution = 0;
    uint32_t pageResolution = 0;
    uint32_t heightWidth = 0;
    uint32_t heightHeight = 0;
    uint32_t paintWidth = 0;
    uint32_t paintHeight = 0;
    uint32_t hasPaint = 0;
    uint32_t candidateCount = 0;
    float heightScale = 0.0f;
    float heightOffset = 0.0f;
    float uvOffsetU = 0.0f;
    float uvOffsetV = 0.0f;
    float uvScaleU = 1.0f;
    float uvScaleV = 1.0f;
    float sizeX = 0.0f;
    float sizeY = 0.0f;
    uint32_t candidates[256] = {};
};

TerrainPageBakeGpu::TerrainPageBakeGpu(
    RendererBackendVulkan& rendererBackend,
    PipelineFactory& pipelineFactory,
    TerrainPageManager& pageManager)
    : rendererBackend(&rendererBackend)
    , pageManager(&pageManager)
{
    try
    {
        pipeline = pipelineFactory.CreateComputePipelineCandidate(
            "terrain/terrainPageBake");
        Initialize();
        available = true;
    }
    catch (const std::exception& exception)
    {
        unavailableReason = exception.what();
        available = false;
        // 半初始化资源按成员析构顺序回收；pipeline 也在这里释放。
        pipeline.reset();
        std::cerr << "[Terrain] GPU page bake disabled: "
                  << unavailableReason << std::endl;
    }
}

TerrainPageBakeGpu::~TerrainPageBakeGpu()
{
    if (rendererBackend == nullptr)
        return;
    for (uint32_t slot = 0; slot < frameSlotCount; ++slot)
    {
        if (slot < paramsMapped.size() && paramsMapped[slot] != nullptr &&
            slot < paramsBuffers.size())
        {
            rendererBackend->UnmapMemory(paramsBuffers[slot].memory);
        }
    }
    for (PlacementInput& input : placements)
        DestroyPlacementInput(input);
    DestroyBuffer(dummyBuffer);
    for (BufferResource& buffer : paramsBuffers)
        DestroyBuffer(buffer);
    for (BufferResource& buffer : outputBuffers)
        DestroyBuffer(buffer);
    if (descriptorPool)
    {
        rendererBackend->DestroyDescriptorPool(descriptorPool);
        descriptorPool = nullptr;
    }
    pipeline.reset();
}

void TerrainPageBakeGpu::Initialize()
{
    if (!pipeline)
        throw std::runtime_error("Terrain page bake compute pipeline is missing");
    if (!rendererBackend)
        throw std::runtime_error("Terrain page bake requires a renderer backend");
    if (!pageManager)
        throw std::runtime_error("Terrain page bake requires a page manager");

    frameSlotCount = rendererBackend->GetFrameFenceCount();
    if (frameSlotCount == 0)
        throw std::runtime_error(
            "Terrain page bake requires at least one frame fence");

    paramsStride = AlignUp(sizeof(JobParams), ParamsAlignment);
    placements.resize(pageManager->Plans().size());
    for (size_t index = 0; index < placements.size(); ++index)
    {
        placements[index] =
            CreatePlacementInput(*pageManager, index);
    }

    dummyBuffer = CreateDeviceLocalBuffer(
        16,
        vk::BufferUsageFlagBits::eStorageBuffer,
        nullptr,
        "TerrainPageBakeDummy");

    paramsBuffers.reserve(frameSlotCount);
    paramsMapped.reserve(frameSlotCount);
    outputBuffers.reserve(frameSlotCount);
    for (uint32_t slot = 0; slot < frameSlotCount; ++slot)
    {
        void* mapped = nullptr;
        paramsBuffers.push_back(CreateHostBuffer(
            paramsStride * MaxJobsPerFrame,
            vk::BufferUsageFlagBits::eStorageBuffer,
            "TerrainPageBakeParams" + std::to_string(slot),
            &mapped));
        paramsMapped.push_back(mapped);
        outputBuffers.push_back(CreateDeviceLocalBuffer(
            2 * MaxJobsPerFrame * PageBytes,
            vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eTransferSrc,
            nullptr,
            "TerrainPageBakeOutput" + std::to_string(slot)));
    }

    const uint32_t maxSets = frameSlotCount * MaxJobsPerFrame;
    std::array<vk::DescriptorPoolSize, 1> poolSizes = {
        vk::DescriptorPoolSize(
            vk::DescriptorType::eStorageBuffer,
            7 * maxSets)
    };
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo.setPoolSizes(poolSizes).setMaxSets(maxSets);
    descriptorPool = rendererBackend->CreateDescriptorPool(
        poolInfo, "TerrainPageBakeDescriptorPool");

    const std::vector<vk::DescriptorSetLayout> layouts(
        MaxJobsPerFrame,
        pipeline->GetDescriptorSetLayouts()[0]);
    descriptorSets.resize(frameSlotCount);
    for (uint32_t slot = 0; slot < frameSlotCount; ++slot)
    {
        vk::DescriptorSetAllocateInfo allocateInfo;
        allocateInfo
            .setDescriptorPool(descriptorPool)
            .setSetLayouts(layouts);
        descriptorSets[slot].resize(MaxJobsPerFrame);
        rendererBackend->AllocateDescriptorSets(
            allocateInfo, descriptorSets[slot]);
        for (uint32_t job = 0; job < MaxJobsPerFrame; ++job)
        {
            rendererBackend->SetDescriptorSetDebugName(
                descriptorSets[slot][job],
                "TerrainPageBake_" + std::to_string(slot) + "_" +
                    std::to_string(job));
        }
    }
}

TerrainPageBakeGpu::BufferResource TerrainPageBakeGpu::CreateDeviceLocalBuffer(
    vk::DeviceSize size,
    vk::BufferUsageFlags usage,
    const void* data,
    const std::string& debugName)
{
    BufferResource result;
    if (size == 0)
        size = 16;
    const std::pair<vk::Buffer, vk::DeviceMemory> destination =
        rendererBackend->CreateBuffer(
            size,
            usage | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            debugName);
    result.buffer = destination.first;
    result.memory = destination.second;

    if (data == nullptr)
        return result;

    try
    {
        const std::pair<vk::Buffer, vk::DeviceMemory> staging =
            rendererBackend->CreateBuffer(
                size,
                vk::BufferUsageFlagBits::eTransferSrc,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                debugName + "Staging");
        void* mapped = rendererBackend->MapMemory(staging.second, size);
        std::memcpy(mapped, data, static_cast<size_t>(size));
        rendererBackend->UnmapMemory(staging.second);
        rendererBackend->CopyBufferToBuffer(
            staging.first, result.buffer, size);
        rendererBackend->DestroyBuffer(staging.first, staging.second);
    }
    catch (...)
    {
        rendererBackend->DestroyBuffer(result.buffer, result.memory);
        throw;
    }
    return result;
}

TerrainPageBakeGpu::BufferResource TerrainPageBakeGpu::CreateHostBuffer(
    vk::DeviceSize size,
    vk::BufferUsageFlags usage,
    const std::string& debugName,
    void** mapped)
{
    BufferResource result;
    const std::pair<vk::Buffer, vk::DeviceMemory> created =
        rendererBackend->CreateBuffer(
            size,
            usage,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            debugName);
    result.buffer = created.first;
    result.memory = created.second;
    *mapped = rendererBackend->MapMemory(result.memory, size);
    return result;
}

void TerrainPageBakeGpu::DestroyBuffer(BufferResource& resource)
{
    if (rendererBackend != nullptr && resource.buffer)
    {
        rendererBackend->DestroyBuffer(resource.buffer, resource.memory);
    }
    resource = {};
}

void TerrainPageBakeGpu::DestroyPlacementInput(PlacementInput& input)
{
    DestroyBuffer(input.heights);
    DestroyBuffer(input.layers);
    DestroyBuffer(input.paintWeights);
    DestroyBuffer(input.paintOpacityPacked);
    input = {};
}

TerrainPageBakeGpu::PlacementInput TerrainPageBakeGpu::CreatePlacementInput(
    const TerrainPageManager& manager,
    size_t registrationIndex)
{
    PlacementInput input;
    const std::vector<TerrainBuildPlan>& plans = manager.Plans();
    if (registrationIndex >= plans.size())
        return input;
    const TerrainBuildPlan& plan = plans[registrationIndex];
    if (!plan.heightmap)
        return input;

    const HostImage& heightmap = *plan.heightmap;
    const size_t heightCount =
        static_cast<size_t>(heightmap.width) * heightmap.height;
    std::vector<float> heights(heightCount);
    for (uint32_t y = 0; y < heightmap.height; ++y)
    {
        for (uint32_t x = 0; x < heightmap.width; ++x)
        {
            heights[static_cast<size_t>(y) * heightmap.width + x] =
                TerrainHeightmap::ReadNormalizedPixel(heightmap, x, y);
        }
    }
    input.heights = CreateDeviceLocalBuffer(
        heights.size() * sizeof(float),
        vk::BufferUsageFlagBits::eStorageBuffer,
        heights.data(),
        "TerrainPageBakeHeight" + std::to_string(registrationIndex));
    input.heightWidth = heightmap.width;
    input.heightHeight = heightmap.height;

    const std::vector<uint8_t>& layerIdMap =
        manager.PlacementLayerIdMap(
            TerrainPlacementIdentity(plan));
    std::vector<GpuLayer> layers(plan.desc.layers.size());
    for (size_t layer = 0; layer < plan.desc.layers.size(); ++layer)
    {
        const TerrainLayerDesc& desc = plan.desc.layers[layer];
        GpuLayer& gpu = layers[layer];
        gpu.heightMin = desc.heightMin;
        gpu.heightMax = desc.heightMax;
        gpu.slopeMin = desc.slopeMin;
        gpu.slopeMax = desc.slopeMax;
        gpu.noise = desc.noise;
        gpu.globalId = layerIdMap.empty()
            ? static_cast<uint32_t>(layer)
            : static_cast<uint32_t>(layerIdMap[layer]);
    }
    const vk::DeviceSize layerBytes =
        static_cast<vk::DeviceSize>(layers.size()) * sizeof(GpuLayer);
    input.layers = CreateDeviceLocalBuffer(
        std::max<vk::DeviceSize>(16, layerBytes),
        vk::BufferUsageFlagBits::eStorageBuffer,
        layers.empty() ? nullptr : layers.data(),
        "TerrainPageBakeLayers" + std::to_string(registrationIndex));
    input.layerCount = static_cast<uint32_t>(layers.size());

    if (plan.layerPaint && !plan.layerPaint->Empty())
    {
        const TerrainLayerPaintData& paint = *plan.layerPaint;
        std::vector<uint32_t> opacity = PackOpacity(paint.opacity);
        input.paintWeights = CreateDeviceLocalBuffer(
            paint.weights.size() * sizeof(float),
            vk::BufferUsageFlagBits::eStorageBuffer,
            paint.weights.data(),
            "TerrainPageBakePaintWeights" + std::to_string(registrationIndex));
        input.paintOpacityPacked = CreateDeviceLocalBuffer(
            opacity.size() * sizeof(uint32_t),
            vk::BufferUsageFlagBits::eStorageBuffer,
            opacity.data(),
            "TerrainPageBakePaintOpacity" + std::to_string(registrationIndex));
        input.paintWidth = paint.width;
        input.paintHeight = paint.height;
        input.hasPaint = true;
    }
    input.heightmapIdentity = plan.heightmap.get();
    input.paintIdentity = plan.layerPaint.get();
    return input;
}

void TerrainPageBakeGpu::RefreshPlacement(const std::string& placementId)
{
    if (rendererBackend == nullptr || pageManager == nullptr)
        return;
    const std::vector<TerrainPageManager::Registration>& registrations =
        pageManager->Registrations();
    for (size_t index = 0; index < registrations.size(); ++index)
    {
        if (registrations[index].placementId != placementId)
            continue;
        PlacementInput replacement =
            CreatePlacementInput(*pageManager, index);
        if (index >= placements.size())
            placements.resize(index + 1);
        DestroyPlacementInput(placements[index]);
        placements[index] = std::move(replacement);
        return;
    }
}

void TerrainPageBakeGpu::FillParams(
    JobParams& params,
    const TerrainPageBakeJob& job,
    uint32_t) const
{
    const TerrainBuildPlan& plan =
        pageManager->Plans()[job.registrationIndex];
    const TerrainHeightmap::MeshUvTransform transform =
        TerrainHeightmap::MeshUvTransformFor(plan.desc, *plan.heightmap);
    const PlacementInput& input = placements[job.registrationIndex];

    params.tileX = job.tileX;
    params.tileZ = job.tileZ;
    params.tilesPerSide =
        pageManager->Registrations()[job.registrationIndex].tilesPerSide;
    params.layerCount = input.layerCount;
    params.resolution =
        params.tilesPerSide * TerrainPageManager::PageResolution;
    params.pageResolution = TerrainPageManager::PageResolution;
    params.heightWidth = input.heightWidth;
    params.heightHeight = input.heightHeight;
    params.paintWidth = input.paintWidth;
    params.paintHeight = input.paintHeight;
    params.hasPaint = input.hasPaint ? 1u : 0u;
    params.heightScale = plan.desc.heightScale;
    params.heightOffset = plan.desc.heightOffset;
    params.uvOffsetU = transform.offsetU;
    params.uvOffsetV = transform.offsetV;
    params.uvScaleU = transform.scaleU;
    params.uvScaleV = transform.scaleV;
    params.sizeX = plan.desc.size.x();
    params.sizeY = plan.desc.size.y();

    const size_t candidateCount = std::min<size_t>(
        job.candidateLayers.size(), 256);
    params.candidateCount = static_cast<uint32_t>(
        candidateCount == 0 ? 1 : candidateCount);
    if (candidateCount == 0)
    {
        params.candidates[0] = 0;
    }
    else
    {
        for (size_t index = 0; index < candidateCount; ++index)
            params.candidates[index] = job.candidateLayers[index];
    }
}

void TerrainPageBakeGpu::WriteJobDescriptors(
    uint32_t frameIndex,
    uint32_t jobIndex,
    const TerrainPageBakeJob& job,
    vk::Buffer outputIds,
    vk::DeviceSize outputIdsOffset,
    vk::Buffer outputWeights,
    vk::DeviceSize outputWeightsOffset)
{
    const PlacementInput& input = placements[job.registrationIndex];
    const vk::Buffer paintWeights = input.hasPaint
        ? input.paintWeights.buffer : dummyBuffer.buffer;
    const vk::Buffer paintOpacity = input.hasPaint
        ? input.paintOpacityPacked.buffer : dummyBuffer.buffer;

    std::array<vk::DescriptorBufferInfo, 7> infos = {
        vk::DescriptorBufferInfo{
            paramsBuffers[frameIndex].buffer,
            static_cast<vk::DeviceSize>(jobIndex) * paramsStride,
            sizeof(JobParams)},
        vk::DescriptorBufferInfo{
            input.heights.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{
            input.layers.buffer, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{paintWeights, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{paintOpacity, 0, VK_WHOLE_SIZE},
        vk::DescriptorBufferInfo{outputIds, outputIdsOffset, PageBytes},
        vk::DescriptorBufferInfo{outputWeights, outputWeightsOffset, PageBytes}
    };
    std::array<vk::WriteDescriptorSet, 7> writes{};
    for (uint32_t binding = 0; binding < 7; ++binding)
    {
        writes[binding]
            .setDstSet(descriptorSets[frameIndex][jobIndex])
            .setDstBinding(binding)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setBufferInfo(infos[binding]);
    }
    rendererBackend->UpdateDescriptorSets(
        std::vector<vk::WriteDescriptorSet>(writes.begin(), writes.end()));
}

void TerrainPageBakeGpu::DispatchJob(
    vk::CommandBuffer commandBuffer,
    uint32_t frameIndex,
    uint32_t jobIndex,
    const TerrainPageBakeJob&)
{
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        pipeline->GetPipelineLayout(),
        0,
        descriptorSets[frameIndex][jobIndex],
        nullptr);
    const uint32_t groups =
        (TerrainPageManager::PageResolution + 15) / 16;
    commandBuffer.dispatch(groups, groups, 1);
}

void TerrainPageBakeGpu::Record(
    vk::CommandBuffer commandBuffer,
    uint32_t frameIndex,
    const std::vector<TerrainPageBakeJob>& jobs,
    TerrainPageGpuResources& pageResources)
{
    if (!available || jobs.empty())
        return;
    if (frameIndex >= frameSlotCount || jobs.size() > MaxJobsPerFrame)
        throw std::runtime_error(
            "Terrain page bake job count exceeds the frame ring capacity");
    if (!pageResources.layerIds || !pageResources.layerWeights)
        throw std::runtime_error(
            "Terrain page bake requires ID/weight atlas resources");

    const vk::DeviceSize idsSectionSize = MaxJobsPerFrame * PageBytes;
    for (uint32_t jobIndex = 0; jobIndex < jobs.size(); ++jobIndex)
    {
        const TerrainPageBakeJob& job = jobs[jobIndex];
        if (job.registrationIndex >= placements.size() ||
            job.registrationIndex >= pageManager->Plans().size())
            throw std::runtime_error(
                "Terrain page bake job references an unknown placement");
        JobParams params;
        FillParams(params, job, frameIndex);
        std::memcpy(
            static_cast<uint8_t*>(paramsMapped[frameIndex]) +
                static_cast<vk::DeviceSize>(jobIndex) * paramsStride,
            &params,
            sizeof(params));
        WriteJobDescriptors(
            frameIndex,
            jobIndex,
            job,
            outputBuffers[frameIndex].buffer,
            static_cast<vk::DeviceSize>(jobIndex) * PageBytes,
            outputBuffers[frameIndex].buffer,
            idsSectionSize + static_cast<vk::DeviceSize>(jobIndex) * PageBytes);
    }

    // host 写入 params buffer → compute 读取。
    vk::MemoryBarrier hostBarrier;
    hostBarrier
        .setSrcAccessMask(vk::AccessFlagBits::eHostWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eHost,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        hostBarrier,
        nullptr,
        nullptr);

    pipeline->Bind(commandBuffer);
    for (uint32_t jobIndex = 0; jobIndex < jobs.size(); ++jobIndex)
        DispatchJob(commandBuffer, frameIndex, jobIndex, jobs[jobIndex]);

    // compute 写 storage buffer → transfer 读取。
    vk::MemoryBarrier computeBarrier;
    computeBarrier
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        computeBarrier,
        nullptr,
        nullptr);

    RecordAtlasCopies(
        commandBuffer,
        jobs,
        pageResources,
        idsSectionSize,
        frameIndex);
}

void TerrainPageBakeGpu::RecordAtlasCopies(
    vk::CommandBuffer commandBuffer,
    const std::vector<TerrainPageBakeJob>& jobs,
    TerrainPageGpuResources& pageResources,
    vk::DeviceSize idsSectionSize,
    uint32_t frameIndex)
{
    auto buildRegions = [&jobs](vk::DeviceSize base)
    {
        std::vector<vk::BufferImageCopy> regions;
        regions.reserve(jobs.size());
        for (size_t index = 0; index < jobs.size(); ++index)
        {
            const TerrainPageBakeJob& job = jobs[index];
            vk::BufferImageCopy region;
            region
                .setBufferOffset(
                    base + static_cast<vk::DeviceSize>(index) * PageBytes)
                .setBufferRowLength(0)
                .setBufferImageHeight(0)
                .setImageSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(vk::ImageAspectFlagBits::eColor)
                    .setMipLevel(0)
                    .setBaseArrayLayer(0)
                    .setLayerCount(1))
                .setImageOffset(vk::Offset3D{
                    static_cast<int32_t>(
                        job.physicalPageX * TerrainPageManager::PageResolution),
                    static_cast<int32_t>(
                        job.physicalPageY * TerrainPageManager::PageResolution),
                    0})
                .setImageExtent(vk::Extent3D{
                    TerrainPageManager::PageResolution,
                    TerrainPageManager::PageResolution,
                    1});
            regions.push_back(region);
        }
        return regions;
    };

    const auto record = [&commandBuffer](
        Texture& texture,
        vk::Buffer buffer,
        const std::vector<vk::BufferImageCopy>& regions)
    {
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            texture.getImage(),
            1,
            texture.GetFormat(),
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::ImageLayout::eTransferDstOptimal);
        CommonFunction::RecordCopyBufferToImageRegions(
            commandBuffer,
            buffer,
            texture.getImage(),
            regions);
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            texture.getImage(),
            1,
            texture.GetFormat(),
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal);
    };

    record(
        *pageResources.layerIds,
        outputBuffers[frameIndex].buffer,
        buildRegions(0));
    record(
        *pageResources.layerWeights,
        outputBuffers[frameIndex].buffer,
        buildRegions(idsSectionSize));
}

bool TerrainPageBakeGpu::BakePageSync(
    const TerrainPageManager& manager,
    uint32_t registrationIndex,
    uint32_t tileX,
    uint32_t tileZ,
    std::vector<uint8_t>& ids,
    std::vector<uint8_t>& weights,
    std::string& error)
{
    error.clear();
    if (!available || rendererBackend == nullptr)
    {
        error = unavailableReason.empty()
            ? "GPU page bake is unavailable" : unavailableReason;
        return false;
    }
    if (registrationIndex >= manager.Plans().size() ||
        registrationIndex >= manager.Registrations().size())
    {
        error = "GPU page bake parity registration index is out of range";
        return false;
    }
    const TerrainPageManager::Registration& registration =
        manager.Registrations()[registrationIndex];
    if (tileX >= registration.tilesPerSide ||
        tileZ >= registration.tilesPerSide)
    {
        error = "GPU page bake parity tile is outside the placement";
        return false;
    }

    TerrainPageBakeJob job;
    job.registrationIndex = registrationIndex;
    job.tileX = tileX;
    job.tileZ = tileZ;
    const TerrainPageManager::PageKey key{
        registration.placementId, tileX, tileZ};
    const TerrainPageManager::PageState* page = manager.FindPage(key);
    if (page != nullptr)
        job.candidateLayers = page->candidateLayers;
    if (job.candidateLayers.empty())
    {
        job.candidateLayers =
            TerrainWeightMapBuilder::BuildPageCandidates(
                manager.Plans()[registrationIndex],
                registration.tilesPerSide,
                TerrainPageManager::PageResolution,
                tileX,
                tileZ).layerIds;
    }

    rendererBackend->WaitIdle();

    JobParams params;
    FillParams(params, job, 0);
    void* paramsMapped = nullptr;
    BufferResource paramBuffer = CreateHostBuffer(
        paramsStride,
        vk::BufferUsageFlagBits::eStorageBuffer,
        "TerrainPageBakeParityParams",
        &paramsMapped);
    std::memcpy(paramsMapped, &params, sizeof(params));

    BufferResource output = CreateDeviceLocalBuffer(
        2 * PageBytes,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc,
        nullptr,
        "TerrainPageBakeParityOutput");
    void* readbackMapped = nullptr;
    BufferResource readback = CreateHostBuffer(
        2 * PageBytes,
        vk::BufferUsageFlagBits::eTransferDst,
        "TerrainPageBakeParityReadback",
        &readbackMapped);

    std::array<vk::DescriptorPoolSize, 1> poolSizes = {
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 7)
    };
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo.setPoolSizes(poolSizes).setMaxSets(1);
    vk::DescriptorPool pool = rendererBackend->CreateDescriptorPool(
        poolInfo, "TerrainPageBakeParityPool");
    vk::DescriptorSet set{};
    try
    {
        vk::DescriptorSetAllocateInfo allocateInfo;
        allocateInfo
            .setDescriptorPool(pool)
            .setSetLayouts(pipeline->GetDescriptorSetLayouts()[0]);
        std::vector<vk::DescriptorSet> sets(1);
        rendererBackend->AllocateDescriptorSets(allocateInfo, sets);
        set = sets[0];

        const PlacementInput& input = placements[registrationIndex];
        const vk::Buffer paintWeights = input.hasPaint
            ? input.paintWeights.buffer : dummyBuffer.buffer;
        const vk::Buffer paintOpacity = input.hasPaint
            ? input.paintOpacityPacked.buffer : dummyBuffer.buffer;
        std::array<vk::DescriptorBufferInfo, 7> infos = {
            vk::DescriptorBufferInfo{paramBuffer.buffer, 0, sizeof(JobParams)},
            vk::DescriptorBufferInfo{input.heights.buffer, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{input.layers.buffer, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{paintWeights, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{paintOpacity, 0, VK_WHOLE_SIZE},
            vk::DescriptorBufferInfo{output.buffer, 0, PageBytes},
            vk::DescriptorBufferInfo{output.buffer, PageBytes, PageBytes}
        };
        std::array<vk::WriteDescriptorSet, 7> writes{};
        for (uint32_t binding = 0; binding < 7; ++binding)
        {
            writes[binding]
                .setDstSet(set)
                .setDstBinding(binding)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(infos[binding]);
        }
        rendererBackend->UpdateDescriptorSets(
            std::vector<vk::WriteDescriptorSet>(writes.begin(), writes.end()));

        vk::CommandBuffer commandBuffer =
            rendererBackend->BeginSingleTimeCommands();
        try
        {
            pipeline->Bind(commandBuffer);
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute,
                pipeline->GetPipelineLayout(),
                0,
                set,
                nullptr);
            const uint32_t groups =
                (TerrainPageManager::PageResolution + 15) / 16;
            commandBuffer.dispatch(groups, groups, 1);
            vk::MemoryBarrier barrier;
            barrier
                .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eComputeShader,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                barrier,
                nullptr,
                nullptr);
            commandBuffer.copyBuffer(
                output.buffer,
                readback.buffer,
                vk::BufferCopy{0, 0, 2 * PageBytes});
        }
        catch (...)
        {
            rendererBackend->AbortSingleTimeCommands(commandBuffer);
            throw;
        }
        rendererBackend->EndSingleTimeCommands(commandBuffer);

        ids.resize(PageBytes);
        weights.resize(PageBytes);
        std::memcpy(
            ids.data(), readbackMapped, static_cast<size_t>(PageBytes));
        std::memcpy(
            weights.data(),
            static_cast<uint8_t*>(readbackMapped) + PageBytes,
            static_cast<size_t>(PageBytes));
    }
    catch (...)
    {
        rendererBackend->DestroyDescriptorPool(pool);
        rendererBackend->UnmapMemory(readback.memory);
        DestroyBuffer(readback);
        DestroyBuffer(output);
        rendererBackend->UnmapMemory(paramBuffer.memory);
        DestroyBuffer(paramBuffer);
        throw;
    }
    rendererBackend->DestroyDescriptorPool(pool);
    rendererBackend->UnmapMemory(readback.memory);
    DestroyBuffer(readback);
    DestroyBuffer(output);
    rendererBackend->UnmapMemory(paramBuffer.memory);
    DestroyBuffer(paramBuffer);
    return true;
}

} // namespace VL
