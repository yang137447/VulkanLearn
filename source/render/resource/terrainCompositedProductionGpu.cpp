#include "render/resource/terrainCompositedProductionGpu.h"

#include "render/backend/rendererBackendVulkan.h"
#include "resource/image/hostImageVulkanPlan.h"
#include "resource/device/deviceTextureFactory.h"
#include "texture.h"
#include "terrain/terrainCompositedVTStore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace VL
{
namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(std::string("Terrain production GPU: ") + message);
}

TerrainRvtBudgetLease Reserve(
    TerrainRvtBudgetOwner& budget, uint64_t gpu, uint64_t cpu, TerrainRvtCostClass costClass)
{
    auto lease = budget.TryReserve({gpu, cpu, 0, 0}, costClass, TerrainRvtResourcePhase::Candidate);
    Require(lease.has_value(), "frozen World budget exceeded");
    return std::move(*lease);
}

vk::Format Format(const HostImage& image)
{
    return HostImageVulkan::ToVulkanFormat(image.format);
}

struct TransferBuffer
{
    RendererBackendVulkan& backend;
    vk::Buffer buffer{};
    vk::DeviceMemory memory{};
    void* mapped = nullptr;
    ~TransferBuffer()
    {
        if (mapped)
            backend.UnmapMemory(memory);
        if (buffer)
            backend.DestroyBuffer(buffer, memory);
    }
    void Allocate(uint64_t bytes, vk::BufferUsageFlags usage)
    {
        const auto allocation = backend.CreateBuffer(bytes, usage,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TerrainProductionTransfer");
        buffer = allocation.first;
        memory = allocation.second;
        mapped = backend.MapMemory(memory, bytes);
    }
};

void Transition(vk::CommandBuffer command, vk::Image image, uint32_t mips, uint32_t layers,
    vk::ImageLayout before, vk::ImageLayout after,
    vk::AccessFlags source, vk::AccessFlags destination,
    vk::PipelineStageFlags sourceStage, vk::PipelineStageFlags destinationStage)
{
    vk::ImageMemoryBarrier barrier;
    barrier.setImage(image).setOldLayout(before).setNewLayout(after)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(source).setDstAccessMask(destination)
        .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, mips, 0, layers});
    command.pipelineBarrier(sourceStage, destinationStage, {}, {}, {}, barrier);
}

constexpr auto ShaderStages = vk::PipelineStageFlagBits::eAllGraphics |
    vk::PipelineStageFlagBits::eComputeShader;

struct PendingImage
{
    RendererBackendVulkan& backend;
    vk::Image image{};
    vk::DeviceMemory memory{};
    vk::ImageView view{};
    vk::Sampler sampler{};
    ~PendingImage()
    {
        backend.DestroyImageResource(image, memory, view, sampler);
    }
};

