#include "terrainLitTarget.h"
#include "render/backend/rendererBackendVulkan.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace VL
{
namespace
{
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool Depth(vk::Format format)
{
    return format==vk::Format::eD32Sfloat||format==vk::Format::eD24UnormS8Uint||
        format==vk::Format::eD32SfloatS8Uint;
}
void RectValid(const TerrainLitRect& rect)
{
    Require(rect.width&&rect.height&&rect.width<=128&&rect.height<=128&&
        uint64_t(rect.x)+rect.width<=rect.fullWidth&&uint64_t(rect.y)+rect.height<=rect.fullHeight,
        "Lit capture default bounded domain is <=128x128 within the actual viewport");
}
uint32_t PixelBytes(vk::Format format)
{
    switch(format)
    {
    case vk::Format::eR8G8B8A8Unorm:case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eD32Sfloat:case vk::Format::eR32Uint:case vk::Format::eR32Sfloat:return 4;
    case vk::Format::eR16G16Sfloat:return 4;
    case vk::Format::eR16G16B16A16Sfloat:return 8;
    case vk::Format::eR32G32B32A32Sfloat:case vk::Format::eR32G32B32A32Uint:return 16;
    default:throw std::runtime_error("Lit raw reader has no declared layout for this actual format");
    }
}
}
struct TerrainLitTarget::Impl
{
    RendererBackendVulkan& backend;
    TerrainLitRect rect;
    vk::RenderPass pass;
    std::vector<vk::Framebuffer> framebuffers;
    std::vector<TerrainLitTargetChannel> channels;
    std::vector<vk::DeviceMemory> memory;
    std::optional<TerrainRvtBudgetLease> lease;
    uint64_t gpuBytes=0;
    explicit Impl(RendererBackendVulkan& backend):backend(backend){}
    ~Impl()
    {
        backend.DestroyFramebuffers(framebuffers);
        if(pass)backend.DestroyRenderPass(pass);
        for(size_t index=0;index<channels.size();++index)
        {
            vk::Sampler unused;
            backend.DestroyImageResource(channels[index].image,memory[index],channels[index].view,unused);
        }
    }
};
TerrainLitTarget::TerrainLitTarget(RendererBackendVulkan& backend,
    std::shared_ptr<TerrainRvtBudgetOwner> workspace,const RenderPassCompatibilityKey& key,TerrainLitRect rect,
    bool swapChainOutput)
{
    RectValid(rect);
    Require(workspace&&workspace->Caps().workspaceGpuBytes<=32ull*1024*1024&&
        !key.attachments.empty()&&key.attachments.size()<=20,"Lit target requires actual frozen Workspace owner and bounded pass");
    auto state=std::make_shared<Impl>(backend);state->rect=rect;
    std::vector<vk::ImageCreateInfo> creates;
    std::set<uint32_t> depthIndices;
    if(key.hasDepthAttachment)depthIndices.insert(key.depthAttachment);
    if(key.hasDepthResolveAttachment)depthIndices.insert(key.depthResolveAttachment);
    for(uint32_t index=0;index<key.attachments.size();++index)
    {
        const auto& desc=key.attachments[index];
        Require(Depth(desc.format)==bool(depthIndices.count(index)),"Lit target exact attachment depth identity mismatch");
        vk::ImageCreateInfo create;
        auto usage=Depth(desc.format)?vk::ImageUsageFlagBits::eDepthStencilAttachment:vk::ImageUsageFlagBits::eColorAttachment;
        create.setImageType(vk::ImageType::e2D).setExtent({rect.width,rect.height,1}).setMipLevels(1).setArrayLayers(1)
            .setSamples(desc.sampleCount).setFormat(desc.format).setTiling(vk::ImageTiling::eOptimal)
            .setUsage(usage|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferSrc)
            .setSharingMode(vk::SharingMode::eExclusive);
        state->gpuBytes+=backend.QueryImageMemoryRequirements(create).size;
        creates.push_back(create);
    }
    state->lease=workspace->TryReserve({state->gpuBytes+65536,131072,0,0},
        TerrainRvtCostClass::Workspace,TerrainRvtResourcePhase::Active);
    Require(bool(state->lease),"Lit target rejected by actual 32MiB Workspace/outer owner");
    std::vector<vk::AttachmentDescription2> attachments;
    for(uint32_t index=0;index<creates.size();++index)
    {
        const auto& create=creates[index];
        const auto allocation=backend.CreateImage(create,vk::MemoryPropertyFlagBits::eDeviceLocal,"TestOnly128LitTarget");
        TerrainLitTargetChannel channel;
        channel.image=allocation.first;channel.format=create.format;channel.samples=create.samples;
        channel.aspect=Depth(channel.format)?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor;
        channel.finalLayout=Depth(channel.format)?vk::ImageLayout::eDepthStencilReadOnlyOptimal:vk::ImageLayout::eShaderReadOnlyOptimal;
        state->channels.push_back(channel);state->memory.push_back(allocation.second);
        state->channels.back().view=backend.CreateImageView(channel.image,vk::ImageViewType::e2D,channel.format,
            Depth(channel.format)?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor,
            0,1,0,1,"TestOnly128LitTargetView");
        vk::AttachmentDescription2 attachment;
        attachment.setFormat(channel.format).setSamples(channel.samples).setLoadOp(vk::AttachmentLoadOp::eClear)
            .setStoreOp(vk::AttachmentStoreOp::eStore).setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
            .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare).setInitialLayout(vk::ImageLayout::eUndefined)
            .setFinalLayout(channel.finalLayout);
        attachments.push_back(attachment);
    }
    std::vector<vk::AttachmentReference2> colors,resolves;
    for(auto index:key.colorAttachments)colors.emplace_back(index,vk::ImageLayout::eColorAttachmentOptimal,vk::ImageAspectFlagBits::eColor);
    for(auto index:key.resolveAttachments)resolves.emplace_back(index,vk::ImageLayout::eColorAttachmentOptimal,vk::ImageAspectFlagBits::eColor);
    vk::AttachmentReference2 depth,depthResolve;
    vk::SubpassDescriptionDepthStencilResolve depthInfo;
    if(key.hasDepthAttachment)depth={key.depthAttachment,vk::ImageLayout::eDepthStencilAttachmentOptimal,vk::ImageAspectFlagBits::eDepth};
    if(key.hasDepthResolveAttachment)
    {
        depthResolve={key.depthResolveAttachment,vk::ImageLayout::eDepthStencilAttachmentOptimal,vk::ImageAspectFlagBits::eDepth};
        depthInfo.setDepthResolveMode(vk::ResolveModeFlagBits::eSampleZero)
            .setStencilResolveMode(vk::ResolveModeFlagBits::eNone).setPDepthStencilResolveAttachment(&depthResolve);
    }
    vk::SubpassDescription2 subpass;
    subpass.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics).setColorAttachments(colors);
    if(!resolves.empty())subpass.setResolveAttachments(resolves);
    if(key.hasDepthAttachment)subpass.setPDepthStencilAttachment(&depth);
    if(key.hasDepthResolveAttachment)subpass.setPNext(&depthInfo);
    // 兼容性不只比较附件，也要求 subpass dependency 的个数/subpass 对与真实 pass 一致
    // （VUID-vkCmdDrawIndexed-renderPass-02684）。真实 RenderGraph pass 恒定有一个
    // 0->0 颜色/深度写依赖，只有输出 swapChain 的 pass（toneMapping）额外有一条
    // EXTERNAL->0 的 viewport 依赖，这里逐字镜像，不能用固定两条依赖。
    std::vector<vk::SubpassDependency2> dependencies(1);
    dependencies[0].setSrcSubpass(0).setDstSubpass(0)
        .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput|vk::PipelineStageFlagBits::eLateFragmentTests)
        .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite|vk::AccessFlagBits::eDepthStencilAttachmentWrite)
        .setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput|vk::PipelineStageFlagBits::eEarlyFragmentTests)
        .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite|vk::AccessFlagBits::eDepthStencilAttachmentWrite)
        .setDependencyFlags(vk::DependencyFlagBits::eByRegion);
    if(swapChainOutput)
    {
        vk::SubpassDependency2 viewportDependency;
        viewportDependency.setSrcSubpass(VK_SUBPASS_EXTERNAL).setDstSubpass(0)
            .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput|vk::PipelineStageFlagBits::eFragmentShader)
            .setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
            .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite|vk::AccessFlagBits::eShaderRead)
            .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentRead|vk::AccessFlagBits::eColorAttachmentWrite)
            .setDependencyFlags(vk::DependencyFlagBits::eByRegion);
        dependencies.push_back(viewportDependency);
    }
    vk::RenderPassCreateInfo2 pass;
    pass.setAttachments(attachments).setSubpasses(subpass).setDependencies(dependencies);
    state->pass=backend.CreateRenderPass(pass,"TestOnlyCompatible128LitPass");
    std::vector<vk::ImageView> views;for(const auto& channel:state->channels)views.push_back(channel.view);
    vk::FramebufferCreateInfo fb;
    fb.setRenderPass(state->pass).setAttachments(views).setWidth(rect.width).setHeight(rect.height).setLayers(1);
    state->framebuffers.push_back(backend.CreateFramebuffer(fb,"TestOnly128LitFramebuffer"));
    impl=std::move(state);
}
TerrainLitTarget::~TerrainLitTarget()=default;
void TerrainLitTarget::Begin(vk::CommandBuffer command,const std::vector<vk::ClearValue>& clear)
{
    Require(clear.size()==impl->channels.size(),"Use actual native pass clear values for each attachment");
    vk::RenderPassBeginInfo begin;
    begin.setRenderPass(impl->pass).setFramebuffer(impl->framebuffers.at(0))
        .setRenderArea({{0,0},{impl->rect.width,impl->rect.height}}).setClearValues(clear);
    command.beginRenderPass(begin,vk::SubpassContents::eInline);
    command.setViewport(0,vk::Viewport(-float(impl->rect.x),-float(impl->rect.y),
        float(impl->rect.fullWidth),float(impl->rect.fullHeight),0,1));
    command.setScissor(0,vk::Rect2D({0,0},{impl->rect.width,impl->rect.height}));
}
void TerrainLitTarget::End(vk::CommandBuffer command){command.endRenderPass();}
vk::RenderPass TerrainLitTarget::RenderPass()const{return impl->pass;}
const TerrainLitRect& TerrainLitTarget::Rect()const{return impl->rect;}
const TerrainLitTargetChannel& TerrainLitTarget::Channel(uint32_t index)const{return impl->channels.at(index);}
uint64_t TerrainLitTarget::ActualGpuBytes()const{return impl->gpuBytes;}
std::shared_ptr<const void> TerrainLitTarget::Owner()const{return impl;}

