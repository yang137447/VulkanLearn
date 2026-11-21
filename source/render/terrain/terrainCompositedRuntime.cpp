#include "render/terrain/terrainCompositedRuntime.h"

#include "render/backend/rendererBackendVulkan.h"
#include "texture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <iostream>

namespace VL
{
namespace
{
vk::Format Format(TerrainCompositedVTFormat format)
{
    switch (format)
    {
    case TerrainCompositedVTFormat::RGBA8UNorm: return vk::Format::eR8G8B8A8Unorm;
    case TerrainCompositedVTFormat::RGBA8Srgb: return vk::Format::eR8G8B8A8Srgb;
    case TerrainCompositedVTFormat::RG8UNorm: return vk::Format::eR8G8Unorm;
    case TerrainCompositedVTFormat::BC5UNorm: return vk::Format::eBc5UnormBlock;
    case TerrainCompositedVTFormat::BC7UNorm: return vk::Format::eBc7UnormBlock;
    case TerrainCompositedVTFormat::BC7Srgb: return vk::Format::eBc7SrgbBlock;
    default: throw std::invalid_argument("Unsupported composited material channel format");
    }
}
uint64_t Align(uint64_t offset) { return (offset + 15) & ~uint64_t(15); }

std::shared_ptr<Texture> RetainAllocation(std::shared_ptr<Texture> texture,
    TerrainRvtBudgetLease lease)
{
    struct Retained
    {
        TerrainRvtBudgetLease lease;
        std::shared_ptr<Texture> texture;
    };
    auto retained = std::make_shared<Retained>(Retained{std::move(lease), std::move(texture)});
    return std::shared_ptr<Texture>(retained, retained->texture.get());
}

uint64_t ArrayAllocation(RendererBackendVulkan& backend, uint32_t width,
    uint32_t height, uint32_t layers, vk::Format format)
{
    vk::ImageCreateInfo info;
    info.setImageType(vk::ImageType::e2D).setExtent({width, height, 1})
        .setMipLevels(1).setArrayLayers(layers).setSamples(vk::SampleCountFlagBits::e1)
        .setFormat(format).setTiling(vk::ImageTiling::eOptimal)
        .setUsage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive);
    return backend.QueryImageMemoryRequirements(info).size;
}

vk::BufferImageCopy Region(uint64_t offset, uint32_t layer,
    uint32_t width, uint32_t height, uint32_t x = 0, uint32_t y = 0)
{
    vk::BufferImageCopy result;
    result.setBufferOffset(offset)
        .setImageSubresource(vk::ImageSubresourceLayers(
            vk::ImageAspectFlagBits::eColor, 0, layer, 1))
        .setImageOffset(vk::Offset3D(static_cast<int32_t>(x), static_cast<int32_t>(y), 0))
        .setImageExtent(vk::Extent3D(width, height, 1));
    return result;
}

void Transition(RendererBackendVulkan& backend, vk::CommandBuffer command,
    Texture& texture, bool begin)
{
    vk::ImageMemoryBarrier barrier;
    barrier.setImage(texture.getImage())
        .setOldLayout(begin ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eTransferDstOptimal)
        .setNewLayout(begin ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(begin ? vk::AccessFlagBits::eShaderRead : vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(begin ? vk::AccessFlagBits::eTransferWrite : vk::AccessFlagBits::eShaderRead)
        .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, texture.GetArrayLayers()});
    const auto readers = vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader;
    command.pipelineBarrier(begin ? readers : vk::PipelineStageFlagBits::eTransfer,
        begin ? vk::PipelineStageFlagBits::eTransfer : readers, {}, {}, {}, barrier);
}

// 4 texel border 只承诺双线性过滤，不能继承普通纹理 sampler 的设备最大 anisotropy。
std::shared_ptr<Texture> CreateArray(
    RendererBackendVulkan& backend, const Texture::EmptyArrayDesc& desc)
{
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
    vk::Sampler sampler;
    try
    {
        vk::ImageCreateInfo info;
        info.setImageType(vk::ImageType::e2D)
            .setExtent(vk::Extent3D(desc.width, desc.height, 1))
            .setMipLevels(1).setArrayLayers(desc.arrayLayers)
            .setSamples(vk::SampleCountFlagBits::e1).setFormat(desc.format)
            .setTiling(vk::ImageTiling::eOptimal)
            .setUsage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst)
            .setSharingMode(vk::SharingMode::eExclusive);
        const auto created = backend.CreateImage(info,
            vk::MemoryPropertyFlagBits::eDeviceLocal, desc.debugName);
        image = created.first;
        memory = created.second;
        backend.TransitionImageLayout(image, 1, desc.format, vk::ImageLayout::eUndefined,
            vk::ImageLayout::eShaderReadOnlyOptimal, 0, desc.arrayLayers);
        view = backend.CreateImageView(image, vk::ImageViewType::e2DArray, desc.format,
            vk::ImageAspectFlagBits::eColor, 0, 1, 0, desc.arrayLayers, desc.debugName);
        vk::SamplerCreateInfo sampling;
        sampling.setMagFilter(desc.filter).setMinFilter(desc.filter)
            .setMipmapMode(vk::SamplerMipmapMode::eNearest)
            .setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
            .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
            .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
            .setMinLod(0).setMaxLod(0).setAnisotropyEnable(false).setMaxAnisotropy(1);
        sampler = backend.CreateSampler(sampling, desc.debugName);
        return std::make_shared<Texture>(backend, image, memory, view, sampler, 1,
            desc.format, desc.arrayLayers);
    }
    catch (...)
    {
        backend.DestroyImageResource(image, memory, view, sampler);
        throw;
    }
}
}

