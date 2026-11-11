#include "hair/hairSimulationPass.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "animation/skinPose.h"
#include "hair/tressfxSourceSchedule.h"
#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "renderableObject.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/resolvedRenderScene.h"

#include "hair/hairSimulationPassInternal.h"

namespace VL::Hair
{
vk::DescriptorPool HairSimulationPass::AcquireDescriptorPool(uint32_t pass,
    uint32_t setCount)
{
    std::vector<DescriptorPoolBudget>& pools = descriptorPools[pass];
    for (DescriptorPoolBudget& budget : pools)
    {
        if (budget.remainingSets >= setCount)
        {
            budget.remainingSets -= setCount;
            return budget.pool;
        }
    }

    // 对象数量在构图阶段未知，因此按小池链增长；旧池在同一次 reset 中统一释放，
    // 不对单个 descriptor set 做 free，避免池在帧中途重排。
    const std::vector<vk::DescriptorPoolSize> sizes{
        {vk::DescriptorType::eUniformBuffer,
            DescriptorSetsPerPool * UniformBuffersPerSet},
        {vk::DescriptorType::eStorageBuffer,
            DescriptorSetsPerPool * StorageBuffersPerSet}};
    vk::DescriptorPoolCreateInfo info;
    info.setPoolSizes(sizes).setMaxSets(DescriptorSetsPerPool);
    const vk::DescriptorPool pool =
        backend->CreateDescriptorPool(info, "HairSimulationDescriptors");
    pools.push_back({pool, DescriptorSetsPerPool - setCount});
    return pool;
}

void HairSimulationPass::CreateObjectDescriptors(
    RenderableObject& object,
    ObjectDescriptors& descriptors)
{
    // vulkan-hpp 的 setBufferInfo 只保存指针（ArrayProxyNoTemporaries），因此
    // DescriptorBufferInfo 必须比 UpdateDescriptorSets 活得更久；这里按
    // [frame slot][binding] 放在函数级数组里，元素地址稳定。
    std::array<std::array<vk::DescriptorBufferInfo, FollowFrameRefBinding + 1>,
        GuideSimulationParamBufferCount>
        bindingInfos{};
    // reset 首轮 Integration 的 binding 0 换成独立参数块，其余 binding 与正常
    // Integration 相同；同样按 slot 保持地址稳定。
    std::array<vk::DescriptorBufferInfo, GuideSimulationParamBufferCount>
        resetParamInfos{};
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
    {
        resetParamInfos[slot] = object.GetTressfxResetParamsInfo(slot);
        std::array<vk::DescriptorBufferInfo, FollowFrameRefBinding + 1>& infos =
            bindingInfos[slot];
        infos[ParamsBinding] = object.GetTressfxParamsInfo(slot);
        infos[CurrentPositionsBinding] = object.GetTressfxPositionInfo();
        infos[PreviousPositionsBinding] = object.GetTressfxPreviousPositionInfo();
        infos[PreviousPreviousPositionsBinding] =
            object.GetTressfxPreviousPreviousPositionInfo();
        infos[RestPositionsBinding] = object.GetTressfxRestPositionInfo();
        infos[RestLengthsBinding] = object.GetTressfxRestLengthInfo();
        infos[StrandLevelBinding] = object.GetTressfxStrandLevelInfo();
        infos[CollidersBinding] = object.GetTressfxColliderInfo(slot);
        infos[SdfParamsBinding] = object.GetTressfxSdfParamsInfo(slot);
        infos[SdfTriangleBinding] = object.GetTressfxSdfTriangleInfo();
        infos[SdfIndexBinding] = object.GetTressfxSdfIndexInfo();
        infos[SdfGridBinding] = object.GetTressfxSdfGridInfo();
        infos[SdfProxyInfluenceBinding] = object.GetTressfxSdfProxyInfluenceInfo();
        // palette 是对象自己的逐 frame-in-flight buffer（静态句柄），内容每帧写。
        infos[SdfPaletteBinding] = object.GetTressfxSdfProxyPaletteInfo(slot);
        infos[BoneWeightsBinding] = object.GetTressfxBoneWeightsInfo();
        infos[SkinningPaletteBinding] =
            object.GetTressfxSkinningPaletteInfo(slot);
        infos[PenetrationStatsBinding] =
            object.GetTressfxPenetrationStatsInfo(slot);
        infos[GroupSimulationConfigBinding] =
            object.GetTressfxGuideGroupConfigInfo(slot);
        infos[GuideGroupIdBinding] = object.GetTressfxGuideGroupIdInfo();
        infos[SimulationGuideIndexBinding] =
            object.GetTressfxSimulationGuideInfo();
        infos[FollowGuideIndexBinding] = object.GetTressfxFollowGuideInfo();
        infos[FollowOffsetBinding] = object.GetTressfxFollowOffsetInfo();
        infos[FollowFrameRefBinding] = object.GetTressfxFollowFrameRefInfo();
    }

    const bool hasSdfCollision = object.HasTressfxSdfCollision();
    const bool hasFollowBinding = object.HasTressfxFollowBinding();
    std::vector<vk::WriteDescriptorSet> writes;
    for (uint32_t pass = 0; pass < GuideSimulationPassCount; ++pass)
    {
        // 未声明 collision.mesh 的对象没有 SDF buffer；Sdf* pass 的 set 保持
        // 空句柄，Host 也不会录制这三个 dispatch。
        if (!hasSdfCollision && PassUsesSdf(static_cast<VL::Hair::GuideSimulationPass>(pass)))
            continue;
        if (!hasFollowBinding &&
            PassUsesFollow(static_cast<VL::Hair::GuideSimulationPass>(pass)))
            continue;
        const PassDefinition& definition = PassDefinitions[pass];
        const vk::DescriptorPool pool =
            AcquireDescriptorPool(pass, GuideSimulationParamBufferCount);
        std::vector<vk::DescriptorSetLayout> layouts(
            GuideSimulationParamBufferCount,
            pipelines[pass]->GetDescriptorSetLayouts().at(0));
        vk::DescriptorSetAllocateInfo allocate;
        allocate.setDescriptorPool(pool).setSetLayouts(layouts);
        std::vector<vk::DescriptorSet> allocated(GuideSimulationParamBufferCount);
        backend->AllocateDescriptorSets(allocate, allocated);

        writes.reserve(writes.size() +
            static_cast<size_t>(GuideSimulationParamBufferCount) *
                definition.bindingCount);
        for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
        {
            descriptors.sets[pass][slot] = allocated[slot];
            for (uint32_t index = 0; index < definition.bindingCount; ++index)
            {
                const PassBinding& binding = definition.bindings[index];
                vk::WriteDescriptorSet write;
                write.setDstSet(allocated[slot]).setDstBinding(binding.binding)
                    .setDescriptorType(binding.type)
                    .setBufferInfo(bindingInfos[slot][binding.binding]);
                writes.push_back(write);
            }
        }
    }

    // reset 首轮 Integration 的独立 descriptor set。它与正常 Integration 的
    // layout 相同，只有 binding 0 指向 resetParams；每个 frame-in-flight 槽一个。
    {
        constexpr uint32_t integrationPass =
            static_cast<uint32_t>(VL::Hair::GuideSimulationPass::Integration);
        const vk::DescriptorPool pool =
            AcquireDescriptorPool(integrationPass, GuideSimulationParamBufferCount);
        std::vector<vk::DescriptorSetLayout> layouts(
            GuideSimulationParamBufferCount,
            pipelines[integrationPass]->GetDescriptorSetLayouts().at(0));
        vk::DescriptorSetAllocateInfo allocate;
        allocate.setDescriptorPool(pool).setSetLayouts(layouts);
        std::vector<vk::DescriptorSet> allocated(GuideSimulationParamBufferCount);
        backend->AllocateDescriptorSets(allocate, allocated);
        const PassDefinition& definition = PassDefinitions[integrationPass];
        for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
        {
            descriptors.resetSets[slot] = allocated[slot];
            for (uint32_t index = 0; index < definition.bindingCount; ++index)
            {
                const PassBinding& binding = definition.bindings[index];
                const vk::DescriptorBufferInfo& info =
                    binding.binding == ParamsBinding ? resetParamInfos[slot]
                                                     : bindingInfos[slot][binding.binding];
                vk::WriteDescriptorSet write;
                write.setDstSet(allocated[slot]).setDstBinding(binding.binding)
                    .setDescriptorType(binding.type).setBufferInfo(info);
                writes.push_back(write);
            }
        }
    }
    backend->UpdateDescriptorSets(writes);

    descriptors.currentPositions = object.GetTressfxPositionInfo().buffer;
    descriptors.previousPositions = object.GetTressfxPreviousPositionInfo().buffer;
    descriptors.previousPreviousPositions =
        object.GetTressfxPreviousPreviousPositionInfo().buffer;
    descriptors.restPositions = object.GetTressfxRestPositionInfo().buffer;
    descriptors.restLengths = object.GetTressfxRestLengthInfo().buffer;
    descriptors.strandLevel = object.GetTressfxStrandLevelInfo().buffer;
    // 胶囊 SSBO 逐槽绑定；内容会随骨骼移动，但 buffer 句柄在创建后不变。
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
        descriptors.colliders[slot] = object.GetTressfxColliderInfo(slot).buffer;
    descriptors.sdfGrid = object.GetTressfxSdfGridInfo().buffer;
    descriptors.sdfTriangles = object.GetTressfxSdfTriangleInfo().buffer;
    descriptors.sdfIndices = object.GetTressfxSdfIndexInfo().buffer;
    descriptors.sdfProxyInfluences =
        object.GetTressfxSdfProxyInfluenceInfo().buffer;
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
    {
        descriptors.sdfPalette[slot] =
            object.GetTressfxSdfProxyPaletteInfo(slot).buffer;
        descriptors.sdfParams[slot] = object.GetTressfxSdfParamsInfo(slot).buffer;
        descriptors.params[slot] = object.GetTressfxParamsInfo(slot).buffer;
        descriptors.resetParams[slot] =
            object.GetTressfxResetParamsInfo(slot).buffer;
        descriptors.skinningPalette[slot] =
            object.GetTressfxSkinningPaletteInfo(slot).buffer;
        descriptors.penetrationStats[slot] =
            object.GetTressfxPenetrationStatsInfo(slot).buffer;
        descriptors.boneWeights = object.GetTressfxBoneWeightsInfo().buffer;
        descriptors.groupConfigs[slot] =
            object.GetTressfxGuideGroupConfigInfo(slot).buffer;
    }
    descriptors.guideGroupIds = object.GetTressfxGuideGroupIdInfo().buffer;
    descriptors.simulationGuides =
        object.GetTressfxSimulationGuideInfo().buffer;
    descriptors.followGuide = object.GetTressfxFollowGuideInfo().buffer;
    descriptors.followOffset = object.GetTressfxFollowOffsetInfo().buffer;
    descriptors.followFrameRef = object.GetTressfxFollowFrameRefInfo().buffer;
}

HairSimulationPass::ObjectDescriptors& HairSimulationPass::EnsureObjectDescriptors(
    RenderableObject& object)
{
    const RenderableObject* key = &object;
    auto cached = objectDescriptors.find(key);
    if (cached != objectDescriptors.end())
    {
        if (MatchesObjectDescriptors(object, cached->second))
            return cached->second;
        // 正常 World/graph 重建会新建本 pass，不会复用对象地址；句柄校验仍保留，
        // 用于在缓存意外过期时拒绝绑定旧 buffer。旧 set 留在池里，统一 reset 时释放。
        objectDescriptors.erase(cached);
    }

    ObjectDescriptors descriptors;
    CreateObjectDescriptors(object, descriptors);
    return objectDescriptors.emplace(key, std::move(descriptors)).first->second;
}

bool HairSimulationPass::MatchesObjectDescriptors(
    const RenderableObject& object, const ObjectDescriptors& descriptors) const
{
    bool matches = descriptors.currentPositions == object.GetTressfxPositionInfo().buffer &&
        descriptors.previousPositions ==
            object.GetTressfxPreviousPositionInfo().buffer &&
        descriptors.previousPreviousPositions ==
            object.GetTressfxPreviousPreviousPositionInfo().buffer &&
        descriptors.restPositions == object.GetTressfxRestPositionInfo().buffer &&
        descriptors.restLengths == object.GetTressfxRestLengthInfo().buffer &&
        descriptors.strandLevel == object.GetTressfxStrandLevelInfo().buffer &&
        descriptors.sdfGrid == object.GetTressfxSdfGridInfo().buffer &&
        descriptors.sdfTriangles == object.GetTressfxSdfTriangleInfo().buffer &&
        descriptors.sdfIndices == object.GetTressfxSdfIndexInfo().buffer &&
        descriptors.sdfProxyInfluences ==
            object.GetTressfxSdfProxyInfluenceInfo().buffer;
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount && matches; ++slot)
    {
        matches = descriptors.params[slot] == object.GetTressfxParamsInfo(slot).buffer &&
            descriptors.resetParams[slot] ==
                object.GetTressfxResetParamsInfo(slot).buffer &&
            descriptors.boneWeights == object.GetTressfxBoneWeightsInfo().buffer &&
            descriptors.skinningPalette[slot] ==
                object.GetTressfxSkinningPaletteInfo(slot).buffer &&
            descriptors.penetrationStats[slot] ==
                object.GetTressfxPenetrationStatsInfo(slot).buffer &&
            descriptors.colliders[slot] ==
                object.GetTressfxColliderInfo(slot).buffer &&
            descriptors.sdfPalette[slot] ==
                object.GetTressfxSdfProxyPaletteInfo(slot).buffer &&
            descriptors.sdfParams[slot] ==
                object.GetTressfxSdfParamsInfo(slot).buffer &&
            descriptors.groupConfigs[slot] ==
                object.GetTressfxGuideGroupConfigInfo(slot).buffer;
    }
    matches = matches &&
        descriptors.guideGroupIds == object.GetTressfxGuideGroupIdInfo().buffer &&
        descriptors.simulationGuides ==
            object.GetTressfxSimulationGuideInfo().buffer &&
        descriptors.followGuide == object.GetTressfxFollowGuideInfo().buffer &&
        descriptors.followOffset == object.GetTressfxFollowOffsetInfo().buffer &&
        descriptors.followFrameRef == object.GetTressfxFollowFrameRefInfo().buffer;
    return matches;
}