struct TerrainLitReadback::Impl
{
    RendererBackendVulkan& backend;
    TerrainLitRect rect;
    vk::Format format;
    uint32_t bytesPerPixel=0;
    uint64_t payload=0,epoch=0;
    vk::Buffer buffer;
    vk::DeviceMemory memory;
    void* mapped=nullptr;
    std::optional<TerrainRvtBudgetLease> lease;
    std::shared_ptr<const void> sourceOwner;
    std::weak_ptr<const TerrainLitReadbackView> outstanding;
    explicit Impl(RendererBackendVulkan& backend):backend(backend){}
    ~Impl(){if(mapped)backend.UnmapMemory(memory);backend.DestroyBuffer(buffer,memory);}
};
TerrainLitReadback::TerrainLitReadback(RendererBackendVulkan& backend,
    std::shared_ptr<TerrainRvtBudgetOwner> workspace,TerrainLitRect rect,vk::Format format)
{
    RectValid(rect);
    Require(workspace&&workspace->Caps().workspaceGpuBytes<=32ull*1024*1024,"Lit reader needs actual frozen owner");
    auto state=std::make_shared<Impl>(backend);state->rect=rect;state->format=format;
    state->bytesPerPixel=PixelBytes(format);state->payload=uint64_t(rect.width)*rect.height*state->bytesPerPixel;
    const auto actual=backend.QueryBufferMemoryRequirements(state->payload,vk::BufferUsageFlagBits::eTransferDst).size;
    state->lease=workspace->TryReserve({actual+4096,actual+65536,0,0},TerrainRvtCostClass::Workspace,TerrainRvtResourcePhase::Active);
    Require(bool(state->lease),"Lit raw reader rejected by actual Workspace/outer owner");
    auto allocation=backend.CreateBuffer(state->payload,vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent,"TestOnly128LitReadback");
    state->buffer=allocation.first;state->memory=allocation.second;state->mapped=backend.MapMemory(state->memory,state->payload);
    impl=std::move(state);
}
TerrainLitReadback::~TerrainLitReadback()=default;
void TerrainLitReadback::Record(vk::CommandBuffer command,const TerrainLitTargetChannel& source,
    std::shared_ptr<const void> sourceOwner,uint64_t epoch)
{
    auto& state=*impl;
    Require(!state.epoch&&state.outstanding.expired()&&sourceOwner&&epoch&&source.image&&
        source.format==state.format&&source.samples==vk::SampleCountFlagBits::e1,
        "Lit raw reader needs one actual resolved owned image/epoch");
    vk::ImageMemoryBarrier barrier;
    barrier.setImage(source.image).setOldLayout(source.finalLayout).setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSubresourceRange({source.aspect,0,1,0,1}).setSrcAccessMask(vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eColorAttachmentWrite|
            vk::AccessFlagBits::eDepthStencilAttachmentWrite).setDstAccessMask(vk::AccessFlagBits::eTransferRead);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics,vk::PipelineStageFlagBits::eTransfer,{},{},{},barrier);
    vk::BufferImageCopy copy;
    copy.setImageSubresource({source.aspect,0,0,1}).setImageExtent({state.rect.width,state.rect.height,1});
    command.copyImageToBuffer(source.image,vk::ImageLayout::eTransferSrcOptimal,state.buffer,copy);
    barrier.setOldLayout(vk::ImageLayout::eTransferSrcOptimal).setNewLayout(source.finalLayout)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferRead).setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eAllGraphics,{},{},{},barrier);
    vk::MemoryBarrier host(vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eHostRead);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},host,{},{});
    state.epoch=epoch;state.sourceOwner=std::move(sourceOwner);
}
std::shared_ptr<const TerrainLitReadbackView> TerrainLitReadback::TryConsume(uint64_t completed)
{
    auto& state=*impl;if(!state.epoch||completed<state.epoch||!state.outstanding.expired())return {};
    auto result=std::make_shared<TerrainLitReadbackView>();
    result->data=state.mapped;result->payloadBytes=state.payload;result->submissionEpoch=state.epoch;result->completedEpoch=completed;
    result->width=state.rect.width;result->height=state.rect.height;result->bytesPerTexel=state.bytesPerPixel;
    result->format=state.format;result->owner=impl;state.outstanding=result;return result;
}
std::shared_ptr<const void> TerrainLitReadback::Owner()const{return impl;}
}