std::shared_ptr<Texture> Upload(RendererBackendVulkan& backend, const HostImage& image,
    const TerrainCompositedProductionImageDesc& desc, bool array)
{
    image.ValidateStorage();
    DeviceTextureCreateOptions options;
    options.generateMipmapsOnDevice = false;
    options.semantic = HostImage::TextureSemantic::Data;
    const auto createInfo = DeviceTextureFactory::ImageCreateInfo(image, options);
    TransferBuffer staging{backend};
    staging.Allocate(std::min<uint64_t>(image.data.size(),
        TerrainCompositedProductionGpu::MaxTransferBytes), vk::BufferUsageFlagBits::eTransferSrc);
    PendingImage pending{backend};
    const auto allocation = backend.CreateImage(createInfo, vk::MemoryPropertyFlagBits::eDeviceLocal,
        "TerrainProductionInput");
    pending.image = allocation.first;
    pending.memory = allocation.second;
    TerrainCompositedProductionGpu::VisitTransferChunks(desc, [&](const auto& chunk)
    {
        std::memcpy(staging.mapped, image.data.data() + chunk.rawOffset, static_cast<size_t>(chunk.bytes));
        auto command = backend.BeginSingleTimeCommands();
        try
        {
            if (chunk.rawOffset == 0)
                Transition(command, pending.image, image.mipLevels, image.arrayLayers,
                    vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
                    vk::AccessFlagBits::eTransferWrite, vk::PipelineStageFlagBits::eTopOfPipe,
                    vk::PipelineStageFlagBits::eTransfer);
            vk::MemoryBarrier host;
            host.setSrcAccessMask(vk::AccessFlagBits::eHostWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,
                vk::PipelineStageFlagBits::eTransfer, {}, host, {}, {});
            command.copyBufferToImage(staging.buffer, pending.image,
                vk::ImageLayout::eTransferDstOptimal, chunk.copy);
            if (chunk.rawOffset + chunk.bytes == image.data.size())
                Transition(command, pending.image, image.mipLevels, image.arrayLayers,
                    vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                    vk::PipelineStageFlagBits::eTransfer, ShaderStages);
            backend.EndSingleTimeCommands(command);
        }
        catch (...)
        {
            if (command)
                backend.AbortSingleTimeCommands(command);
            throw;
        }
    });
    pending.view = backend.CreateImageView(pending.image, array ? vk::ImageViewType::e2DArray :
        vk::ImageViewType::e2D, createInfo.format, vk::ImageAspectFlagBits::eColor, 0,
        image.mipLevels, 0, image.arrayLayers, "TerrainProductionInputView");
    pending.sampler = backend.Create2DSampler(desc.filter == TerrainCompositedProductionFilter::Linear
        ? vk::Filter::eLinear : vk::Filter::eNearest,
        desc.address == TerrainCompositedProductionAddress::Repeat ? vk::SamplerAddressMode::eRepeat :
            vk::SamplerAddressMode::eClampToEdge, image.mipLevels > 1, "TerrainProductionInputSampler");
    auto result = std::make_shared<Texture>(backend, pending.image, pending.memory, pending.view,
        pending.sampler, image.mipLevels, createInfo.format, image.arrayLayers);
    pending.image = nullptr;
    pending.memory = nullptr;
    pending.view = nullptr;
    pending.sampler = nullptr;
    return result;
}

float Narrow(double value)
{
    Require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
        "parameter cannot be represented by Baker");
    return static_cast<float>(value);
}
}

TerrainCompositedProductionAllocationQuery TerrainCompositedProductionGpu::MakeAllocationQuery(
    RendererBackendVulkan& backend, TerrainRvtBudgetOwner& budget,
    const TerrainCompositedProductionAllocationOverheads& overheads)
{
    Require(!(overheads.imageViewAndSampler == TerrainRvtCost{}) &&
        overheads.imageMetadata.cpuBytes > 0 && overheads.transferMetadata.cpuBytes > 0 &&
        overheads.queryMetadata.cpuBytes > 0, "explicit frozen allocation overheads are required");
    const auto imageOverhead = AddAllocationCosts(overheads.imageViewAndSampler, overheads.imageMetadata);
    AddAllocationCosts({}, overheads.transferMetadata);
    AddAllocationCosts({}, overheads.queryMetadata);
    TerrainCompositedProductionAllocationQuery result;
    result.image = [&backend, &budget, imageOverhead, overheads](
        const TerrainCompositedProductionImageDesc& desc, bool array)
    {
        Require(array || desc.layers == 1, "2D image query has multiple layers");
        auto queryLease = Reserve(budget, overheads.queryMetadata.gpuBytes,
            overheads.queryMetadata.cpuBytes, TerrainRvtCostClass::Workspace);
        const auto layout = ImageLayout(desc);
        DeviceTextureCreateOptions options;
        options.generateMipmapsOnDevice = false;
        options.semantic = HostImage::TextureSemantic::Data;
        const auto requirements = backend.QueryImageMemoryRequirements(
            DeviceTextureFactory::ImageCreateInfo(layout, options));
        // requirements.size 已包含层/mip、格式、tiling 与驱动对齐，不再由 payload 估算。
        return AddAllocationCosts({requirements.size, 0, 0, 0}, imageOverhead);
    };
    result.transfer = [&backend, &budget, overheads](uint64_t bytes)
    {
        auto queryLease = Reserve(budget, overheads.queryMetadata.gpuBytes,
            overheads.queryMetadata.cpuBytes, TerrainRvtCostClass::Workspace);
        const auto upload = backend.QueryBufferMemoryRequirements(bytes, vk::BufferUsageFlagBits::eTransferSrc);
        const auto readback = backend.QueryBufferMemoryRequirements(bytes, vk::BufferUsageFlagBits::eTransferDst);
        const auto allocation = std::max(upload.size, readback.size);
        // staging 同时计入 Vulkan allocation 与映射的 CPU 成本；CPU payload 由 TransferCost 添加。
        return AddAllocationCosts({allocation, allocation, 0, 0}, overheads.transferMetadata);
    };
    return result;
}

