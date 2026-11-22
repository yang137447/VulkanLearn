#include "terrainLitReplaySession.h"
#include "render/pass/passRuntime.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/frontend/renderScene.h"
#include "material.h"
#include "materialInstance.h"
#include "renderGraph.h"
#include "shaderCompiler.h"
#include "pipeline/pipelineBase.h"
#include "pipeline/pipelineFactory.h"
#include "renderableObject.h"
#include "shader/build/contentHash.h"
#include "terrainEvidenceWriter.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <nlohmann/json.hpp>

namespace VL
{
namespace
{
using Json=nlohmann::json;
void Require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
std::string Text(const std::filesystem::path& path)
{
    std::ifstream file(path);Require(bool(file),"Test-only expanded lighting source missing");
    return {std::istreambuf_iterator<char>(file),{}};
}
struct DescriptorClone
{
    RendererBackendVulkan& backend;
    vk::DescriptorPool pool;
    vk::DescriptorSet set;
    std::optional<TerrainRvtBudgetLease> lease;
    std::vector<std::vector<vk::DescriptorImageInfo>> images;
    std::vector<std::vector<vk::DescriptorBufferInfo>> buffers;
    explicit DescriptorClone(RendererBackendVulkan& backend):backend(backend){}
    ~DescriptorClone(){if(set)backend.FreeDescriptorSet(pool,set);if(pool)backend.DestroyDescriptorPool(pool);}
};
}
struct TerrainLitReplaySession::Impl
{
    TerrainCaptureFrameInfo frame;
    std::shared_ptr<TerrainReferenceAtlas> reference;
    TerrainLitReplayConfig config;
    ShaderCompiler compiler;
    std::unique_ptr<PipelineFactory> factory;
    std::array<std::unique_ptr<TerrainLitTarget>,2> geometry,lighting,display;
    std::vector<std::shared_ptr<DescriptorClone>> descriptors;
    std::vector<std::shared_ptr<PipelineBase>> pipelines;
    std::vector<std::shared_ptr<TerrainReferenceDescriptorOverride>> referenceDescriptors;
    std::array<std::unique_ptr<TerrainLitReadback>,2> litReadbacks,depthReadbacks;
    std::vector<TerrainCaptureDrawInputs> actualDraws;
    std::vector<RenderDrawPacket> stablePackets;
    const RenderScene* actualScene=nullptr;
    vk::CommandBuffer actualCommand;
    uint32_t actualSwapchainIndex=0;
    uint64_t submitted=0;
    bool geometryRecorded=false,lightingRecorded=false,displayRecorded=false,finished=false;
    std::optional<TerrainRvtBudgetLease> metadata;
    std::optional<TerrainRvtBudgetLease> drawMetadata;
    Json identity;
    std::string failure;
    explicit Impl(TerrainCaptureFrameInfo frame):frame(std::move(frame)){}

