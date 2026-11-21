#include "terrainReferenceAtlas.h"
#include "render/diagnostics/terrainCaptureHooks.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "pipeline/pipelineBase.h"
#include "shader/build/contentHash.h"
#include "shaderReflect.h"
#include "texture.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>

namespace VL
{
namespace
{
constexpr uint64_t ChannelBytes=136ull*136*4;
constexpr uint64_t PackedBytes=3*ChannelBytes;
void Require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
vk::ImageCreateInfo ImageInfo(vk::Format format,uint32_t slots)
{
    vk::ImageCreateInfo info;
    info.setImageType(vk::ImageType::e2D).setExtent({136,136,1}).setMipLevels(1).setArrayLayers(slots)
        .setSamples(vk::SampleCountFlagBits::e1).setFormat(format).setTiling(vk::ImageTiling::eOptimal)
        .setUsage(vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive);
    return info;
}
}
struct TerrainReferenceAtlas::Impl
{
    RendererBackendVulkan& backend;
    std::array<vk::Image,3> images{};
    std::array<vk::DeviceMemory,3> memory{};
    std::array<vk::ImageView,3> views{};
    std::array<vk::Sampler,3> unusedSamplers{};
    std::set<uint32_t> validSlots;
    std::optional<TerrainRvtBudgetLease> poolLease,workspaceLease;
    uint64_t allocation=0;
    explicit Impl(RendererBackendVulkan& backend):backend(backend){}
    ~Impl()
    {
        for(uint32_t channel=0;channel<3;++channel)
            backend.DestroyImageResource(images[channel],memory[channel],views[channel],unusedSamplers[channel]);
    }
};
TerrainReferenceAtlas::TerrainReferenceAtlas(RendererBackendVulkan& backend,
    std::shared_ptr<TerrainRvtBudgetOwner> poolOwner,std::shared_ptr<TerrainRvtBudgetOwner> workspaceOwner,
    const std::array<vk::Format,3>& formats,uint32_t slots,const std::vector<TerrainReferencePage>& pages)
{
    Require(poolOwner&&workspaceOwner&&slots>0&&slots<=512&&!pages.empty()&&pages.size()<=512,
        "Reference atlas needs actual admitted owners/slot layout and bounded referenced pages");
    Require(poolOwner->Caps().physicalPoolGpuBytes<=224ull*1024*1024&&
        workspaceOwner->Caps().workspaceGpuBytes<=32ull*1024*1024,"Reference atlas cannot increase P0 pool/workspace caps");
    Require((formats[0]==vk::Format::eR8G8B8A8Srgb||formats[0]==vk::Format::eR8G8B8A8Unorm)&&
        formats[1]==vk::Format::eR8G8B8A8Unorm&&formats[2]==vk::Format::eR8G8B8A8Unorm,
        "Original uncompressed evaluator pages cannot silently substitute compressed atlas formats");
    auto state=std::make_shared<Impl>(backend);
    for(const auto& page:pages)
        Require(page.actualPhysicalSlot<slots&&state->validSlots.insert(page.actualPhysicalSlot).second,
            "Reference atlas slot duplicate/out of actual layout");
    for(uint32_t channel=0;channel<3;++channel)
        state->allocation+=backend.QueryImageMemoryRequirements(ImageInfo(formats[channel],slots)).size;
    state->poolLease=poolOwner->TryReserve({state->allocation,65536,slots,0},
        TerrainRvtCostClass::PhysicalPool,TerrainRvtResourcePhase::Candidate);
    Require(bool(state->poolLease),"Reference overlap rejected by legal pool/outer owner; no cap/fallback");
    const auto stagingBytes=backend.QueryBufferMemoryRequirements(PackedBytes,vk::BufferUsageFlagBits::eTransferSrc).size;
    state->workspaceLease=workspaceOwner->TryReserve({stagingBytes+4096,stagingBytes+PackedBytes+65536,0,0},
        TerrainRvtCostClass::Workspace,TerrainRvtResourcePhase::Candidate);
    Require(bool(state->workspaceLease),"Reference staging rejected by actual Workspace/outer owner");
    struct Upload
    {
        RendererBackendVulkan& backend;
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        void* mapped=nullptr;
        std::optional<TerrainRvtBudgetLease> lease;
        ~Upload(){if(mapped)backend.UnmapMemory(memory);backend.DestroyBuffer(buffer,memory);}
    } upload{backend};
    upload.lease=std::move(state->workspaceLease);
    auto buffer=backend.CreateBuffer(PackedBytes,vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent,"TestOnlyReferenceUpload");
    upload.buffer=buffer.first;upload.memory=buffer.second;upload.mapped=backend.MapMemory(upload.memory,PackedBytes);
    for(uint32_t channel=0;channel<3;++channel)
    {
        auto image=backend.CreateImage(ImageInfo(formats[channel],slots),vk::MemoryPropertyFlagBits::eDeviceLocal,
            "TestOnlyOriginalEvaluatorReference");
        state->images[channel]=image.first;state->memory[channel]=image.second;
        backend.TransitionImageLayout(image.first,1,formats[channel],vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal,0,slots);
        state->views[channel]=backend.CreateImageView(image.first,vk::ImageViewType::e2DArray,formats[channel],
            vk::ImageAspectFlagBits::eColor,0,1,0,slots,"TestOnlyOriginalReferenceView");
    }
    for(const auto& page:pages)
    {
        Require(std::filesystem::file_size(page.originalGpuOutput)==PackedBytes+4&&
            ContentHasher::HashFile(page.originalGpuOutput).ToHex()==page.originalOutputBlake3,
            "Pinned original actual-GPU page bytes missing/changed");
        std::vector<uint8_t> bytes(PackedBytes+4);
        std::ifstream input(page.originalGpuOutput,std::ios::binary);
        input.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
        uint32_t status;std::memcpy(&status,bytes.data()+PackedBytes,4);
        Require(bool(input)&&status==0,"Original evaluator page has invalid status");
        std::memcpy(upload.mapped,bytes.data(),PackedBytes);
        auto command=backend.BeginSingleTimeCommands();
        try
        {
            vk::MemoryBarrier host(vk::AccessFlagBits::eHostWrite,vk::AccessFlagBits::eTransferRead);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eTransfer,{},host,{},{});
            for(uint32_t channel=0;channel<3;++channel)
            {
                vk::BufferImageCopy region;
                region.setBufferOffset(channel*ChannelBytes).setImageSubresource({vk::ImageAspectFlagBits::eColor,0,page.actualPhysicalSlot,1})
                    .setImageExtent({136,136,1});
                command.copyBufferToImage(upload.buffer,state->images[channel],vk::ImageLayout::eTransferDstOptimal,region);
            }
            backend.EndSingleTimeCommands(command);
        }
        catch(...){if(command)backend.AbortSingleTimeCommands(command);throw;}
    }
    for(uint32_t channel=0;channel<3;++channel)
        backend.TransitionImageLayout(state->images[channel],1,formats[channel],vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,0,slots);
    impl=std::move(state);
}
TerrainReferenceAtlas::~TerrainReferenceAtlas()=default;
std::array<vk::DescriptorImageInfo,3> TerrainReferenceAtlas::DescriptorsWithActualSamplers(const std::array<vk::Sampler,3>& samplers)const
{
    std::array<vk::DescriptorImageInfo,3> result;
    for(uint32_t channel=0;channel<3;++channel)
    {Require(bool(samplers[channel]),"Actual RVT sampler missing");result[channel]={samplers[channel],impl->views[channel],vk::ImageLayout::eShaderReadOnlyOptimal};}
    return result;
}
bool TerrainReferenceAtlas::ContainsActualSlot(uint32_t slot)const{return impl->validSlots.count(slot)!=0;}
uint64_t TerrainReferenceAtlas::ActualImageAllocationBytes()const{return impl->allocation;}
std::shared_ptr<const void> TerrainReferenceAtlas::LifetimeOwner()const{return impl;}