TerrainCompositedProductionObject TerrainCompositedProductionGpu::CaptureGpuImage(
    RendererBackendVulkan& backend, TerrainCompositedProductionObject object,
    std::shared_ptr<Texture> texture, TerrainRvtBudgetOwner& budget,
    const TerrainProductionObjectWriter& writer,
    const TerrainCompositedProductionAllocationQuery& allocations)
{
    Require(bool(texture) && bool(writer), "missing GPU snapshot/writer");
    auto image = ImageLayout(object.image);
    Require(object.kind == TerrainCompositedProductionObjectKind::Image &&
        texture->GetFormat() == Format(image) &&
        texture->GetArrayLayers() == image.arrayLayers && texture->getMipLevels() == image.mipLevels &&
        backend.GetImageExtent(texture->getImage()) == vk::Extent2D{image.width, image.height},
        "GPU snapshot metadata mismatch");
    const auto bytes = object.image.ByteSize();
    const auto cost = TransferCost(bytes, allocations);
    auto workspace = Reserve(budget, cost.gpuBytes, cost.cpuBytes, TerrainRvtCostClass::Workspace);
    image.data.resize(static_cast<size_t>(bytes));
    TransferBuffer readback{backend};
    readback.Allocate(std::min(bytes, MaxTransferBytes), vk::BufferUsageFlagBits::eTransferDst);
    VisitTransferChunks(object.image, [&](const auto& chunk)
    {
        auto command = backend.BeginSingleTimeCommands();
        try
        {
            Transition(command, texture->getImage(), image.mipLevels, image.arrayLayers,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
                vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eTransferRead,
                ShaderStages, vk::PipelineStageFlagBits::eTransfer);
            vk::MemoryBarrier host;
            host.setSrcAccessMask(vk::AccessFlagBits::eHostRead)
                .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,
                vk::PipelineStageFlagBits::eTransfer, {}, host, {}, {});
            command.copyImageToBuffer(texture->getImage(), vk::ImageLayout::eTransferSrcOptimal,
                readback.buffer, chunk.copy);
            Transition(command, texture->getImage(), image.mipLevels, image.arrayLayers,
                vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
                vk::PipelineStageFlagBits::eTransfer, ShaderStages);
            host.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(vk::AccessFlagBits::eHostRead);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eHost, {}, host, {}, {});
            backend.EndSingleTimeCommands(command);
        }
        catch (...)
        {
            if (command)
                backend.AbortSingleTimeCommands(command);
            throw;
        }
        std::memcpy(image.data.data() + chunk.rawOffset, readback.mapped, static_cast<size_t>(chunk.bytes));
    });
    PopulateCapturedImageMetadata(object, image.data);
    writer(object, image.data);
    return object;
}

std::shared_ptr<TerrainCompositedProductionGpuResources> TerrainCompositedProductionGpu::Restore(
    RendererBackendVulkan& backend, const TerrainCompositedVTStore& store,
    TerrainRvtBudgetOwner& budget, const TerrainCompositedProductionAllocationQuery& allocations,
    std::shared_ptr<const TerrainCompositedProductionGpuResources> prior,
    TerrainCompositedProductionGpuResources::WorldScope worldScope)
{
    return Restore(backend, store.Production(),
        [&](const auto& object) { return store.ReadInput(object.objectId); }, budget, allocations,
        std::move(prior), std::move(worldScope));
}

std::shared_ptr<TerrainCompositedProductionGpuResources> TerrainCompositedProductionGpu::Restore(
    RendererBackendVulkan& backend, const TerrainCompositedProductionClosure& closure,
    const TerrainProductionObjectReader& reader, TerrainRvtBudgetOwner& budget,
    const TerrainCompositedProductionAllocationQuery& allocations,
    std::shared_ptr<const TerrainCompositedProductionGpuResources> prior,
    TerrainCompositedProductionGpuResources::WorldScope worldScope)
{
    return TerrainCompositedProductionGpuResources::RestoreImages(
        closure, reader, budget, allocations,
        [&](const HostImage& image, const auto& desc, bool array)
        {
            auto texture = Upload(backend, image, desc, array);
            const auto descriptor = texture->GetDescriptorInfo();
            return TerrainCompositedProductionGpuResources::UploadedImage{
                texture, texture->getImage(), descriptor.imageView, descriptor.sampler};
        }, std::move(prior), std::move(worldScope));
}

