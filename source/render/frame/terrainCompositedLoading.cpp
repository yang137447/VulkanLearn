#include "render/frame/terrainCompositedLoading.h"

#include "render/backend/rendererBackendVulkan.h"

#include <array>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace VL
{
struct TerrainCompositedLoading::Entry
{
    RendererBackendVulkan* backend = nullptr;
    TerrainCompositedLoadingTarget target;
    std::shared_ptr<TerrainRvtBudgetLease> reservation;
    vk::RenderPass renderPass;
    std::vector<vk::Framebuffer> framebuffers;

    ~Entry()
    {
        if (backend)
        {
            backend->DestroyFramebuffers(framebuffers);
            if (renderPass)
                backend->DestroyRenderPass(renderPass);
        }
    }

    bool Matches(RendererBackendVulkan& candidateBackend,
        const TerrainCompositedLoadingTarget& candidate) const
    {
        return backend == &candidateBackend &&
            target.ownerGeneration == candidate.ownerGeneration &&
            target.view == candidate.view && target.format == candidate.format &&
            target.extent == candidate.extent && target.samples == candidate.samples &&
            target.layout == candidate.layout && target.owner.get() == candidate.owner.get() &&
            !target.owner.owner_before(candidate.owner) &&
            !candidate.owner.owner_before(target.owner);
    }
};

TerrainCompositedLoading::TerrainCompositedLoading(
    const std::shared_ptr<TerrainRvtBudgetOwner>& budget,
    TerrainRvtCost metadataPerEntry)
{
    if (!budget)
        throw std::invalid_argument("Loading cache requires the active World budget owner");
    if (metadataPerEntry.cpuBytes < sizeof(Entry) + sizeof(vk::Framebuffer) + 256)
        throw std::invalid_argument("Loading CPU metadata reserve does not cover cache entry storage");
    auto lease = budget->TryReserve(ReservationCost(metadataPerEntry),
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
    if (!lease)
        throw std::runtime_error("Loading cache exceeds frozen Workspace/CPU budget");
    reservation = std::make_shared<TerrainRvtBudgetLease>(std::move(*lease));
}

TerrainCompositedLoading::~TerrainCompositedLoading() = default;

std::shared_ptr<void> TerrainCompositedLoading::Record(
    RendererBackendVulkan& backend,
    vk::CommandBuffer command,
    const TerrainCompositedLoadingTarget& target,
    uint64_t submissionEpoch,
    bool terrainWorld,
    bool qualityReady)
{
    if (!Required(terrainWorld, qualityReady))
        return {};
    if (!command || !target.view || !target.owner ||
        target.format == vk::Format::eUndefined ||
        target.extent.width == 0 || target.extent.height == 0 ||
        submissionEpoch == 0 ||
        (target.layout != vk::ImageLayout::eColorAttachmentOptimal &&
         target.layout != vk::ImageLayout::ePresentSrcKHR))
    {
        throw std::invalid_argument(
            "Terrain loading requires the actual final color target, its lifetime owner, "
            "a submission epoch and its color-attachment/present layout");
    }

    std::shared_ptr<Entry> resources;
    for (const auto& entry : entries)
        if (entry && entry->Matches(backend, target))
        {
            resources = entry;
            break;
        }

    if (resources)
    {
        const vk::ClearValue black{vk::ClearColorValue{std::array<float, 4>{0.f, 0.f, 0.f, 1.f}}};
        vk::RenderPassBeginInfo begin;
        begin.setRenderPass(resources->renderPass).setFramebuffer(resources->framebuffers.front())
            .setRenderArea(vk::Rect2D{{0, 0}, target.extent}).setClearValues(black);
        command.beginRenderPass(begin, vk::SubpassContents::eInline);
        command.endRenderPass();
        return resources;
    }

    auto slot = std::find(entries.begin(), entries.end(), nullptr);
    if (slot == entries.end())
        slot = std::find_if(entries.begin(), entries.end(),
            [](const auto& entry) { return entry.use_count() == 1; });
    if (slot == entries.end())
        throw std::runtime_error("Loading cache full: all eight targets are retained by pending frames");
    slot->reset();
    resources = std::make_shared<Entry>();
    resources->backend = &backend;
    resources->target = target;
    resources->reservation = reservation;
    resources->framebuffers.reserve(1);

    vk::AttachmentDescription2 color;
    color.setFormat(target.format).setSamples(target.samples)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
        .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setInitialLayout(target.layout).setFinalLayout(target.layout);
    vk::AttachmentReference2 reference;
    reference.setAttachment(0).setLayout(vk::ImageLayout::eColorAttachmentOptimal)
        .setAspectMask(vk::ImageAspectFlagBits::eColor);
    vk::SubpassDescription2 subpass;
    subpass.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
        .setColorAttachments(reference);
    std::array<vk::SubpassDependency2, 2> dependencies;
    dependencies[0].setSrcSubpass(VK_SUBPASS_EXTERNAL).setDstSubpass(0)
        .setSrcStageMask(vk::PipelineStageFlagBits::eAllCommands)
        .setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
        .setSrcAccessMask(vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite)
        .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite);
    dependencies[1].setSrcSubpass(0).setDstSubpass(VK_SUBPASS_EXTERNAL)
        .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
        .setDstStageMask(vk::PipelineStageFlagBits::eAllCommands)
        .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
        .setDstAccessMask(vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite);
    vk::RenderPassCreateInfo2 create;
    create.setAttachments(color).setSubpasses(subpass).setDependencies(dependencies);
    resources->renderPass = backend.CreateRenderPass(create, "TerrainCompositedLoading");
    vk::FramebufferCreateInfo framebuffer;
    framebuffer.setRenderPass(resources->renderPass).setAttachments(target.view)
        .setWidth(target.extent.width).setHeight(target.extent.height).setLayers(1);
    resources->framebuffers.push_back(
        backend.CreateFramebuffer(framebuffer, "TerrainCompositedLoading"));

    *slot = resources;
    const vk::ClearValue black{vk::ClearColorValue{std::array<float, 4>{0.f, 0.f, 0.f, 1.f}}};
    vk::RenderPassBeginInfo begin;
    begin.setRenderPass(resources->renderPass).setFramebuffer(resources->framebuffers.front())
        .setRenderArea(vk::Rect2D{{0, 0}, target.extent}).setClearValues(black);
    command.beginRenderPass(begin, vk::SubpassContents::eInline);
    command.endRenderPass();
    return resources;
}
}