HairSimulationPass::SkinningDescriptors&
HairSimulationPass::EnsureSkinningDescriptors(
    const std::shared_ptr<RenderableObject>& object)
{
    auto cached = skinningDescriptors.find(object.get());
    if (cached != skinningDescriptors.end())
    {
        const SkinningDescriptors& state = cached->second;
        bool matches = state.owner.lock() == object &&
            state.currentPositions == object->GetTressfxPositionInfo().buffer &&
            state.restPositions == object->GetTressfxRestPositionInfo().buffer &&
            state.boneWeights == object->GetTressfxBoneWeightsInfo().buffer;
        for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount && matches; ++slot)
            matches = state.params[slot] == object->GetTressfxParamsInfo(slot).buffer &&
                state.palette[slot] == object->GetTressfxSkinningPaletteInfo(slot).buffer;
        if (matches)
            return cached->second;
        // World 包退休后对象地址可能复用，不能仅凭地址绑定上一代 descriptor。
        skinningDescriptors.erase(cached);
    }

    SkinningDescriptors descriptors;
    descriptors.owner = object;
    descriptors.currentPositions = object->GetTressfxPositionInfo().buffer;
    descriptors.restPositions = object->GetTressfxRestPositionInfo().buffer;
    descriptors.boneWeights = object->GetTressfxBoneWeightsInfo().buffer;
    const vk::DescriptorPool pool =
        AcquireDescriptorPool(GuideSimulationPassCount, GuideSimulationParamBufferCount);
    const std::vector<vk::DescriptorSetLayout> layouts(
        GuideSimulationParamBufferCount,
        skinningPipeline->GetDescriptorSetLayouts().at(0));
    vk::DescriptorSetAllocateInfo allocate;
    allocate.setDescriptorPool(pool).setSetLayouts(layouts);
    std::vector<vk::DescriptorSet> sets(GuideSimulationParamBufferCount);
    backend->AllocateDescriptorSets(allocate, sets);

    // 与 solver 相同，WriteDescriptorSet 引用的 BufferInfo 必须存活到批量更新完成。
    std::array<std::array<vk::DescriptorBufferInfo, SkinningPaletteBinding + 1>,
        GuideSimulationParamBufferCount> infos{};
    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(GuideSimulationParamBufferCount * std::size(SkinningBindings));
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
    {
        descriptors.sets[slot] = sets[slot];
        descriptors.params[slot] = object->GetTressfxParamsInfo(slot).buffer;
        descriptors.palette[slot] = object->GetTressfxSkinningPaletteInfo(slot).buffer;
        infos[slot][ParamsBinding] = object->GetTressfxParamsInfo(slot);
        infos[slot][CurrentPositionsBinding] = object->GetTressfxPositionInfo();
        infos[slot][RestPositionsBinding] = object->GetTressfxRestPositionInfo();
        infos[slot][BoneWeightsBinding] = object->GetTressfxBoneWeightsInfo();
        infos[slot][SkinningPaletteBinding] = object->GetTressfxSkinningPaletteInfo(slot);
        for (const PassBinding& binding : SkinningBindings)
        {
            vk::WriteDescriptorSet write;
            write.setDstSet(sets[slot]).setDstBinding(binding.binding)
                .setDescriptorType(binding.type)
                .setBufferInfo(infos[slot][binding.binding]);
            writes.push_back(write);
        }
    }
    backend->UpdateDescriptorSets(writes);
    return skinningDescriptors.emplace(object.get(), std::move(descriptors)).first->second;
}