TerrainCompositedOwnedBakeInput TerrainCompositedProductionGpu::MakeBakeInput(
    std::shared_ptr<const TerrainCompositedProductionGpuResources> owner, const std::string& surfaceId)
{
    Require(bool(owner), "missing restored resource owner");
    const auto& closure = owner->Closure();
    const auto found = std::find_if(closure.surfaces.begin(), closure.surfaces.end(),
        [&](const auto& surface) { return surface.surfaceId == surfaceId; });
    Require(found != closure.surfaces.end(), "unknown surface");
    const auto& surface = *found;
    TerrainCompositedOwnedBakeInput result;
    result.owner = std::move(owner);
    auto& input = result.input;
    const auto sample = [&](const std::string& id)
    {
        TerrainCompositedBakeGpu::SampledTexture sampled;
        if (id.empty()) return sampled;
        const auto texture = result.owner->Image(id);
        const auto& desc = closure.Object(id).image;
        sampled.descriptor = texture->GetDescriptorInfo();
        sampled.width = desc.width;
        sampled.height = desc.height;
        sampled.layers = desc.layers;
        sampled.mipLevels = desc.mipLevels;
        sampled.encoding = desc.transfer == TerrainCompositedProductionTransfer::Srgb
            ? TerrainCompositedBakeGpu::Encoding::SrgbView : TerrainCompositedBakeGpu::Encoding::Linear;
        return sampled;
    };
    const auto& bindings = surface.inputs;
    input.coveragePageTable = sample(bindings.coveragePageTable);
    input.coverageIds = sample(bindings.coverageIds);
    input.coverageWeights = sample(bindings.coverageWeights);
    input.localLayerWeights = sample(bindings.localLayerWeights);
    input.layerCatalog = sample(bindings.layerCatalog);
    input.layerAlbedo = sample(bindings.layerAlbedo);
    input.layerNormal = sample(bindings.layerNormal);
    input.layerOrm = sample(bindings.layerOrm);
    input.layerHeight = sample(bindings.layerHeight);
    input.surfaceLocalNormal = sample(bindings.surfaceLocalNormal);
    input.surfaceLocalTangent = sample(bindings.surfaceLocalTangent);
    input.globalColour = sample(bindings.globalColour);
    input.globalMasks = sample(bindings.globalMasks);
    input.weightSource = surface.weightSource == TerrainCompositedWeightSource::LocalLayerArray
        ? TerrainCompositedBakeGpu::WeightSource::LocalLayerArray
        : TerrainCompositedBakeGpu::WeightSource::CoverageAtlas;
    input.localLayerCount = static_cast<uint32_t>(surface.localToGlobal.size());
    std::copy(surface.localToGlobal.begin(), surface.localToGlobal.end(), input.localToGlobalLayer.begin());
    input.globalToSourceSlice = surface.globalToSourceSlice;
    const auto& sampling = surface.sampling;
    input.coveragePageResolution = sampling.coveragePageResolution;
    const auto pair = [](const std::array<double, 2>& value)
    { return std::array<float, 2>{Narrow(value[0]), Narrow(value[1])}; };
    input.surfaceUvOffset = pair(sampling.surfaceUvOffset);
    input.surfaceUvScale = pair(sampling.surfaceUvScale);
    input.coverageUvOffset = pair(sampling.coverageUvOffset);
    input.coverageUvScale = pair(sampling.coverageUvScale);
    input.frameUvOffset = pair(sampling.frameUvOffset);
    input.frameUvScale = pair(sampling.frameUvScale);
    std::transform(surface.localToWorld.begin(), surface.localToWorld.end(), input.localToWorld.begin(), Narrow);
    input.uvScale = Narrow(surface.parameters.uvScale);
    input.frozenHeightBlendFade = Narrow(surface.parameters.frozenHeightBlendFade);
    input.retainedFragment.heightBlendStrength = Narrow(surface.parameters.heightBlendStrength);
    input.retainFragmentPostprocess = surface.parameters.retainFragmentPostprocess;
    input.contentIdentity = closure.SurfaceContentHash(surfaceId).ToHex();
    return result;
}
}