    std::shared_ptr<DescriptorClone> ClonePassInputs(const PassRuntimeContext& context,
        const PipelineBase& pipeline,const std::map<uint32_t,TerrainLitTargetChannel>& replace)
    {
        auto clone=std::make_shared<DescriptorClone>(*frame.backend);
        clone->lease=frame.actualWorldBudget->TryReserve({4096,65536,0,0},
            TerrainRvtCostClass::Workspace,TerrainRvtResourcePhase::Active);
        Require(bool(clone->lease),"Lit replay descriptors rejected by actual Workspace owner");
        const auto actualSet=context.renderPass.GetDescriptorSets().at(context.swapChainImageIndex).at(3);
        std::vector<vk::DescriptorPoolSize> sizes;
        for(const auto& binding:pipeline.GetDescriptorLayoutBindings())
            if(binding.set==3)sizes.emplace_back(binding.type,binding.descriptorCount);
        vk::DescriptorPoolCreateInfo pool;
        pool.setMaxSets(1).setPoolSizes(sizes).setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet);
        clone->pool=frame.backend->CreateDescriptorPool(pool,"TestOnlyLitRoiPassDescriptors");
        vk::DescriptorSetAllocateInfo allocation;
        allocation.setDescriptorPool(clone->pool).setSetLayouts(pipeline.GetDescriptorSetLayouts().at(3));
        std::vector<vk::DescriptorSet> sets(1);frame.backend->AllocateDescriptorSets(allocation,sets);clone->set=sets[0];
        clone->images.reserve(64);clone->buffers.reserve(64);
        std::vector<vk::WriteDescriptorSet> writes;writes.reserve(64);
        std::set<uint32_t> patched;
        for(const auto& source:context.renderPass.writeDescriptorSets.at(context.swapChainImageIndex))
        {
            if(source.dstSet!=actualSet)continue;
            Require(writes.size()<64&&source.descriptorCount==1&&!source.pNext&&!source.pTexelBufferView,
                "Unsupported actual lighting descriptor snapshot");
            auto write=source;write.dstSet=clone->set;
            if(source.pImageInfo)
            {
                auto info=*source.pImageInfo;
                if(const auto found=replace.find(write.dstBinding);found!=replace.end())
                {
                    info.imageView=found->second.view;info.imageLayout=found->second.finalLayout;
                    patched.insert(write.dstBinding);
                }
                clone->images.push_back({info});write.pImageInfo=clone->images.back().data();
            }
            if(source.pBufferInfo)
            {clone->buffers.push_back({*source.pBufferInfo});write.pBufferInfo=clone->buffers.back().data();}
            writes.push_back(write);
        }
        // 图只写真实 pass 材质实际声明的 set3 binding；裁剪片元未必采样全部 GBuffer 通道。
        // 只要求“裁剪片元确实采样且属于被替换集合”的 binding 都已替换，其余缺失仅记录，
        // 否则会把未使用通道的缺失误判成证据失败。
        std::set<uint32_t> sampled;
        for(const auto& binding:pipeline.GetShaderBindings())
            if(binding.set==3)sampled.insert(binding.binding);
        std::vector<uint32_t> missing;
        for(const auto& item:replace)
            if(!patched.count(item.first)&&sampled.count(item.first))missing.push_back(item.first);
        Require(!writes.empty()&&missing.empty(),"Actual pass input override binding missing");
        for(const auto& item:replace)
            if(!patched.count(item.first))
                std::cerr<<"[LitReplay] set3 binding "<<item.first
                    <<" is not written by the actual pass and is not sampled by the crop fragment"<<std::endl;
        frame.backend->UpdateDescriptorSets(writes);descriptors.push_back(clone);return clone;
    }

    std::shared_ptr<PipelineBase> CropPipeline(const PassRuntimeContext& context,
        const std::filesystem::path& fragment)
    {
        auto instance=context.renderPass.materialInstance.lock();
        Require(bool(instance),"Actual pass material instance unavailable");
        auto material=instance->GetBaseMaterial().lock();
        Require(bool(material),"Actual pass material unavailable");
        auto key=material->GetShaderVariantKey();
        key.macros.push_back("VL_TEST_ONLY_BOUNDED_LIT_REFERENCE=1");
        auto request=compiler.CreateGraphicsVariantBuildRequest(key);
        for(auto& stage:request.stages)
            if(stage.stageName=="fragment")stage.sourceCode=Text(fragment);
        compiler.FreezeSourceSnapshot(request);
        auto build=compiler.CompileCandidate(request);
        auto artifact=BuildGraphicsShaderVariantArtifact(build,"Test-only bounded existing lighting");
        GraphicsPipelineLayoutDesc layout;
        const auto& regular=*material->GetRenderPipeline();
        for(uint32_t set=0;set<4;++set)layout.overrideSets[set]=true;
        for(const auto& binding:regular.GetDescriptorLayoutBindings())
        {Require(binding.set<4,"Unexpected actual lighting set");layout.setBindings[binding.set].push_back(binding);}
        for(const auto& binding:artifact.shaderBindings)
        {
            Require(binding.set<4,"Crop shader introduced a descriptor set");
            const auto& original=layout.setBindings[binding.set];
            const auto found=std::find_if(original.begin(),original.end(),
                [&](const ShaderBinding& item){return item.binding==binding.binding;});
            Require(found!=original.end()&&HasSameShaderBindingLayout(*found,binding)&&
                (binding.stageFlags&~found->stageFlags)==vk::ShaderStageFlags{},
                "Crop shader changed actual lighting descriptor contract");
        }
        auto renderPass=context.renderPass.renderPass;
        const auto& recipe=material->GetPipelineReloadRecipe().surface;
        PipelineFactory::GraphicsCandidateState candidate;
        auto pipeline=factory->CreateGraphicsPipelineCandidate(candidate,&renderPass,recipe.passPipelineContractKey,
            artifact,recipe.cullMode,recipe.blendMode,layout);
        pipelines.push_back(pipeline);
        identity["lightingArtifacts"].push_back({{"pass",context.renderPass.name},
            {"sourceFingerprint",build.sourceFingerprint},{"artifactGenerationKey",build.artifactGenerationKey},
            {"expandedSourceBlake3",ContentHasher::HashFile(fragment).ToHex()}});
        return pipeline;
    }