void HairSimulationPass::RecordSkinning(vk::CommandBuffer commandBuffer,
    const VL::ResolvedRenderScene& scene,
    const VL::RenderScene& renderScene,
    uint32_t frameSlot)
{
    if (!skinningPipeline)
        return;
    for (auto cached = skinningDescriptors.begin(); cached != skinningDescriptors.end();)
    {
        if (cached->second.owner.expired())
            cached = skinningDescriptors.erase(cached);
        else
            ++cached;
    }
    const SimulationOwnerMap owners = CollectSimulationOwners(scene, renderScene);
    std::unordered_set<const RenderableObject*> seen;
    bool anyDispatch = false;
    for (const ResolvedMaterialGroup& materialGroup : scene.materialGroups)
        for (const ResolvedMaterialInstanceGroup& instanceGroup : materialGroup.materialInstances)
            for (const ResolvedDrawPacket& draw : instanceGroup.draws)
            {
                const std::shared_ptr<RenderableObject> object = draw.renderableObject;
                if (!object || !object->UsesGpuHairSkinning() ||
                    !seen.insert(object.get()).second)
                    continue;
                const TressfxSkinningBinding& skinning = object->GetTressfxSkinningBinding();
                const VL::RenderDrawPacket* sourceDraw =
                    FindSkinningSourceDraw(skinning, renderScene);
                if (!skinning.enabled || !HasRootSkinPose(sourceDraw, skinning.jointIndex))
                    continue;
                SkinningDescriptors& state = EnsureSkinningDescriptors(object);
                state.simulationOwner.Merge(owners.at(object.get()));
                if (state.simulationOwner.HasConflict())
                {
                    if (!state.reportedOwnerConflict)
                    {
                        std::cerr << "[TressFX] skinning disabled for shared RenderableObject \""
                                  << object->GetName()
                                  << "\": multiple placements share mutable positions" << std::endl;
                        state.reportedOwnerConflict = true;
                    }
                    continue;
                }
                const VL::RenderDrawPacket& hairDraw =
                    renderScene.drawPackets[draw.drawPacketIndex];
                UpdateTressfxFrameContext(state.frameContext, hairDraw.model,
                    sourceDraw->model, *sourceDraw->skinPose,
                    skinning.jointIndex, sourceDraw->simulationDeltaSeconds);
                object->WriteTressfxSkinningPalette(state.frameContext.jointMatrices, frameSlot);
                const GuideSimulationParams params = MakeGuideSkinningParams(
                    object->GetTressfxGuideCount(), object->GetTressfxPointsPerGuide(),
                    state.frameContext.skinningMatrix, !skinning.strandBones.empty());
                object->WriteTressfxSimulationParams(params, frameSlot);
                if (!anyDispatch)
                    RecordPreDispatchBarrier(commandBuffer);
                skinningPipeline->Bind(commandBuffer);
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                    skinningPipeline->GetPipelineLayout(), 0, state.sets[frameSlot], nullptr);
                skinningPipeline->Dispatch(commandBuffer,
                    (params.pointCount + GuideSimulationWorkgroupSize - 1) /
                        GuideSimulationWorkgroupSize, 1, 1);
                anyDispatch = true;
                if (!state.reportedFirstDispatch)
                {
                    std::cout << "[TressFX] GPU skinning-only dispatch: object=\""
                              << object->GetName() << "\" guides=" << params.guideCount
                              << " points=" << params.pointCount << std::endl;
                    state.reportedFirstDispatch = true;
                }
            }
    if (anyDispatch)
        RecordPostDispatchBarrier(commandBuffer);
}