TerrainCompositedRuntime::TerrainCompositedRuntime(
    RendererBackendVulkan& renderer, TerrainCompositedRuntimeBudget limits)
    : backend(renderer), budget(limits), frames(limits.frameRingSize)
{
    if (!budget.frameRingSize || !budget.uploadsPerFrame || !budget.uploadBytesPerFrame ||
        !budget.surface.slots || budget.surface.slots > 2048 || !budget.owner ||
        !budget.schedulingMicroseconds || budget.freeSlotReserve >= budget.surface.slots ||
        budget.uploadBytesPerFrame > budget.cpuBytes / budget.frameRingSize)
        throw std::invalid_argument("Invalid composited VT frame budget");
    const uint64_t ringBytes = std::min<uint64_t>(budget.uploadBytesPerFrame, 1024 * 1024);
    const uint64_t allocation = backend.QueryBufferMemoryRequirements(
        ringBytes, vk::BufferUsageFlagBits::eTransferSrc).size;
    auto lease = budget.owner->TryReserve(
        {allocation * frames.size(), allocation * frames.size(), 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
    if (!lease)
        throw std::runtime_error("World RVT staging exceeds cross-World admission budget");
    leases.push_back(std::move(*lease));
    try
    {
        for (uint32_t index = 0; index < frames.size(); ++index)
        {
            auto& frame = frames[index];
            const auto created = backend.CreateBuffer(ringBytes, vk::BufferUsageFlagBits::eTransferSrc,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                "WorldRvtUploadRing" + std::to_string(index));
            frame.buffer = created.first;
            frame.memory = created.second;
            frame.bytes = ringBytes;
            frame.mapped = backend.MapMemory(frame.memory, ringBytes);
        }
    }
    catch (...)
    {
        for (auto& frame : frames)
            ReleaseFrame(frame);
        throw;
    }
    gpuBytes = allocation * frames.size();
    sharedPool = std::make_shared<TerrainCompositedResidencyPool>(
        budget.surface.slots, budget.freeSlotReserve);
}

TerrainCompositedRuntime::~TerrainCompositedRuntime()
{
    Abort();
    for (auto& frame : frames)
        ReleaseFrame(frame);
}

void TerrainCompositedRuntime::ReleaseFrame(Frame& frame)
{
    if (frame.mapped)
        backend.UnmapMemory(frame.memory);
    if (frame.buffer)
        backend.DestroyBuffer(frame.buffer, frame.memory);
    frame = {};
}

void TerrainCompositedRuntime::RegisterSurface(
    std::shared_ptr<const TerrainCompositedVTStore> store, uint64_t generation, uint64_t revision)
{
    if (!store)
        throw std::invalid_argument("Missing composited VT store");
    const auto descriptions = store->Surfaces();
    RegisterDescriptions(std::move(store), descriptions, generation, revision);
}

void TerrainCompositedRuntime::RegisterPreviewSurface(
    const TerrainCompositedVTBuildDesc& surface, uint64_t generation, uint64_t revision)
{
    RegisterDescriptions({}, {surface}, generation, revision);
}

void TerrainCompositedRuntime::RegisterSurface(
    std::shared_ptr<const TerrainCompositedVTStore> store,
    const TerrainCompositedVTBuildDesc& surface, uint64_t generation, uint64_t revision)
{
    if (!store)
        throw std::invalid_argument("Missing World RVT publication");
    RegisterDescriptions(std::move(store), {surface}, generation, revision);
}

void TerrainCompositedRuntime::SeedReadOnlyBaseline()
{
    if (recordingFrame || hasSubmitted || surfaces.empty())
        throw std::logic_error("Read-only baseline seeding requires fresh registered surfaces");
    for (auto& surface : surfaces)
        surface.second.residency->SeedReadOnlyBaseline();
    baselineSeeded = true;
}

void TerrainCompositedRuntime::RegisterDescriptions(
    std::shared_ptr<const TerrainCompositedVTStore> store,
    const std::vector<TerrainCompositedVTBuildDesc>& descriptions,
    uint64_t generation, uint64_t revision,
    std::shared_ptr<TerrainRvtBudgetOwner> allocationOwner)
{
    if (recordingFrame || jointSwitchPrepared)
        throw std::invalid_argument("Invalid composited VT registration state");
    uint64_t extraGpuBytes = 0;
    uint64_t extraCpuBytes = 0;
    uint64_t pageTableBytes = 0;
    const bool candidate = bool(allocationOwner);
    auto owner = candidate ? allocationOwner : budget.owner;
    std::map<std::string, std::string> namespaces;
    for (const auto& desc : descriptions)
    {
        ValidateTerrainCompositedProductionOutput(desc);
        if ((!candidate && activeNamespaces.count(desc.surfaceId)) ||
            namespaces.count(desc.surfaceId))
            throw std::invalid_argument("Duplicate VT surface: use a new World resource package");
        auto id = candidate ? desc.surfaceId + "|candidate:" + std::to_string(++nextNamespace) :
            desc.surfaceId;
        if (surfaces.count(id))
            throw std::invalid_argument("Duplicate VT namespace");
        namespaces.emplace(desc.surfaceId, std::move(id));
        if (desc.channels.size() != 3 ||
            (desc.channels[0] != TerrainCompositedVTFormat::RGBA8UNorm &&
             desc.channels[0] != TerrainCompositedVTFormat::RGBA8Srgb &&
             desc.channels[0] != TerrainCompositedVTFormat::BC7UNorm &&
             desc.channels[0] != TerrainCompositedVTFormat::BC7Srgb) ||
            (desc.channels[1] != TerrainCompositedVTFormat::RGBA8UNorm &&
             desc.channels[1] != TerrainCompositedVTFormat::RG8UNorm &&
             desc.channels[1] != TerrainCompositedVTFormat::BC5UNorm) ||
            desc.channels[2] != TerrainCompositedVTFormat::RGBA8UNorm)
            throw std::invalid_argument("Expected albedo/normal/ORM composited VT channel encoding");
        const auto layout = desc.Layout();
        if (desc.width > 8192 || desc.height > 8192)
            throw std::invalid_argument("Composited VT feedback supports virtual dimensions up to 8192");
        if ((!sharedFormats.empty() && sharedFormats != desc.channels) ||
            (!descriptions.empty() && descriptions.front().channels != desc.channels))
            throw std::invalid_argument("World RVT requires one explicit channel format group");
        pageTableBytes += ArrayAllocation(backend, layout.Mip(0).pagesX,
            layout.Mip(0).pagesY, layout.MipCount(), vk::Format::eR32G32B32A32Sfloat);
        extraCpuBytes += budget.surface.cpuBytes;
        if (desc.PageBytes() + 3 * 16 + 16 > budget.uploadBytesPerFrame)
            throw std::invalid_argument("Composited VT page exceeds frame upload budget");
    }
    const bool createPool = sharedFormats.empty();
    if (createPool && !descriptions.empty())
        for (const auto format : descriptions.front().channels)
            extraGpuBytes += ArrayAllocation(backend, 136, 136, budget.surface.slots, Format(format));
    if (surfaces.size() + descriptions.size() + budget.freeSlotReserve > budget.surface.slots)
        throw std::runtime_error("World RVT pool cannot fit roots, free watermark and material pages");
    const uint64_t stagingReservation = budget.uploadBytesPerFrame * frames.size();
    if (!candidate && (extraGpuBytes + pageTableBytes > budget.gpuBytes - gpuBytes ||
        extraCpuBytes > budget.cpuBytes - stagingReservation - reservedCpuBytes))
        throw std::runtime_error("Composited VT baseline/atlas exceeds World memory budget");
    std::optional<TerrainRvtBudgetLease> allocation;
    if (extraGpuBytes)
        allocation = owner->TryReserve(
            {extraGpuBytes, 0, budget.surface.slots, 0},
            TerrainRvtCostClass::PhysicalPool, TerrainRvtResourcePhase::Candidate);
    if (extraGpuBytes && !allocation)
        throw std::runtime_error("World RVT pool/page tables exceed actual allocation admission budget");
    if (createPool && !descriptions.empty())
    {
        Texture::EmptyArrayDesc image;
        image.width = image.height = TerrainCompositedVTLayout::StoredExtent;
        image.arrayLayers = budget.surface.slots;
        image.filter = vk::Filter::eLinear;
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            image.format = Format(descriptions.front().channels[channel]);
            image.debugName = "WorldRvtChannel" + std::to_string(channel);
            sharedChannels[channel] = RetainAllocation(CreateArray(backend, image), *allocation);
        }
        sharedFormats = descriptions.front().channels;
    }
    std::map<std::string, Surface> candidates;
    for (const auto& desc : descriptions)
    {
        Surface surface;
        surface.logicalId = desc.surfaceId;
        surface.tableBytes = ArrayAllocation(backend, desc.Layout().Mip(0).pagesX,
            desc.Layout().Mip(0).pagesY, desc.Layout().MipCount(),
            vk::Format::eR32G32B32A32Sfloat);
        surface.tableLease = owner->TryReserve(
            {surface.tableBytes, budget.surface.cpuBytes, 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        if (!surface.tableLease)
            throw std::runtime_error("World RVT page table exceeds allocation admission budget");
        surface.formats = desc.channels;
        surface.residency = std::make_unique<TerrainCompositedResidency>(
            store, desc, generation, revision, budget.surface, sharedPool,
            namespaces.at(desc.surfaceId));
        auto& binding = surface.binding;
        const auto& layout = surface.residency->Layout();
        binding.u_compositedResolution = {float(desc.width), float(desc.height)};
        binding.u_compositedMaxMip = float(layout.MipCount() - 1);
        binding.u_compositedRepeat =
            desc.addressMode == TerrainCompositedVTAddressMode::Repeat ? 1.f : 0.f;
        binding.u_compositedNormalRG =
            desc.channels[1] == TerrainCompositedVTFormat::RGBA8UNorm ? 0.f : 1.f;
        Texture::EmptyArrayDesc image;
        image.width = layout.Mip(0).pagesX;
        image.height = layout.Mip(0).pagesY;
        image.arrayLayers = layout.MipCount();
        image.format = vk::Format::eR32G32B32A32Sfloat;
        image.filter = vk::Filter::eNearest;
        image.debugName = desc.surfaceId + ":compositedPageTable";
        binding.compositedPageTable = RetainAllocation(
            CreateArray(backend, image), *surface.tableLease);
        std::shared_ptr<Texture>* channels[] = {
            &binding.compositedAlbedo, &binding.compositedNormal, &binding.compositedOrm};
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            surface.channelBytes[channel] = TerrainCompositedVTFormatBytes(desc.channels[channel]);
            *channels[channel] = sharedChannels[channel];
        }
        candidates.emplace(namespaces.at(desc.surfaceId), std::move(surface));
    }
    surfaces.merge(candidates);
    if (candidate)
        candidateNamespaces.merge(namespaces);
    else
        activeNamespaces.merge(namespaces);
    gpuBytes += extraGpuBytes + pageTableBytes;
    reservedCpuBytes += extraCpuBytes;
    if (allocation)
        leases.push_back(std::move(*allocation));
}

void TerrainCompositedRuntime::ConsumeFeedback(
    const std::vector<TerrainCompositedVTTaskToken>& requests)
{
    std::map<std::string, std::vector<TerrainCompositedVTTaskToken>> grouped;
    for (const auto& request : requests)
        if (surfaces.count(request.key.surfaceId) && !surfaces.at(request.key.surfaceId).retired &&
            !CandidateToken(request))
            grouped[request.key.surfaceId].push_back(request);
    std::set<TerrainCompositedVTPageKey> selected;
    for (const auto& group : grouped)
        for (const auto& token : group.second)
            if (token.Matches(Token(token.key)) &&
                surfaces.at(group.first).residency->Layout().Contains(token.key))
                selected.insert(token.key);
    size_t candidatePages = 0;
    for (const auto& item : candidateNamespaces)
        candidatePages += surfaces.at(item.second).requiredPages.size();
    const size_t reserved = budget.freeSlotReserve + surfaces.size() + candidatePages;
    const size_t targetCapacity = reserved < budget.surface.slots ? budget.surface.slots - reserved : 0;
    effectiveMipBias = 0;
    if (selected.size() > targetCapacity)
    {
        std::set<TerrainCompositedVTPageKey> parents;
        for (const auto& key : selected)
        {
            const auto& layout = surfaces.at(key.surfaceId).residency->Layout();
            if (key.mip + 1 < layout.MipCount())
                parents.insert(layout.Parent(key));
        }
        selected = std::move(parents);
        effectiveMipBias = 1;
    }
    const bool insufficient = selected.size() > targetCapacity;
    if (insufficient && !budgetInsufficient)
        std::cerr << "[TerrainVT] budget insufficient at frozen target+1: requested="
            << selected.size() << " available=" << targetCapacity << std::endl;
    budgetInsufficient = insufficient;
    grouped.clear();
    for (const auto& key : selected)
        grouped[key.surfaceId].push_back(Token(key));
    for (auto& surface : surfaces)
        if (!surface.second.retired &&
            activeNamespaces.at(surface.second.logicalId) == surface.first)
            surface.second.residency->ConsumeFeedback(grouped[surface.first]);
}

void TerrainCompositedRuntime::InvalidatePreview(
    const TerrainCompositedVTBuildDesc& desc, uint64_t generation, uint64_t revision,
    const std::vector<TerrainCompositedVTPageKey>& dirtyPages)
{
    if (recordingFrame || jointSwitchPrepared)
        throw std::logic_error("Cannot invalidate a recorded VT frame");
    auto& surface = surfaces.at(activeNamespaces.at(desc.surfaceId));
    if (surface.formats != desc.channels)
        throw std::invalid_argument("Preview encoding change requires a new VT resource package");
    surface.residency->InvalidatePreview(desc, generation, revision, dirtyPages);
}

void TerrainCompositedRuntime::SubmitPreviewPages(
    const TerrainCompositedVTBuildDesc& desc, uint64_t generation, uint64_t revision,
    const std::vector<TerrainCompositedVTPageData>& pages)
{
    if (recordingFrame || jointSwitchPrepared)
        throw std::logic_error("Cannot replace pages in a recorded VT frame");
    auto& surface = surfaces.at(activeNamespaces.at(desc.surfaceId));
    if (surface.formats != desc.channels)
        throw std::invalid_argument("Preview encoding change requires a new VT resource package");
    surface.residency->SubmitPreviewPages(desc, generation, revision, pages);
}

bool TerrainCompositedRuntime::RecordUploads(
    vk::CommandBuffer command, uint32_t frameIndex, uint64_t completedEpoch)
{
    TerrainCompositedFrameUploadBudget allowance;
    allowance.remainingBytes = budget.uploadBytesPerFrame;
    allowance.remainingPages = budget.uploadsPerFrame;
    allowance.remainingSchedulingMicroseconds = budget.schedulingMicroseconds;
    return RecordUploads(command, frameIndex, completedEpoch, allowance);
}

bool TerrainCompositedRuntime::RecordUploads(
    vk::CommandBuffer command, uint32_t frameIndex, uint64_t completedEpoch,
    TerrainCompositedFrameUploadBudget& sharedBudget)
{
    if (recordingFrame || frameIndex >= frames.size() || !command)
        throw std::logic_error("Invalid composited VT recording state");
    CollectRetired(completedEpoch);
    auto& frame = frames[frameIndex];
    if (frame.epoch > completedEpoch)
        throw std::logic_error("Composited VT staging ring fence has not completed");
    const auto started = std::chrono::steady_clock::now();
    recordingFrame = frameIndex;
    uint64_t required = 0;
    uint32_t pageCount = 0;
    const uint64_t byteLimit = std::min({budget.uploadBytesPerFrame,
        sharedBudget.remainingBytes, frame.bytes});
    const uint32_t pageLimit = std::min(budget.uploadsPerFrame, sharedBudget.remainingPages);
    const uint64_t timeLimit =
        std::min(budget.schedulingMicroseconds, sharedBudget.remainingSchedulingMicroseconds);
    try
    {
        std::vector<std::string> order;
        for (const auto& item : surfaces)
            if (!item.second.retired)
                order.push_back(item.first);
        if (!order.empty())
            std::rotate(order.begin(), order.begin() + (scheduleCursor++ % order.size()), order.end());
        for (const auto& id : order)
        {
            if (!timeLimit || !pageLimit || required >= byteLimit || pageCount >= pageLimit)
                break;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (!(baselineSeeded && !hasSubmitted) &&
                !pending.empty() && static_cast<uint64_t>(elapsed) >= timeLimit)
                break;
            auto& surface = surfaces.at(id);
            if (baselineSeeded && lastSubmittedEpoch == 0 && !Ready() &&
                surface.residency->Ready())
                continue;
            const uint64_t payloadBytes = surface.channelBytes[0] +
                surface.channelBytes[1] + surface.channelBytes[2];
            const uint64_t pageCost = Align(payloadBytes) + 3 * 16 + 16;
            const uint32_t allowedPages = static_cast<uint32_t>(std::min<uint64_t>(
                pageLimit - pageCount, (byteLimit - required) / pageCost));
            if (!allowedPages)
                continue;
            auto batch = surface.residency->BeginUploads(
                completedEpoch, uint64_t(allowedPages) * payloadBytes, allowedPages);
            if (baselineSeeded && lastSubmittedEpoch == 0 && !Ready() &&
                batch.uploads.empty() && batch.removals.empty())
            {
                surface.residency->Abort(batch.id);
                continue;
            }
            uint64_t bytes = batch.removals.size() * 16;
            for (const auto& upload : batch.uploads)
                bytes += upload.bytes ? Align(upload.bytes->size()) + 3 * 16 + 16 : 16;
            if (bytes > byteLimit - required ||
                batch.uploads.size() > pageLimit - pageCount)
            {
                surface.residency->Abort(batch.id);
                continue;
            }
            required += bytes;
            pageCount += static_cast<uint32_t>(batch.uploads.size());
            pending.push_back({id, std::move(batch)});
        }
        if (required > frame.bytes)
        {
            ReleaseFrame(frame);
            const auto created = backend.CreateBuffer(required, vk::BufferUsageFlagBits::eTransferSrc,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                "TerrainCompositedStaging" + std::to_string(frameIndex));
            frame.buffer = created.first;
            frame.memory = created.second;
            frame.mapped = backend.MapMemory(frame.memory, required);
            frame.bytes = required;
        }
        uint64_t offset = 0;
        for (const auto& transaction : pending)
        {
            auto& surface = surfaces.at(transaction.surfaceId);
            auto& binding = surface.binding;
            std::array<std::vector<vk::BufferImageCopy>, 3> channelRegions;
            std::vector<vk::BufferImageCopy> tableRegions;
            auto tableEntry = [&](const TerrainCompositedVTPageKey& key, uint32_t value) {
                offset = Align(offset);
                const std::array<float, 4> entry{float(value), 0, 0, 0};
                std::memcpy(static_cast<uint8_t*>(frame.mapped) + offset, entry.data(), sizeof(entry));
                tableRegions.push_back(Region(offset, key.mip, 1, 1, key.x, key.y));
                offset += 16;
            };
            for (const auto& key : transaction.batch.removals)
            {
                auto& removedSurface = surfaces.at(key.surfaceId);
                if (key.surfaceId == transaction.surfaceId)
                    tableEntry(key, 0);
                else
                {
                    offset = Align(offset);
                    const std::array<float, 4> invalid{};
                    std::memcpy(static_cast<uint8_t*>(frame.mapped) + offset,
                        invalid.data(), sizeof(invalid));
                    auto& removedTable = *removedSurface.binding.compositedPageTable;
                    Transition(backend, command, removedTable, true);
                    backend.RecordCopyBufferToImageRegions(command, frame.buffer,
                        removedTable.getImage(), {Region(offset, key.mip, 1, 1, key.x, key.y)});
                    Transition(backend, command, removedTable, false);
                    offset += 16;
                }
            }
            for (const auto& upload : transaction.batch.uploads)
            {
                uint64_t sourceOffset = 0;
                for (uint32_t channel = 0; upload.bytes && channel < 3; ++channel)
                {
                    offset = Align(offset);
                    const auto size = surface.channelBytes[channel];
                    std::memcpy(static_cast<uint8_t*>(frame.mapped) + offset,
                        upload.bytes->data() + sourceOffset, size);
                    channelRegions[channel].push_back(Region(offset, upload.slot, 136, 136));
                    sourceOffset += size;
                    offset += size;
                }
                tableEntry(upload.token.key, upload.slot + 1);
            }
            Texture* channels[] = {binding.compositedAlbedo.get(),
                binding.compositedNormal.get(), binding.compositedOrm.get()};
            for (uint32_t channel = 0; channel < 3; ++channel)
                if (!channelRegions[channel].empty())
                {
                    Transition(backend, command, *channels[channel], true);
                    backend.RecordCopyBufferToImageRegions(command, frame.buffer,
                        channels[channel]->getImage(), channelRegions[channel]);
                    Transition(backend, command, *channels[channel], false);
                }
            auto& table = *binding.compositedPageTable;
            if (!surface.initialized || !tableRegions.empty())
            {
                Transition(backend, command, table, true);
                if (!surface.initialized)
                    backend.RecordClearColorImage(command, table.getImage(),
                        vk::ImageLayout::eTransferDstOptimal,
                        vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 0}),
                        0, table.GetArrayLayers());
                if (!tableRegions.empty())
                {
                    if (!surface.initialized)
                    {
                        vk::MemoryBarrier dependency(vk::AccessFlagBits::eTransferWrite,
                            vk::AccessFlagBits::eTransferWrite);
                        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eTransfer, {}, dependency, {}, {});
                    }
                    backend.RecordCopyBufferToImageRegions(command, frame.buffer,
                        table.getImage(), tableRegions);
                }
                Transition(backend, command, table, false);
            }
        }
    }
    catch (...)
    {
        Abort();
        throw;
    }
    lastRecordMicroseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
    sharedBudget.remainingBytes -= required;
    sharedBudget.remainingPages -= pageCount;
    sharedBudget.remainingSchedulingMicroseconds -=
        std::min(sharedBudget.remainingSchedulingMicroseconds, lastRecordMicroseconds);
    return required != 0;
}

