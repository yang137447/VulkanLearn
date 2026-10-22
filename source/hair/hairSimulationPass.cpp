#include "hair/hairSimulationPass.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "renderableObject.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/resolvedRenderScene.h"

namespace
{
constexpr uint32_t ParamsBinding = 0;
constexpr uint32_t CurrentPositionsBinding = 1;
constexpr uint32_t PreviousPositionsBinding = 2;
constexpr uint32_t RestPositionsBinding = 3;

// 每个 pool 的 set 容量；逐对象按 frame-in-flight 槽各分配一个 set。
constexpr uint32_t DescriptorSetsPerPool = VL::Hair::GuideSimulationParamBufferCount * 8;

// dispatch 前：上一帧 vertex shader 读过 current positions，本帧 compute 要写同一 buffer。
// 这是跨提交的 write-after-read；同一 graphics queue、命令缓冲提交顺序固定，因此在
// 本帧命令缓冲里用 src=VERTEX_SHADER 的 barrier 覆盖上一帧读取即可。src 同时包含
// TRANSFER_WRITE：device-local 位置 buffer 的初始 staging 上传发生在建对象时，
// 首次 dispatch 前必须把该 transfer 写转为 compute 和 vertex 可见。
// 上一帧 compute 对 previous positions 的读写也纳入 src 作用域，供本帧读取/覆盖。
void RecordPreDispatchBarrier(vk::CommandBuffer commandBuffer)
{
    vk::MemoryBarrier barrier;
    barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite |
            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer |
            vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eComputeShader,
        {}, barrier, nullptr, nullptr);
}

// dispatch 后：本帧所有图形 pass 的 vertex shader 读取 current positions；下一帧的
// compute 也会读写 previous positions。dst 同时覆盖两个 stage，使本帧写入对本帧
// 顶点读取和下一帧 dispatch 都可见。同一 queue 的提交顺序保证 barrier 先于后续提交生效。
void RecordPostDispatchBarrier(vk::CommandBuffer commandBuffer)
{
    vk::MemoryBarrier barrier;
    barrier.setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eComputeShader,
        {}, barrier, nullptr, nullptr);
}

// 同一对象的多子步是同一命令缓冲里的连续 dispatch；Vulkan 不会自动为它们建立
// 内存依赖，必须显式让前一子步的 current/previous 写入对后一子步可见。
void RecordSubstepBarrier(vk::CommandBuffer commandBuffer)
{
    vk::MemoryBarrier barrier;
    barrier.setSrcAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {}, barrier, nullptr, nullptr);
}
} // namespace