struct TerrainReferenceDescriptorOverride::Impl
{
    RendererBackendVulkan& backend;
    std::shared_ptr<const PipelineBase> pipeline;
    std::shared_ptr<RendererObjectResourceEntry> object;
    std::shared_ptr<TerrainReferenceAtlas> atlas;
    std::optional<TerrainRvtBudgetLease> lease;
    vk::DescriptorPool pool;
    vk::DescriptorSet descriptor;
    explicit Impl(RendererBackendVulkan& backend):backend(backend){}
    ~Impl(){if(descriptor)backend.FreeDescriptorSet(pool,descriptor);if(pool)backend.DestroyDescriptorPool(pool);}
};
TerrainReferenceDescriptorOverride::TerrainReferenceDescriptorOverride(RendererBackendVulkan& backend,
    std::shared_ptr<TerrainRvtBudgetOwner> workspace,std::shared_ptr<const PipelineBase> pipeline,
    std::shared_ptr<RendererObjectResourceEntry> object,std::shared_ptr<TerrainReferenceAtlas> atlas,uint32_t imageIndex)
{
    Require(workspace&&pipeline&&object&&atlas,"Reference descriptor requires actual draw/atlas owners");
    auto state=std::make_shared<Impl>(backend);
    state->pipeline=std::move(pipeline);state->object=std::move(object);state->atlas=std::move(atlas);
    state->lease=workspace->TryReserve({4096,65536,0,0},TerrainRvtCostClass::Workspace,TerrainRvtResourcePhase::Candidate);
    Require(bool(state->lease),"Reference descriptor overhead exceeds frozen workspace");
    auto& resources=state->object->GetResources();
    const auto actualSet=resources.descriptorSets.at(imageIndex).at(1);
    std::vector<vk::WriteDescriptorSet> writes;
    std::vector<std::vector<vk::DescriptorImageInfo>> imageInfos;
    std::vector<std::vector<vk::DescriptorBufferInfo>> bufferInfos;
    writes.reserve(128);imageInfos.reserve(128);bufferInfos.reserve(128);
    std::array<vk::Sampler,3> samplers{};
    std::array<uint32_t,3> textureBindings{};
    const std::array<std::string,3> names={"compositedAlbedo","compositedNormal","compositedOrm"};
    std::vector<vk::DescriptorPoolSize> sizes;
    for(const auto& binding:state->pipeline->GetDescriptorLayoutBindings())
    {
        if(binding.set!=1)continue;
        Require(binding.descriptorCount==1,"Reference set1 only accepts current single material descriptors");
        sizes.emplace_back(binding.type,binding.descriptorCount);
        for(uint32_t channel=0;channel<3;++channel)
            if(binding.name==names[channel])textureBindings[channel]=binding.binding;
    }
    Require(textureBindings[0]&&textureBindings[1]&&textureBindings[2],"Actual three RVT material bindings missing");
    for(const auto& actual:resources.writeDescriptorSets.at(imageIndex))
    {
        if(actual.dstSet!=actualSet)continue;
        Require(writes.size()<128&&actual.descriptorCount==1&&!actual.pNext&&!actual.pTexelBufferView,
            "Unsupported/unbounded material descriptor snapshot");
        auto copy=actual;
        if(actual.pImageInfo)
        {
            imageInfos.push_back({*actual.pImageInfo});copy.pImageInfo=imageInfos.back().data();
            for(uint32_t channel=0;channel<3;++channel)
                if(copy.dstBinding==textureBindings[channel])samplers[channel]=actual.pImageInfo->sampler;
        }
        if(actual.pBufferInfo){bufferInfos.push_back({*actual.pBufferInfo});copy.pBufferInfo=bufferInfos.back().data();}
        writes.push_back(copy);
    }
    auto reference=state->atlas->DescriptorsWithActualSamplers(samplers);
    for(auto& write:writes)
        for(uint32_t channel=0;channel<3;++channel)
            if(write.dstBinding==textureBindings[channel])write.pImageInfo=&reference[channel];
    Require(!writes.empty(),"Actual set1 write snapshot missing");
    vk::DescriptorPoolCreateInfo pool;
    pool.setMaxSets(1).setPoolSizes(sizes).setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
    state->pool=backend.CreateDescriptorPool(pool,"TestOnlyReferenceMaterialDescriptors");
    vk::DescriptorSetAllocateInfo allocation;
    allocation.setDescriptorPool(state->pool).setSetLayouts(state->pipeline->GetDescriptorSetLayouts().at(1));
    std::vector<vk::DescriptorSet> sets(1);backend.AllocateDescriptorSets(allocation,sets);state->descriptor=sets[0];
    for(auto& write:writes)write.dstSet=state->descriptor;
    backend.UpdateDescriptorSets(writes);
    impl=std::move(state);
}
TerrainReferenceDescriptorOverride::~TerrainReferenceDescriptorOverride()=default;
void TerrainReferenceDescriptorOverride::BindOnlySet1(vk::CommandBuffer command)const
{
    command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,impl->pipeline->GetPipelineLayout(),1,impl->descriptor,{});
}
std::shared_ptr<const void> TerrainReferenceDescriptorOverride::LifetimeOwner()const{return impl;}