void TerrainCompositedRuntime::Commit(uint64_t submittedEpoch)
{
    const bool loadingCommit = baselineSeeded && submittedEpoch == 0 &&
        lastSubmittedEpoch == 0 && !Ready();
    if (!recordingFrame || (!loadingCommit && submittedEpoch <= lastSubmittedEpoch))
        throw std::logic_error("Invalid composited VT submission epoch");
    if (loadingCommit &&
        std::any_of(pending.begin(), pending.end(), [](const auto& transaction) {
            return transaction.batch.uploads.size() > 1 || !transaction.batch.removals.empty();
        }))
        throw std::logic_error("Loading epoch zero only publishes verified root pages");
    for (const auto& transaction : pending)
    {
        auto& surface = surfaces.at(transaction.surfaceId);
        surface.residency->Commit(transaction.batch.id, submittedEpoch);
        surface.initialized = true;
        if (!transaction.batch.uploads.empty())
            surface.publishEpoch = submittedEpoch;
    }
    for (const auto& item : activeNamespaces)
        surfaces.at(item.second).residency->MarkReferenced(submittedEpoch);
    frames[*recordingFrame].epoch = submittedEpoch;
    lastSubmittedEpoch = submittedEpoch;
    hasSubmitted = true;
    pending.clear();
    recordingFrame.reset();
}