namespace VL::Hair
{
HairSimulationPass::~HairSimulationPass()
{
    ResetDescriptorCache();
    pipeline.reset();
    backend = nullptr;
}

void HairSimulationPass::Initialize(PipelineFactory& factory,
    RendererBackendVulkan& renderer)
{
    backend = &renderer;
    ComputeShaderArtifact artifact;
    // Candidate 入口不写 active compute cache；本 pipeline 也不注册 hot reload participant。
    pipeline = factory.CreateComputePipelineCandidate("hair/tressfxGuideSim", &artifact);
    initialized = true;
}

vk::DescriptorPool HairSimulationPass::AcquireDescriptorPool()
{
    for (DescriptorPoolBudget& budget : descriptorPools)
    {
        if (budget.remainingSets >= GuideSimulationParamBufferCount)
        {
            budget.remainingSets -= GuideSimulationParamBufferCount;
            return budget.pool;
        }
    }

    // 对象数量在构图阶段未知，因此按小池链增长；旧池在同一次 reset 中统一释放，
    // 不对单个 descriptor set 做 free，避免池在帧中途重排。
    const std::vector<vk::DescriptorPoolSize> sizes{
        {vk::DescriptorType::eUniformBuffer, DescriptorSetsPerPool},
        {vk::DescriptorType::eStorageBuffer,
            DescriptorSetsPerPool * 3u}};
    vk::DescriptorPoolCreateInfo info;
    info.setPoolSizes(sizes).setMaxSets(DescriptorSetsPerPool);
    const vk::DescriptorPool pool =
        backend->CreateDescriptorPool(info, "HairSimulationDescriptors");
    descriptorPools.push_back(
        {pool, DescriptorSetsPerPool - GuideSimulationParamBufferCount});
    return pool;
}

void HairSimulationPass::CreateObjectDescriptors(
    RenderableObject& object,
    ObjectDescriptors& descriptors)
{
    const vk::DescriptorPool pool = AcquireDescriptorPool();
    std::vector<vk::DescriptorSetLayout> layouts(
        GuideSimulationParamBufferCount, pipeline->GetDescriptorSetLayouts().at(0));
    vk::DescriptorSetAllocateInfo allocate;
    allocate.setDescriptorPool(pool).setSetLayouts(layouts);
    std::vector<vk::DescriptorSet> allocated(GuideSimulationParamBufferCount);
    backend->AllocateDescriptorSets(allocate, allocated);
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
        descriptors.sets[slot] = allocated[slot];

    const vk::DescriptorBufferInfo currentInfo = object.GetTressfxPositionInfo();
    const vk::DescriptorBufferInfo previousInfo = object.GetTressfxPreviousPositionInfo();
    const vk::DescriptorBufferInfo restInfo = object.GetTressfxRestPositionInfo();
    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(GuideSimulationParamBufferCount * 4u);
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
    {
        const vk::DescriptorBufferInfo paramsInfo = object.GetTressfxParamsInfo(slot);
        const vk::DescriptorSet set = descriptors.sets[slot];
        vk::WriteDescriptorSet paramsWrite;
        paramsWrite.setDstSet(set).setDstBinding(ParamsBinding).setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eUniformBuffer).setBufferInfo(paramsInfo);
        vk::WriteDescriptorSet currentWrite;
        currentWrite.setDstSet(set).setDstBinding(CurrentPositionsBinding).setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer).setBufferInfo(currentInfo);
        vk::WriteDescriptorSet previousWrite;
        previousWrite.setDstSet(set).setDstBinding(PreviousPositionsBinding).setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer).setBufferInfo(previousInfo);
        vk::WriteDescriptorSet restWrite;
        restWrite.setDstSet(set).setDstBinding(RestPositionsBinding).setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer).setBufferInfo(restInfo);
        writes.push_back(paramsWrite);
        writes.push_back(currentWrite);
        writes.push_back(previousWrite);
        writes.push_back(restWrite);
    }
    backend->UpdateDescriptorSets(writes);

    descriptors.currentPositions = currentInfo.buffer;
    descriptors.previousPositions = previousInfo.buffer;
    descriptors.restPositions = restInfo.buffer;
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
        descriptors.params[slot] = object.GetTressfxParamsInfo(slot).buffer;
}

HairSimulationPass::ObjectDescriptors& HairSimulationPass::EnsureObjectDescriptors(
    RenderableObject& object)
{
    const RenderableObject* key = &object;
    auto cached = objectDescriptors.find(key);
    if (cached != objectDescriptors.end())
    {
        const ObjectDescriptors& descriptors = cached->second;
        bool matches = descriptors.currentPositions == object.GetTressfxPositionInfo().buffer &&
            descriptors.previousPositions == object.GetTressfxPreviousPositionInfo().buffer &&
            descriptors.restPositions == object.GetTressfxRestPositionInfo().buffer;
        for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount && matches; ++slot)
            matches = descriptors.params[slot] == object.GetTressfxParamsInfo(slot).buffer;
        if (matches)
            return cached->second;
        // 正常 World/graph 重建会新建本 pass，不会复用对象地址；句柄校验仍保留，
        // 用于在缓存意外过期时拒绝绑定旧 buffer。旧 set 留在池里，统一 reset 时释放。
        objectDescriptors.erase(cached);
    }

    ObjectDescriptors descriptors;
    CreateObjectDescriptors(object, descriptors);
    return objectDescriptors.emplace(key, std::move(descriptors)).first->second;
}