void HairSimulationPass::RecordPass(vk::CommandBuffer commandBuffer,
    GuideSimulationPass pass,
    vk::DescriptorSet descriptorSet,
    uint32_t groupCount,
    bool& anyDispatch,
    uint32_t& objectDispatchCount)
{
    const uint32_t index = static_cast<uint32_t>(pass);
    ComputePipeline& pipeline = *pipelines[index];
    // 相邻 pass 之间必须显式同步；第一个 pass 由 RecordPreDispatchBarrier 覆盖。
    if (anyDispatch)
        RecordInterPassBarrier(commandBuffer);
    pipeline.Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
        pipeline.GetPipelineLayout(), 0, descriptorSet, nullptr);
    pipeline.Dispatch(commandBuffer, groupCount, 1, 1);
    anyDispatch = true;
    ++objectDispatchCount;
}

void HairSimulationPass::ResetDescriptorCache() noexcept
{
    objectDescriptors.clear();
    skinningDescriptors.clear();
    if (backend)
    {
        for (std::vector<DescriptorPoolBudget>& pools : descriptorPools)
        {
            for (DescriptorPoolBudget& budget : pools)
                backend->DestroyDescriptorPool(budget.pool);
            pools.clear();
        }
    }
    initialized = false;
}

} // namespace VL::Hair