void TerrainCompositedRuntime::Abort()
{
    for (const auto& transaction : pending)
        surfaces.at(transaction.surfaceId).residency->Abort(transaction.batch.id);
    pending.clear();
    recordingFrame.reset();
}

TerrainCompositedMaterialBinding TerrainCompositedRuntime::Binding(const std::string& id) const
{
    const auto& surface = surfaces.at(activeNamespaces.at(id));
    auto binding = surface.binding;
    binding.u_compositedEnabled = surface.initialized && surface.residency->Ready() ? 1.f : 0.f;
    return binding;
}

TerrainCompositedVTTaskToken TerrainCompositedRuntime::Token(
    const TerrainCompositedVTPageKey& key) const
{
    auto resolved = key;
    if (!surfaces.count(resolved.surfaceId))
        resolved.surfaceId = activeNamespaces.at(key.surfaceId);
    else if (activeNamespaces.count(key.surfaceId))
        resolved.surfaceId = activeNamespaces.at(key.surfaceId);
    return surfaces.at(resolved.surfaceId).residency->Token(resolved);
}

std::vector<TerrainCompositedVTTaskToken> TerrainCompositedRuntime::PreviewRequests() const
{
    std::vector<TerrainCompositedVTTaskToken> result;
    for (const auto& surface : surfaces)
    {
        if (surface.second.retired)
            continue;
        const auto requests = surface.second.residency->PreviewRequests();
        result.insert(result.end(), requests.begin(), requests.end());
    }
    return result;
}