void HairSimulationPass::Record(vk::CommandBuffer commandBuffer,
    const VL::ResolvedRenderScene& scene,
    uint32_t frameSlot,
    float deltaSeconds)
{
    if (!initialized || !pipeline)
        return;
    // frameSlot 直接索引 params UBO；调用方必须传 frame-in-flight 槽（0..1）。
    if (frameSlot >= GuideSimulationParamBufferCount)
        throw std::out_of_range("TressFX guide simulation frame slot is out of range");

    struct Dispatch
    {
        std::shared_ptr<RenderableObject> object;
        uint32_t guideCount = 0;
    };
    std::vector<Dispatch> dispatches;
    std::unordered_set<const RenderableObject*> seen;
    for (const ResolvedMaterialGroup& materialGroup : scene.materialGroups)
    {
        for (const ResolvedMaterialInstanceGroup& instanceGroup : materialGroup.materialInstances)
        {
            for (const ResolvedDrawPacket& draw : instanceGroup.draws)
            {
                std::shared_ptr<RenderableObject> object = draw.renderableObject.lock();
                if (!object || !object->UsesGpuGuideSimulation())
                    continue;
                // 同一对象可能因材质分组出现在多个 draw 中；重复 dispatch 会多步进一次。
                if (!seen.insert(object.get()).second)
                    continue;
                const uint32_t guideCount = object->GetTressfxGuideCount();
                dispatches.push_back({std::move(object), guideCount});
            }
        }
    }
    if (dispatches.empty())
        return;

    if (!reportedFirstDispatch)
    {
        // 一次性诊断：确认 GPU backend 真正录到了 dispatch，而不是静默走静态回退。
        uint32_t totalGuides = 0;
        uint32_t totalPoints = 0;
        for (const Dispatch& dispatch : dispatches)
        {
            totalGuides += dispatch.guideCount;
            totalPoints += dispatch.guideCount * dispatch.object->GetTressfxPointsPerGuide();
        }
        std::cout << "[TressFX] GPU guide simulation dispatch: objects="
                  << dispatches.size() << " guides=" << totalGuides
                  << " points=" << totalPoints << std::endl;
        reportedFirstDispatch = true;
    }

    RecordPreDispatchBarrier(commandBuffer);
    // pipeline 对整个 Record 恒定，绑定一次即可；descriptor set 逐对象变化。
    pipeline->Bind(commandBuffer);
    for (Dispatch& dispatch : dispatches)
    {
        RenderableObject& object = *dispatch.object;
        ObjectDescriptors& state = EnsureObjectDescriptors(object);
        const TressfxSimulationConfig& config = object.GetTressfxSimulationConfig();

        // 与 CPU GuideSimulation::Step 相同的 accumulator 算术。这里刻意重复这 6 行：
        // GPU 侧还没有跨帧状态 buffer，pass 的逐对象状态就是权威；两处必须一起改。
        state.accumulator += std::clamp(deltaSeconds, 0.0f, config.maxDt);
        uint32_t steps = 0;
        while (state.accumulator >= config.fixedStep && steps < config.maxSubsteps)
        {
            state.accumulator -= config.fixedStep;
            ++steps;
        }
        if (steps == config.maxSubsteps)
            state.accumulator = 0.0f;
        if (steps == 0)
            continue;

        // 一帧内所有子步共用 frame-start simulationTime 和 fixedStep，因此参数只写一次。
        const GuideSimulationParams params = MakeGuideSimulationParams(
            config,
            dispatch.guideCount,
            object.GetTressfxPointsPerGuide(),
            dispatch.guideCount * object.GetTressfxPointsPerGuide(),
            state.simulationTime);
        object.WriteTressfxSimulationParams(params, frameSlot);
        state.simulationTime += static_cast<float>(steps) * config.fixedStep;

        const vk::DescriptorSet set = state.sets[frameSlot];
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
            pipeline->GetPipelineLayout(), 0, set, nullptr);
        const uint32_t groupCount =
            (dispatch.guideCount + GuideSimulationWorkgroupSize - 1) /
            GuideSimulationWorkgroupSize;
        for (uint32_t substep = 0; substep < steps; ++substep)
        {
            if (substep > 0)
                RecordSubstepBarrier(commandBuffer);
            pipeline->Dispatch(commandBuffer, groupCount, 1, 1);
        }
    }
    RecordPostDispatchBarrier(commandBuffer);
}

void HairSimulationPass::ResetDescriptorCache() noexcept
{
    objectDescriptors.clear();
    if (backend)
    {
        for (DescriptorPoolBudget& budget : descriptorPools)
            backend->DestroyDescriptorPool(budget.pool);
    }
    descriptorPools.clear();
    initialized = false;
}

} // namespace VL::Hair