    void Fullscreen(PassRuntimeContext& context,TerrainLitTarget& target,
        const PipelineBase& pipeline,const DescriptorClone& clone)
    {
        target.Begin(context.commandBuffer,context.renderPass.clearValues);
        context.commandBuffer.bindPipeline(pipeline.GetBindPoint(),pipeline.GetPipeline());
        const auto& sets=context.renderPass.GetDescriptorSets().at(context.swapChainImageIndex);
        std::set<uint32_t> bound;
        for(const auto& binding:pipeline.GetShaderBindings())
            if(bound.insert(binding.set).second)
                context.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,pipeline.GetPipelineLayout(),
                    binding.set,binding.set==3?clone.set:sets.at(binding.set),{});
        context.commandBuffer.draw(3,1,0,0);
        target.End(context.commandBuffer);
    }
};
TerrainLitReplaySession::TerrainLitReplaySession(const TerrainCaptureFrameInfo& frame,
    std::shared_ptr<TerrainReferenceAtlas> reference,TerrainLitReplayConfig config)
{
    Require(frame.backend&&frame.actualWorldBudget&&frame.immutableFrameOwner&&frame.immutableGraphOwner&&
        frame.submissionEpoch&&reference,"Lit replay requires same native frame and admitted reference atlas");
    Require(config.rect.width==128&&config.rect.height==128&&
        config.inputClosureSha256.size()==64&&config.referencePageSetSha256.size()==64&&
        !config.allActualRoiSampledSlots.empty(),"Lit replay explicit bounded configuration incomplete");
    auto state=std::make_shared<Impl>(frame);state->reference=std::move(reference);state->config=std::move(config);
    state->metadata=frame.actualWorldBudget->TryReserve({65536,524288,0,0},
        TerrainRvtCostClass::Workspace,TerrainRvtResourcePhase::Active);
    Require(bool(state->metadata),"Lit replay metadata rejected by actual Workspace owner");
    state->compiler.Initialize(std::filesystem::path(CommonFunction::GetProjectPath())/"shader");
    state->factory=frame.backend->CreatePipelineFactory();state->factory->SetShaderCompiler(&state->compiler);
    state->identity={{"frame",frame.frame},{"submissionEpoch",frame.submissionEpoch},{"worldGeneration",frame.worldGeneration},
        {"poseId",frame.poseId},{"actualBaseFineResidentPages",frame.actualBaseFineResidentPages},
        {"inputClosureSha256",state->config.inputClosureSha256},
        {"originalEvaluatorSha256",state->config.originalEvaluatorSha256},{"referencePageSetSha256",state->config.referencePageSetSha256},
        {"rect",{state->config.rect.x,state->config.rect.y,128,128}},{"lightingArtifacts",Json::array()},
        {"actualDraws",Json::array()},
        {"scope","Same-epoch bounded deferred geometry/lighting with shared actual postprocess inputs; not full dynamic/postprocess independence"}};
    impl=std::move(state);
}
TerrainLitReplaySession::~TerrainLitReplaySession()=default;
void TerrainLitReplaySession::AfterActualPass(PassRuntimeContext& context)
{
    auto& state=*impl;
    if(!state.failure.empty())throw std::runtime_error(state.failure);
    std::cerr<<"[LitReplay] pass="<<context.renderPass.name<<" type="<<uint32_t(context.renderPass.type)
        <<" geometry="<<state.geometryRecorded<<" lighting="<<state.lightingRecorded
        <<" display="<<state.displayRecorded<<" submitted="<<state.submitted<<std::endl;
    Require(context.renderScene.worldGeneration==state.frame.worldGeneration,"Lit frame World changed");
    if(state.actualScene)
        Require(state.actualScene==&context.renderScene&&state.actualCommand==context.commandBuffer&&
            state.actualSwapchainIndex==context.swapChainImageIndex,
            "Lit pass callback changed native scene/command/swapchain submission");
    if(context.renderPass.type==RenderGraphPassType::Geometry&&!state.geometryRecorded)
    {
        Require(context.renderScene.drawPackets.size()<=4096,"Lit native draw list exceeds bounded metadata");
        state.drawMetadata=state.frame.actualWorldBudget->TryReserve(
            {0,65536+context.renderScene.drawPackets.size()*(sizeof(RenderDrawPacket)+4096),0,0},
            TerrainRvtCostClass::Workspace,TerrainRvtResourcePhase::Active);
        Require(bool(state.drawMetadata),"Lit immutable draw metadata rejected by actual owner");
        state.actualScene=&context.renderScene;
        state.actualCommand=context.commandBuffer;
        state.actualSwapchainIndex=context.swapChainImageIndex;
        state.stablePackets.reserve(context.renderScene.drawPackets.size());
        state.identity["actualSnapshotFrameIndex"]=context.renderScene.frameIndex;
        state.identity["actualSwapchainImageIndex"]=context.swapChainImageIndex;
        state.identity["cameraViewProjectionBlake3"]=ContentHasher::HashBytes(
            context.renderScene.camera.viewProjection.data(),16*sizeof(float)).ToHex();
        state.identity["cameraPosition"]={context.renderScene.camera.position.x(),
            context.renderScene.camera.position.y(),context.renderScene.camera.position.z()};
        for(const auto& group:context.resolvedRenderScene.materialGroups)
        {
            if(!IsGeometryRenderMode(group.material->GetShaderVariantKey().renderMode))continue;
            const auto& pipeline=group.material->GetRenderPipeline();
            for(const auto& instance:group.materialInstances)
                for(const auto& draw:instance.draws)
                {
                    const auto& packet=context.renderScene.drawPackets.at(draw.drawPacketIndex);
                    Require(state.stablePackets.size()<state.stablePackets.capacity()&&
                        packet.mesh.key.size()<=512&&packet.lodGroupId.size()<=512,
                        "Lit actual draw identity exceeds bounded immutable metadata");
                    auto input=BuildTerrainCaptureDrawInputs(packet,uint32_t(draw.drawPacketIndex),
                        context.swapChainImageIndex%RenderableObject::DynamicMeshFrameSlotCount,context.swapChainImageIndex,
                        *pipeline,context.renderPass,draw.renderableObject,draw.objectResourceEntry,group.material,instance.materialInstance);
                    state.stablePackets.emplace_back();
                    auto& stable=state.stablePackets.back();
                    stable.objectId=packet.objectId;stable.mesh=packet.mesh;stable.model=packet.model;
                    stable.isTerrain=packet.isTerrain;stable.lodLevel=packet.lodLevel;stable.lodGroupId=packet.lodGroupId;
                    input.packet=&stable;
                    state.identity["actualDraws"].push_back({{"objectId",packet.objectId},
                        {"mesh",packet.mesh.key},{"lodLevel",packet.lodLevel},{"lodGroupId",packet.lodGroupId},
                        {"indexCount",input.mesh.indexCount},{"firstIndex",input.mesh.firstIndex},
                        {"indexOffset",input.mesh.indexOffset},{"vertexOffset",input.mesh.vertexOffset},
                        {"instanceCount",input.mesh.instanceCount},{"firstInstance",input.mesh.firstInstance},
                        {"topology",input.mesh.patches?"patch-list":"triangle-list"},
                        {"modelBlake3",ContentHasher::HashBytes(packet.model.data(),16*sizeof(float)).ToHex()}});
                    state.actualDraws.push_back(std::move(input));
                }
        }
        Require(!state.actualDraws.empty(),"Lit replay has no actual geometry draws");
        for(uint32_t branch=0;branch<2;++branch)
        {
            state.geometry[branch]=std::make_unique<TerrainLitTarget>(*state.frame.backend,state.frame.actualWorldBudget,
                context.renderPass.pipelineContractKey.renderPassCompatibilityKey,state.config.rect);
            auto& target=*state.geometry[branch];
            target.Begin(context.commandBuffer,context.renderPass.clearValues);
            for(const auto& draw:state.actualDraws)
            {
                const auto& pipeline=*draw.regularPipeline;
                context.commandBuffer.bindPipeline(pipeline.GetBindPoint(),pipeline.GetPipeline());
                for(uint32_t set=0;set<4;++set)
                    if(draw.descriptorSetMask&(1u<<set))context.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                        pipeline.GetPipelineLayout(),set,draw.descriptorSets[set],{});
                if(branch==1&&draw.packet->isTerrain)
                {
                    auto override=std::make_shared<TerrainReferenceDescriptorOverride>(*state.frame.backend,state.frame.actualWorldBudget,
                        draw.materialOwner->GetRenderPipeline(),draw.objectResourceOwner,state.reference,context.swapChainImageIndex);
                    TerrainReferenceReplayStamp stamp;
                    stamp.frame=state.frame.frame;stamp.submissionEpoch=state.frame.submissionEpoch;
                    stamp.worldGeneration=state.frame.worldGeneration;stamp.poseId=state.frame.poseId;
                    stamp.inputClosureSha256=state.config.inputClosureSha256;stamp.originalEvaluatorSha256=state.config.originalEvaluatorSha256;
                    stamp.referencedPageSetSha256=state.config.referencePageSetSha256;stamp.allRoiSampledPhysicalSlots=state.config.allActualRoiSampledSlots;
                    stamp.immutableFrameOwner=state.frame.immutableFrameOwner;stamp.referenceTargetOwner=target.Owner();
                    RecordFrozenTerrainReferenceDraw(context.commandBuffer,draw,stamp,stamp,*state.reference,*override);
                    state.referenceDescriptors.push_back(std::move(override));
                }
                else
                {
                    context.commandBuffer.bindVertexBuffers(0,2,draw.mesh.vertexBuffers.data(),draw.mesh.vertexOffsets.data());
                    context.commandBuffer.bindIndexBuffer(draw.mesh.indexBuffer,draw.mesh.indexOffset,draw.mesh.indexType);
                    context.commandBuffer.drawIndexed(draw.mesh.indexCount,draw.mesh.instanceCount,draw.mesh.firstIndex,
                        draw.mesh.vertexOffset,draw.mesh.firstInstance);
                }
            }
            target.End(context.commandBuffer);
        }
        state.geometryRecorded=true;
    }
    else if(context.renderPass.name=="deferredLighting"&&state.geometryRecorded&&!state.lightingRecorded)
    {
        const auto& geometryPass=context.renderGraph.RequireUniquePass(RenderGraphPassType::Geometry);
        const auto& key=geometryPass.pipelineContractKey.renderPassCompatibilityKey;
        const auto pipeline=state.CropPipeline(context,state.config.expandedLightingFragment);
        for(uint32_t branch=0;branch<2;++branch)
        {
            state.lighting[branch]=std::make_unique<TerrainLitTarget>(*state.frame.backend,state.frame.actualWorldBudget,
                context.renderPass.pipelineContractKey.renderPassCompatibilityKey,state.config.rect);
            std::map<uint32_t,TerrainLitTargetChannel> inputs;
            Require(key.colorAttachments.size()==8,"Current lit ROI expects actual eight GBuffer/color attachments");
            for(uint32_t binding=0;binding<8;++binding)
                inputs[binding]=state.geometry[branch]->Channel(key.resolveAttachments.empty()?key.colorAttachments[binding]:key.resolveAttachments[binding]);
            inputs[8]=state.geometry[branch]->Channel(key.hasDepthResolveAttachment?key.depthResolveAttachment:key.depthAttachment);
            const auto clone=state.ClonePassInputs(context,*pipeline,inputs);
            state.Fullscreen(context,*state.lighting[branch],*pipeline,*clone);
        }
        state.lightingRecorded=true;
    }
    else if(context.renderPass.name=="toneMapping"&&state.lightingRecorded&&!state.displayRecorded)
    {
        const auto pipeline=state.CropPipeline(context,state.config.expandedToneMappingFragment);
        const auto& geometryKey=context.renderGraph.RequireUniquePass(RenderGraphPassType::Geometry).pipelineContractKey.renderPassCompatibilityKey;
        for(uint32_t branch=0;branch<2;++branch)
        {
            state.display[branch]=std::make_unique<TerrainLitTarget>(*state.frame.backend,state.frame.actualWorldBudget,
                context.renderPass.pipelineContractKey.renderPassCompatibilityKey,state.config.rect,true);
            const auto clone=state.ClonePassInputs(context,*pipeline,{{0,state.lighting[branch]->Channel(0)}});
            state.Fullscreen(context,*state.display[branch],*pipeline,*clone);
            state.litReadbacks[branch]=std::make_unique<TerrainLitReadback>(*state.frame.backend,state.frame.actualWorldBudget,
                state.config.rect,state.display[branch]->Channel(0).format);
            state.litReadbacks[branch]->Record(context.commandBuffer,state.display[branch]->Channel(0),
                state.display[branch]->Owner(),state.frame.submissionEpoch);
            const auto& depth=state.geometry[branch]->Channel(geometryKey.hasDepthResolveAttachment?
                geometryKey.depthResolveAttachment:geometryKey.depthAttachment);
            state.depthReadbacks[branch]=std::make_unique<TerrainLitReadback>(*state.frame.backend,state.frame.actualWorldBudget,
                state.config.rect,depth.format);
            state.depthReadbacks[branch]->Record(context.commandBuffer,depth,state.geometry[branch]->Owner(),state.frame.submissionEpoch);
        }
        state.displayRecorded=true;
    }
}
std::shared_ptr<const void> TerrainLitReplaySession::Submitted(uint64_t epoch)
{
    if(!(epoch==impl->frame.submissionEpoch&&impl->displayRecorded&&!impl->submitted))
        std::cerr<<"[LitReplay] submit mismatch epoch="<<epoch<<" expected="<<impl->frame.submissionEpoch
            <<" geometry="<<impl->geometryRecorded<<" lighting="<<impl->lightingRecorded
            <<" display="<<impl->displayRecorded<<" submitted="<<impl->submitted<<std::endl;
    Require(epoch==impl->frame.submissionEpoch&&impl->displayRecorded&&!impl->submitted,"Lit replay submission/incomplete pass chain mismatch");
    impl->submitted=epoch;return impl;
}
void TerrainLitReplaySession::AbortProvenUnsubmitted()
{
    Require(!impl->submitted,"Submitted lit replay must retire at its actual fence");
    impl->finished=true;
}
bool TerrainLitReplaySession::ConsumeCompleted(uint64_t completed)
{
    auto& state=*impl;if(state.finished)return true;if(!state.submitted||completed<state.submitted)return false;
    if(!state.failure.empty())throw std::runtime_error(state.failure);
    try
    {
    std::filesystem::create_directories(state.config.output);
    auto metadata=state.identity;
    metadata["completedEpoch"]=completed;metadata["channels"]=Json::array();
    for(uint32_t branch=0;branch<2;++branch)
        for(uint32_t kind=0;kind<2;++kind)
        {
            auto view=kind?state.depthReadbacks[branch]->TryConsume(completed):state.litReadbacks[branch]->TryConsume(completed);
            Require(bool(view),"Same-epoch lit/depth readback incomplete");
            const auto name=std::string(branch?"reference":"candidate")+(kind?"-depth.bin":"-lit.bin");
            const auto path=state.config.output/name;Require(!std::filesystem::exists(path),"Preserve existing native lit evidence");
            WriteTerrainEvidenceFile(path,[&](std::ostream& bytes){
                bytes.write(static_cast<const char*>(view->data),view->payloadBytes);
            });
            metadata["channels"].push_back({{"role",branch?"reference":"candidate"},{"channel",kind?"deviceDepth":"lit"},
                {"path",name},{"bytes",view->payloadBytes},{"format",uint32_t(view->format)},
                {"payloadBlake3",ContentHasher::HashBytes(view->data,view->payloadBytes).ToHex()},
                {"width",view->width},{"height",view->height},{"bytesPerTexel",view->bytesPerTexel},
                {"submissionEpoch",view->submissionEpoch},{"completedEpoch",view->completedEpoch}});
        }
    metadata["gpuExecuted"]=true;metadata["sharedActualBloomExposureDistortion"]=true;
    metadata["overallP5Passed"]=false;
    metadata["referenceAtlasActualVkRequirementBytes"]=state.reference->ActualImageAllocationBytes();
    const auto budget=state.frame.actualWorldBudget->Statistics();
    metadata["actualWorkspaceGpuBytes"]=budget.byClass[size_t(TerrainRvtCostClass::Workspace)].gpuBytes;
    WriteTerrainEvidenceFile(state.config.output/"native-lit-replay.json",
        [&](std::ostream& stream){stream<<metadata.dump(2)<<'\n';});
    state.finished=true;return true;
    }
    catch(const std::exception& error)
    {
        state.failure=std::string("Native lit evidence incomplete: ")+error.what();
        throw std::runtime_error(state.failure);
    }
}
std::shared_ptr<const void> TerrainLitReplaySession::Owner()const{return impl;}
}