std::vector<TerrainCompositedVTTaskToken> TerrainCompositedRuntime::ProductionRequests() const
{
    std::vector<TerrainCompositedVTTaskToken> result;
    for (const auto& surface : surfaces)
    {
        if (surface.second.retired)
            continue;
        const auto requests = surface.second.residency->ProductionRequests();
        result.insert(result.end(), requests.begin(), requests.end());
    }
    return result;
}

std::optional<TerrainCompositedPageUpload> TerrainCompositedRuntime::ReserveProduction(
    const TerrainCompositedVTTaskToken& token, uint64_t completedEpoch)
{
    const auto found = surfaces.find(token.key.surfaceId);
    if (found == surfaces.end() || found->second.retired)
        return {};
    return found->second.residency->ReserveProduction(token, completedEpoch);
}

void TerrainCompositedRuntime::RecordProductionCopy(
    vk::CommandBuffer command, vk::Buffer output, const TerrainCompositedPageUpload& page)
{
    const auto found = surfaces.find(page.token.key.surfaceId);
    if (found == surfaces.end() || found->second.retired ||
        !page.token.Matches(found->second.residency->Token(page.token.key)) ||
        !found->second.residency->OwnsProduction(page))
        throw std::logic_error("Stale GPU page cannot be copied into current World pool");
    auto& surface = surfaces.at(page.token.key.surfaceId);
    uint64_t offset = 0;
    for (uint32_t channel = 0; channel < 3; ++channel)
    {
        auto& image = *sharedChannels[channel];
        Transition(backend, command, image, true);
        backend.RecordCopyBufferToImageRegions(command, output, image.getImage(),
            {Region(offset, page.slot, 136, 136)});
        Transition(backend, command, image, false);
        offset += surface.channelBytes[channel];
    }
}