void RecordFrozenTerrainReferenceDraw(vk::CommandBuffer command,const TerrainCaptureDrawInputs& draw,
    const TerrainReferenceReplayStamp& candidate,const TerrainReferenceReplayStamp& reference,
    const TerrainReferenceAtlas& atlas,const TerrainReferenceDescriptorOverride& material)
{
    Require(command&&draw.regularPipeline&&draw.renderableOwner&&draw.objectResourceOwner&&
        draw.materialOwner&&draw.materialInstanceOwner,"Reference replay requires actual frozen draw owners");
    Require(candidate.frame==reference.frame&&candidate.submissionEpoch==reference.submissionEpoch&&
        candidate.worldGeneration==reference.worldGeneration&&candidate.poseId==reference.poseId&&
        candidate.inputClosureSha256==reference.inputClosureSha256&&candidate.inputClosureSha256.size()==64&&
        reference.originalEvaluatorSha256=="d977382c566ad4af542554008ec7d850ebe704831889789eea96b3037af3038b"&&
        reference.referencedPageSetSha256.size()==64&&
        candidate.immutableFrameOwner&&reference.immutableFrameOwner&&reference.referenceTargetOwner,
        "Reference replay must bind the same actual frame/epoch/constants and explicit independent page identity");
    Require(!reference.allRoiSampledPhysicalSlots.empty()&&reference.allRoiSampledPhysicalSlots.size()<=512,
        "Reference ROI actual tap-slot footprint witness missing");
    for(uint32_t slot:reference.allRoiSampledPhysicalSlots)
        Require(atlas.ContainsActualSlot(slot),"Reference page absent for an actual ROI sample tap");
    Require(draw.mesh.indexCount&&draw.mesh.indexBuffer&&draw.mesh.vertexBuffers[0]&&
        draw.mesh.patches==draw.regularPipeline->UsesTessellation(),"Reference replay actual VB/IB/topology mismatch");
    const auto& pipeline=*draw.regularPipeline;
    command.bindPipeline(pipeline.GetBindPoint(),pipeline.GetPipeline());
    for(uint32_t set=0;set<4;++set)
        if(draw.descriptorSetMask&(1u<<set))
            command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,pipeline.GetPipelineLayout(),
                set,draw.descriptorSets[set],{});
    material.BindOnlySet1(command);
    command.bindVertexBuffers(0,2,draw.mesh.vertexBuffers.data(),draw.mesh.vertexOffsets.data());
    command.bindIndexBuffer(draw.mesh.indexBuffer,draw.mesh.indexOffset,draw.mesh.indexType);
    command.drawIndexed(draw.mesh.indexCount,draw.mesh.instanceCount,draw.mesh.firstIndex,
        draw.mesh.vertexOffset,draw.mesh.firstInstance);
}
}