bool TerrainCompositedRuntime::CompleteProduction(
    const TerrainCompositedPageUpload& page, uint64_t epoch, bool success)
{
    const auto found = surfaces.find(page.token.key.surfaceId);
    return found != surfaces.end() &&
        found->second.residency->CompleteProduction(page, epoch, success && !found->second.retired);
}

std::optional<TerrainCompositedPageDiagnostics> TerrainCompositedRuntime::ProductionDiagnostics(
    const TerrainCompositedVTTaskToken& token, uint64_t completedEpoch) const
{
    const auto found = surfaces.find(token.key.surfaceId);
    if (found == surfaces.end())
        return {};
    return found->second.residency->Diagnostics(token, completedEpoch);
}

std::optional<TerrainCompositedCandidateDiagnostics> TerrainCompositedRuntime::CandidateDiagnostics(
    const std::string& logicalId, uint64_t completedEpoch) const
{
    const auto active = activeNamespaces.find(logicalId);
    const auto candidate = candidateNamespaces.find(logicalId);
    if (active == activeNamespaces.end() && candidate == candidateNamespaces.end())
        return {};
    TerrainCompositedCandidateDiagnostics result;
    result.completedEpoch = completedEpoch;
    result.jointSwitchPrepared = jointSwitchPrepared;
    const auto capture = [&](const Surface& surface, bool required) {
        TerrainCompositedSurfaceProgressDiagnostics progress;
        progress.present = true;
        progress.initialized = surface.initialized;
        progress.tablePublishEpoch = surface.publishEpoch;
        progress.retireEpoch = surface.retireEpoch;
        progress.pages = surface.residency->ProgressDiagnostics(
            required ? &surface.requiredPages : nullptr, completedEpoch);
        return progress;
    };
    if (active != activeNamespaces.end())
        result.current = capture(surfaces.at(active->second), false);
    if (candidate != candidateNamespaces.end())
        result.candidate = capture(surfaces.at(candidate->second), true);
    return result;
}

std::optional<TerrainCompositedSlotDiagnostics> TerrainCompositedRuntime::SlotDiagnostics(
    uint32_t slot, uint64_t completedEpoch) const
{
    return sharedPool->SlotDiagnostics(slot, completedEpoch);
}

bool TerrainCompositedRuntime::ArmSlotReuseDiagnostics(uint32_t slot)
{
    return sharedPool->ArmSlotReuseDiagnostics(slot, *budget.owner);
}

void TerrainCompositedRuntime::CancelSlotReuseDiagnostics() noexcept
{
    sharedPool->CancelSlotReuseDiagnostics();
}

std::optional<TerrainCompositedSlotReuseDiagnostics> TerrainCompositedRuntime::SlotReuseDiagnostics(
    uint64_t completedEpoch) const
{
    return sharedPool->SlotReuseDiagnostics(completedEpoch);
}

std::vector<TerrainCompositedSlotOwner> TerrainCompositedRuntime::RecordedOwners() const
{
    if (surfaces.empty())
        return {};
    auto owners = surfaces.begin()->second.residency->CommittedOwners();
    for (const auto& transaction : pending)
    {
        for (const auto& key : transaction.batch.removals)
            for (auto& owner : owners)
                if (owner.valid && owner.token.key == key)
                    owner.valid = false;
        for (const auto& upload : transaction.batch.uploads)
            owners.at(upload.slot) = {upload.slot, true, upload.token};
    }
    for (auto& owner : owners)
    {
        if (!owner.valid)
            continue;
        const auto found = surfaces.find(owner.token.key.surfaceId);
        if (found == surfaces.end() || found->second.retired ||
            activeNamespaces.at(found->second.logicalId) != found->first)
            owner.valid = false;
        else
            owner.token.key.surfaceId = found->second.logicalId;
    }
    return owners;
}

bool TerrainCompositedRuntime::Ready() const
{
    return !activeNamespaces.empty() &&
        std::all_of(activeNamespaces.begin(), activeNamespaces.end(), [&](const auto& item) {
            const auto& surface = surfaces.at(item.second);
            return surface.initialized && surface.residency->Ready();
        });
}

bool TerrainCompositedRuntime::RootsCurrent() const
{
    return !activeNamespaces.empty() &&
        std::all_of(activeNamespaces.begin(), activeNamespaces.end(), [&](const auto& item) {
            const auto& surface = surfaces.at(item.second);
            return surface.initialized && surface.residency->CurrentRoot();
        });
}

TerrainCompositedRuntimeStats TerrainCompositedRuntime::Statistics() const
{
    TerrainCompositedRuntimeStats result;
    result.gpuBytes = gpuBytes;
    result.reservedCpuBytes = reservedCpuBytes;
    result.lastRecordMicroseconds = lastRecordMicroseconds;
    result.budgetInsufficient = budgetInsufficient;
    result.effectiveMipBias = effectiveMipBias;
    for (const auto& frame : frames)
        result.stagingBytes += frame.bytes;
    for (const auto& surface : surfaces)
        if (activeNamespaces.at(surface.second.logicalId) == surface.first)
            result.surfaces.emplace(surface.second.logicalId, surface.second.residency->Statistics());
    return result;
}

void TerrainCompositedRuntime::BeginCandidate(
    const std::vector<TerrainCompositedVTBuildDesc>& descriptions,
    uint64_t generation, uint64_t revision,
    std::shared_ptr<TerrainRvtBudgetOwner> candidateOwner)
{
    if (recordingFrame || !candidateNamespaces.empty() || !candidateOwner || descriptions.empty())
        throw std::logic_error("Invalid RVT candidate preparation state");
    for (const auto& desc : descriptions)
    {
        const auto& current = surfaces.at(activeNamespaces.at(desc.surfaceId));
        const auto token = current.residency->Token(
            {current.residency->Layout().SurfaceId(), 0, 0, 0});
        if (generation != token.worldGeneration || revision <= token.revision)
            throw std::invalid_argument("RVT candidate requires a newer revision in the same World");
    }
    RegisterDescriptions({}, descriptions, generation, revision, candidateOwner);
    candidatePhaseOwner = std::move(candidateOwner);
    candidateCompletedEpoch = 0;
    try
    {
        for (const auto& desc : descriptions)
        {
            auto& candidate = surfaces.at(candidateNamespaces.at(desc.surfaceId));
            const auto& current = surfaces.at(activeNamespaces.at(desc.surfaceId));
            std::vector<TerrainCompositedVTTaskToken> requests;
            for (auto key : current.residency->SelectedPages())
            {
                key.surfaceId = candidate.residency->Layout().SurfaceId();
                if (!candidate.residency->Layout().Contains(key))
                    throw std::invalid_argument("RVT candidate layout cannot preserve selected pages");
                requests.push_back(candidate.residency->Token(key));
            }
            candidate.residency->ConsumeFeedback(requests);
            candidate.requiredPages = candidate.residency->SelectedPages();
        }
    }
    catch (...)
    {
        CancelCandidate();
        throw;
    }
}

bool TerrainCompositedRuntime::CandidateReady(uint64_t completedEpoch) const
{
    const bool ready = !candidateNamespaces.empty() &&
        std::all_of(candidateNamespaces.begin(), candidateNamespaces.end(), [&](const auto& item) {
            const auto& candidate = surfaces.at(item.second);
            return candidate.initialized && candidate.publishEpoch <= completedEpoch &&
                candidate.residency->AllCurrent(candidate.requiredPages, completedEpoch);
        });
    candidateCompletedEpoch = ready ? completedEpoch : 0;
    return ready;
}

bool TerrainCompositedRuntime::PrepareJointSwitch(uint64_t completedEpoch,
    const std::vector<TerrainRvtBudgetPhaseChange>& additionalPhaseChanges)
{
    if (recordingFrame)
        throw std::logic_error("Cannot prepare RVT joint switch during recording");
    CancelPreparedJointSwitch();
    abandonedPhaseBatch.reset();
    if (!CandidateReady(completedEpoch))
        return false;
    auto nextNamespaces = activeNamespaces;
    std::vector<Surface*> retirements;
    retirements.reserve(candidateNamespaces.size());
    std::vector<TerrainRvtBudgetPhaseChange> phaseChanges;
    if (additionalPhaseChanges.size() > TerrainRvtBudgetPhaseBatch::MaxCharges ||
        candidateNamespaces.size() * 2 >
            TerrainRvtBudgetPhaseBatch::MaxCharges - additionalPhaseChanges.size())
        throw std::length_error("RVT joint switch exceeds phase batch limit");
    phaseChanges.reserve(candidateNamespaces.size() * 2 + additionalPhaseChanges.size());
    for (const auto& item : candidateNamespaces)
    {
        auto& previous = surfaces.at(activeNamespaces.at(item.first));
        auto& next = surfaces.at(item.second);
        if (!previous.residency->CanRetire() || !next.residency->CanRetire() ||
            !previous.tableLease || !next.tableLease ||
            next.tableLease->Phase() == TerrainRvtResourcePhase::Retiring)
            throw std::logic_error("RVT candidate phase/retirement preflight failed");
        nextNamespaces.at(item.first) = item.second;
        retirements.push_back(&previous);
        phaseChanges.push_back({*previous.tableLease, TerrainRvtResourcePhase::Retiring});
        phaseChanges.push_back({*next.tableLease, TerrainRvtResourcePhase::Active});
    }
    phaseChanges.insert(phaseChanges.end(), additionalPhaseChanges.begin(), additionalPhaseChanges.end());
    auto phaseBatch = TerrainRvtBudgetPhaseBatch::Prepare(phaseChanges, *candidatePhaseOwner);
    if (!phaseBatch)
        return false;
    preparedNamespaces.swap(nextNamespaces);
    preparedRetirements.swap(retirements);
    preparedPhaseBatch = std::move(phaseBatch);
    jointSwitchPrepared = true;
    return true;
}

bool TerrainCompositedRuntime::CommitCandidate() noexcept
{
    if (recordingFrame || !jointSwitchPrepared || !preparedPhaseBatch ||
        !preparedPhaseBatch->TryCommitNoThrow())
    {
        CancelPreparedJointSwitch();
        return false;
    }
    for (auto* previous : preparedRetirements)
    {
        previous->retired = true;
        previous->retireEpoch = lastSubmittedEpoch;
        previous->residency->RetireAllNoThrow(
            lastSubmittedEpoch, previous->binding.compositedPageTable);
    }
    activeNamespaces.swap(preparedNamespaces);
    candidateNamespaces.clear();
    preparedNamespaces.clear();
    preparedRetirements.clear();
    jointSwitchPrepared = false;
    return true;
}

void TerrainCompositedRuntime::CancelPreparedJointSwitch() noexcept
{
    jointSwitchPrepared = false;
    preparedNamespaces.clear();
    preparedRetirements.clear();
    if (preparedPhaseBatch)
    {
        abandonedPhaseBatch.swap(preparedPhaseBatch);
        preparedPhaseBatch.reset();
    }
}

void TerrainCompositedRuntime::CancelCandidate()
{
    if (recordingFrame)
        throw std::logic_error("Cannot cancel a recorded RVT candidate");
    CancelPreparedJointSwitch();
    for (const auto& item : candidateNamespaces)
    {
        auto& candidate = surfaces.at(item.second);
        candidate.retired = true;
        candidate.retireEpoch = lastSubmittedEpoch;
        candidate.residency->RetireAll(lastSubmittedEpoch, candidate.binding.compositedPageTable);
        candidate.tableLease->SetPhase(TerrainRvtResourcePhase::Retiring);
    }
    candidateNamespaces.clear();
    candidateCompletedEpoch = 0;
}

bool TerrainCompositedRuntime::CandidateToken(const TerrainCompositedVTTaskToken& token) const
{
    const auto found = surfaces.find(token.key.surfaceId);
    if (found == surfaces.end() || found->second.retired)
        return false;
    const auto candidate = candidateNamespaces.find(found->second.logicalId);
    return candidate != candidateNamespaces.end() && candidate->second == found->first &&
        token.Matches(found->second.residency->Token(token.key));
}

std::string TerrainCompositedRuntime::LogicalSurfaceId(const std::string& id) const
{
    return surfaces.at(id).logicalId;
}

TerrainCompositedMaterialBinding TerrainCompositedRuntime::CandidateBinding(const std::string& id) const
{
    const auto& surface = surfaces.at(candidateNamespaces.at(id));
    auto binding = surface.binding;
    binding.u_compositedEnabled = surface.initialized &&
        surface.residency->AllCurrent(surface.requiredPages, candidateCompletedEpoch) ? 1.f : 0.f;
    return binding;
}

std::vector<TerrainCompositedVTPageKey> TerrainCompositedRuntime::SelectedPages() const
{
    std::vector<TerrainCompositedVTPageKey> result;
    for (const auto& item : activeNamespaces)
        for (auto key : surfaces.at(item.second).residency->SelectedPages())
        {
            key.surfaceId = item.first;
            result.push_back(std::move(key));
        }
    return result;
}

void TerrainCompositedRuntime::CollectRetired(uint64_t completedEpoch)
{
    abandonedPhaseBatch.reset();
    if (!jointSwitchPrepared && candidateNamespaces.empty())
        preparedPhaseBatch.reset();
    for (auto& item : surfaces)
        if (item.second.retired && item.second.retireEpoch <= completedEpoch)
        {
            item.second.binding.compositedPageTable.reset();
            item.second.tableLease.reset();
        }
    sharedPool->CollectReusable(completedEpoch);
    for (auto iterator = surfaces.begin(); iterator != surfaces.end();)
        if (iterator->second.retired && iterator->second.retireEpoch <= completedEpoch &&
            iterator->second.residency->Retired())
        {
            gpuBytes -= iterator->second.tableBytes;
            reservedCpuBytes -= budget.surface.cpuBytes;
            iterator = surfaces.erase(iterator);
        }
        else
            ++iterator;
}
}
