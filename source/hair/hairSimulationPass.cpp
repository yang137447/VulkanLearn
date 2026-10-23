#include "hair/hairSimulationPass.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "animation/skinPose.h"
#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "renderableObject.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/resolvedRenderScene.h"

namespace
{
// 所有 pass 共用同一组 binding 编号；每个 shader 只声明自己用到的子集，
// descriptor set layout 因此按 pipeline 独立。
constexpr uint32_t ParamsBinding = 0;
constexpr uint32_t CurrentPositionsBinding = 1;
constexpr uint32_t PreviousPositionsBinding = 2;
constexpr uint32_t PreviousPreviousPositionsBinding = 3;
constexpr uint32_t RestPositionsBinding = 4;
constexpr uint32_t RestLengthsBinding = 5;
constexpr uint32_t StrandLevelBinding = 6;
constexpr uint32_t CollidersBinding = 7;
// `collision.mesh` 的逐帧距离场资源。只有 SDF 的三个 pass 使用；未声明
// collision.mesh 的对象不会分配这些 descriptor set。
constexpr uint32_t SdfParamsBinding = 8;
constexpr uint32_t SdfTriangleBinding = 9;
constexpr uint32_t SdfIndexBinding = 10;
constexpr uint32_t SdfGridBinding = 11;

// 每个对象需要 GuideSimulationPassCount * GuideSimulationParamBufferCount 个 set
// （9 * 2 = 18）；按 64 预留，池链只在对象数量溢出时增长。
constexpr uint32_t DescriptorSetsPerPool = 64;
// 单个 set 最多 3 个 SSBO（SdfSplat 用代理顶点、索引、距离网格；Collision 用
// current + colliders）。SdfSplat/SdfCollide 各有两个 UBO（params + SDF params）。
constexpr uint32_t StorageBuffersPerSet = 4;
constexpr uint32_t UniformBuffersPerSet = 2;

struct PassBinding
{
    uint32_t binding;
    vk::DescriptorType type;
};

struct PassDefinition
{
    const char* shaderName;
    const PassBinding* bindings;
    uint32_t bindingCount;
};

// 与 shader/glsl/hair/tressfxSim*.comp 的声明一一对应，顺序必须同步修改。
constexpr PassBinding IntegrationBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {RestPositionsBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding StrandLevelBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {StrandLevelBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding VspBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {StrandLevelBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding LocalShapeBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {RestPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {StrandLevelBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding LengthWindBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {RestLengthsBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding CollisionBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {CollidersBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding SdfClearBindings[] = {
    {SdfParamsBinding, vk::DescriptorType::eUniformBuffer},
    {SdfGridBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding SdfSplatBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {SdfParamsBinding, vk::DescriptorType::eUniformBuffer},
    {SdfTriangleBinding, vk::DescriptorType::eStorageBuffer},
    {SdfIndexBinding, vk::DescriptorType::eStorageBuffer},
    {SdfGridBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding SdfCollideBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {SdfParamsBinding, vk::DescriptorType::eUniformBuffer},
    {SdfGridBinding, vk::DescriptorType::eStorageBuffer},
};

constexpr PassDefinition PassDefinitions[] = {
    {"hair/tressfxSimIntegration", IntegrationBindings,
        static_cast<uint32_t>(std::size(IntegrationBindings))},
    {"hair/tressfxSimStrandLevel", StrandLevelBindings,
        static_cast<uint32_t>(std::size(StrandLevelBindings))},
    {"hair/tressfxSimVsp", VspBindings,
        static_cast<uint32_t>(std::size(VspBindings))},
    {"hair/tressfxSimLocalShape", LocalShapeBindings,
        static_cast<uint32_t>(std::size(LocalShapeBindings))},
    {"hair/tressfxSimLengthWind", LengthWindBindings,
        static_cast<uint32_t>(std::size(LengthWindBindings))},
    {"hair/tressfxSimCollision", CollisionBindings,
        static_cast<uint32_t>(std::size(CollisionBindings))},
    {"hair/tressfxSimSdfClear", SdfClearBindings,
        static_cast<uint32_t>(std::size(SdfClearBindings))},
    {"hair/tressfxSimSdfSplat", SdfSplatBindings,
        static_cast<uint32_t>(std::size(SdfSplatBindings))},
    {"hair/tressfxSimSdfCollide", SdfCollideBindings,
        static_cast<uint32_t>(std::size(SdfCollideBindings))},
};
static_assert(std::size(PassDefinitions) == VL::Hair::GuideSimulationPassCount,
    "PassDefinitions must cover every GuideSimulationPass in order");

// SDF 资源只在声明 collision.mesh 的对象上创建；其余对象的 Sdf* pass 既没有
// descriptor set 也不会被录制。
constexpr bool PassUsesSdf(VL::Hair::GuideSimulationPass pass)
{
    return pass == VL::Hair::GuideSimulationPass::SdfClear ||
        pass == VL::Hair::GuideSimulationPass::SdfSplat ||
        pass == VL::Hair::GuideSimulationPass::SdfCollide;
}

// dispatch 前：上一帧 vertex shader 读过 current positions，本帧 compute 要写同一 buffer。
// 这是跨提交的 write-after-read；同一 graphics queue、命令缓冲提交顺序固定，因此在
// 本帧命令缓冲里用 src=VERTEX_SHADER 的 barrier 覆盖上一帧读取即可。src 同时包含
// TRANSFER_WRITE：device-local 位置 buffer 的初始 staging 上传发生在建对象时，
// 首次 dispatch 前必须把该 transfer 写转为 compute 和 vertex 可见。
// 上一帧 compute 对 previous/prevPrev 的读写也纳入 src 作用域。
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
// compute 也会读写 previous/prevPrev/strandLevel。dst 同时覆盖两个 stage。
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

// 同一对象的相邻 pass 是同一命令缓冲里的连续 dispatch；Vulkan 不会自动建立内存依赖，
// 必须显式让前一 pass 的写入对后一 pass 可见。compute -> compute 的 read/write barrier
// 同时提供执行依赖与内存依赖。
void RecordInterPassBarrier(vk::CommandBuffer commandBuffer)
{
    vk::MemoryBarrier barrier;
    barrier.setSrcAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {}, barrier, nullptr, nullptr);
}

constexpr uint32_t PassIndex(VL::Hair::GuideSimulationPass pass)
{
    return static_cast<uint32_t>(pass);
}
} // namespace

namespace VL::Hair
{
HairSimulationPass::~HairSimulationPass()
{
    ResetDescriptorCache();
    for (std::shared_ptr<ComputePipeline>& pipeline : pipelines)
        pipeline.reset();
    backend = nullptr;
}

void HairSimulationPass::Initialize(PipelineFactory& factory,
    RendererBackendVulkan& renderer)
{
    backend = &renderer;
    for (uint32_t pass = 0; pass < GuideSimulationPassCount; ++pass)
    {
        ComputeShaderArtifact artifact;
        // Candidate 入口不写 active compute cache；本 pipeline 也不注册 hot reload participant。
        pipelines[pass] = factory.CreateComputePipelineCandidate(
            PassDefinitions[pass].shaderName, &artifact);
    }
    initialized = true;
}

vk::DescriptorPool HairSimulationPass::AcquireDescriptorPool(uint32_t pass)
{
    std::vector<DescriptorPoolBudget>& pools = descriptorPools[pass];
    for (DescriptorPoolBudget& budget : pools)
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
        {vk::DescriptorType::eUniformBuffer,
            DescriptorSetsPerPool * UniformBuffersPerSet},
        {vk::DescriptorType::eStorageBuffer,
            DescriptorSetsPerPool * StorageBuffersPerSet}};
    vk::DescriptorPoolCreateInfo info;
    info.setPoolSizes(sizes).setMaxSets(DescriptorSetsPerPool);
    const vk::DescriptorPool pool =
        backend->CreateDescriptorPool(info, "HairSimulationDescriptors");
    pools.push_back({pool, DescriptorSetsPerPool - GuideSimulationParamBufferCount});
    return pool;
}

void HairSimulationPass::CreateObjectDescriptors(
    RenderableObject& object,
    ObjectDescriptors& descriptors)
{
    // vulkan-hpp 的 setBufferInfo 只保存指针（ArrayProxyNoTemporaries），因此
    // DescriptorBufferInfo 必须比 UpdateDescriptorSets 活得更久；这里按
    // [frame slot][binding] 放在函数级数组里，元素地址稳定。
    std::array<std::array<vk::DescriptorBufferInfo, SdfGridBinding + 1>,
        GuideSimulationParamBufferCount>
        bindingInfos{};
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
    {
        std::array<vk::DescriptorBufferInfo, SdfGridBinding + 1>& infos =
            bindingInfos[slot];
        infos[ParamsBinding] = object.GetTressfxParamsInfo(slot);
        infos[CurrentPositionsBinding] = object.GetTressfxPositionInfo();
        infos[PreviousPositionsBinding] = object.GetTressfxPreviousPositionInfo();
        infos[PreviousPreviousPositionsBinding] =
            object.GetTressfxPreviousPreviousPositionInfo();
        infos[RestPositionsBinding] = object.GetTressfxRestPositionInfo();
        infos[RestLengthsBinding] = object.GetTressfxRestLengthInfo();
        infos[StrandLevelBinding] = object.GetTressfxStrandLevelInfo();
        infos[CollidersBinding] = object.GetTressfxColliderInfo();
        infos[SdfParamsBinding] = object.GetTressfxSdfParamsInfo(slot);
        infos[SdfTriangleBinding] = object.GetTressfxSdfTriangleInfo();
        infos[SdfIndexBinding] = object.GetTressfxSdfIndexInfo();
        infos[SdfGridBinding] = object.GetTressfxSdfGridInfo();
    }

    const bool hasSdfCollision = object.HasTressfxSdfCollision();
    std::vector<vk::WriteDescriptorSet> writes;
    for (uint32_t pass = 0; pass < GuideSimulationPassCount; ++pass)
    {
        // 未声明 collision.mesh 的对象没有 SDF buffer；Sdf* pass 的 set 保持
        // 空句柄，Host 也不会录制这三个 dispatch。
        if (!hasSdfCollision && PassUsesSdf(static_cast<VL::Hair::GuideSimulationPass>(pass)))
            continue;
        const PassDefinition& definition = PassDefinitions[pass];
        const vk::DescriptorPool pool = AcquireDescriptorPool(pass);
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
    backend->UpdateDescriptorSets(writes);

    descriptors.currentPositions = object.GetTressfxPositionInfo().buffer;
    descriptors.previousPositions = object.GetTressfxPreviousPositionInfo().buffer;
    descriptors.previousPreviousPositions =
        object.GetTressfxPreviousPreviousPositionInfo().buffer;
    descriptors.restPositions = object.GetTressfxRestPositionInfo().buffer;
    descriptors.restLengths = object.GetTressfxRestLengthInfo().buffer;
    descriptors.strandLevel = object.GetTressfxStrandLevelInfo().buffer;
    descriptors.colliders = object.GetTressfxColliderInfo().buffer;
    descriptors.sdfGrid = object.GetTressfxSdfGridInfo().buffer;
    descriptors.sdfTriangles = object.GetTressfxSdfTriangleInfo().buffer;
    descriptors.sdfIndices = object.GetTressfxSdfIndexInfo().buffer;
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
    {
        descriptors.sdfParams[slot] = object.GetTressfxSdfParamsInfo(slot).buffer;
        descriptors.params[slot] = object.GetTressfxParamsInfo(slot).buffer;
    }
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
            descriptors.previousPositions ==
                object.GetTressfxPreviousPositionInfo().buffer &&
            descriptors.previousPreviousPositions ==
                object.GetTressfxPreviousPreviousPositionInfo().buffer &&
            descriptors.restPositions == object.GetTressfxRestPositionInfo().buffer &&
            descriptors.restLengths == object.GetTressfxRestLengthInfo().buffer &&
            descriptors.strandLevel == object.GetTressfxStrandLevelInfo().buffer &&
            descriptors.colliders == object.GetTressfxColliderInfo().buffer &&
            descriptors.sdfGrid == object.GetTressfxSdfGridInfo().buffer &&
            descriptors.sdfTriangles == object.GetTressfxSdfTriangleInfo().buffer &&
            descriptors.sdfIndices == object.GetTressfxSdfIndexInfo().buffer;
        for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount && matches; ++slot)
        {
            matches = descriptors.params[slot] == object.GetTressfxParamsInfo(slot).buffer &&
                descriptors.sdfParams[slot] ==
                    object.GetTressfxSdfParamsInfo(slot).buffer;
        }
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

void HairSimulationPass::RecordPass(vk::CommandBuffer commandBuffer,
    GuideSimulationPass pass,
    vk::DescriptorSet descriptorSet,
    uint32_t groupCount,
    bool& anyDispatch)
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
}

bool HairSimulationPass::HasWork(const VL::ResolvedRenderScene& scene) const
{
    if (!initialized || !pipelines[0])
        return false;

    for (const ResolvedMaterialGroup& materialGroup : scene.materialGroups)
    {
        for (const ResolvedMaterialInstanceGroup& instanceGroup : materialGroup.materialInstances)
        {
            for (const ResolvedDrawPacket& draw : instanceGroup.draws)
            {
                const std::shared_ptr<RenderableObject> object = draw.renderableObject.lock();
                if (object && object->UsesGpuGuideSimulation())
                    return true;
            }
        }
    }
    return false;
}

void HairSimulationPass::Record(vk::CommandBuffer commandBuffer,
    const VL::ResolvedRenderScene& scene,
    const VL::RenderScene& renderScene,
    uint32_t frameSlot,
    float deltaSeconds)
{
    if (!initialized || !pipelines[0])
        return;
    // frameSlot 直接索引 params UBO；调用方必须传 frame-in-flight 槽（0..1）。
    if (frameSlot >= GuideSimulationParamBufferCount)
        throw std::out_of_range("TressFX guide simulation frame slot is out of range");

    struct Dispatch
    {
        std::shared_ptr<RenderableObject> object;
        uint32_t guideCount = 0;
        uint32_t pointCount = 0;
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
                const uint32_t pointsPerGuide = object->GetTressfxPointsPerGuide();
                // 先算完再 move：braced-init-list 从左到右求值，move 之后 object 已为空。
                dispatches.push_back({std::move(object), guideCount,
                    guideCount * pointsPerGuide});
            }
        }
    }
    if (dispatches.empty())
        return;

    if (!reportedFirstDispatch)
    {
        // 一次性诊断：确认 GPU backend 真正录到了多 pass dispatch，而不是静默回退。
        uint32_t totalGuides = 0;
        uint32_t totalPoints = 0;
        uint32_t sdfObjects = 0;
        for (const Dispatch& dispatch : dispatches)
        {
            totalGuides += dispatch.guideCount;
            totalPoints += dispatch.pointCount;
            if (dispatch.object->HasTressfxSdfCollision())
                ++sdfObjects;
        }
        std::cout << "[TressFX] GPU guide simulation dispatch: objects="
                  << dispatches.size() << " guides=" << totalGuides
                  << " points=" << totalPoints
                  << " passes=" << GuideSimulationPassCount
                  << " sdfObjects=" << sdfObjects << std::endl;
        reportedFirstDispatch = true;
    }

    RecordPreDispatchBarrier(commandBuffer);
    bool anyDispatch = false;
    for (Dispatch& dispatch : dispatches)
    {
        RenderableObject& object = *dispatch.object;
        const TressfxSimulationConfig& config = object.GetTressfxSimulationConfig();

        // 变量步长：TressFX 直接使用帧 DeltaTime；这里按 maxDt 收紧，一帧一个 step。
        const float timeStep = std::clamp(deltaSeconds, 0.0f, config.maxDt);
        if (timeStep <= 0.0f)
            continue;

        ObjectDescriptors& state = EnsureObjectDescriptors(object);

        // 根点骨骼矩阵来自同一帧的 RenderScene 冻结快照。只按场景对象身份匹配，
        // 不复制播放器或骨架状态；pose 缺失时保留上一帧矩阵并只报告一次。
        const VL::Hair::TressfxSkinningBinding& skinning =
            object.GetTressfxSkinningBinding();
        if (skinning.enabled)
        {
            const VL::RenderDrawPacket* sourceDraw = nullptr;
            for (const VL::RenderDrawPacket& packet : renderScene.drawPackets)
            {
                if (packet.sceneObjectIdentity != skinning.sourceSceneObject ||
                    !packet.skinPose)
                    continue;
                if (skinning.jointIndex >= packet.skinPose->matrices.size())
                    continue;
                sourceDraw = &packet;
                break;
            }
            if (sourceDraw != nullptr)
                state.skinningMatrix = sourceDraw->skinPose->matrices[skinning.jointIndex];
            else if (!state.reportedMissingSkinPose)            {
                std::cerr << "[TressFX] skinning pose unavailable for object \""
                          << object.GetName() << "\" source=\""
                          << skinning.sourceSceneObject << "\" bone=\""
                          << skinning.rootBone << "\"; keeping the last matrix"
                          << std::endl;
                state.reportedMissingSkinPose = true;
            }
        }

        // 所有 pass 共用 frame-start simulationTime 与同一个 dt，params 只写一次。
        const GuideSimulationParams params = MakeGuideSimulationParams(
            config,
            dispatch.guideCount,
            object.GetTressfxPointsPerGuide(),
            dispatch.pointCount,
            timeStep,
            state.simulationTime,
            state.skinningMatrix);
        object.WriteTressfxSimulationParams(params, frameSlot);
        // SDF 域每帧从蒙皮后的代理包围盒重算；非有限包围盒会返回 false，
        // 该帧跳过三个 SDF dispatch（object 自己只报告一次）。
        const bool sdfDomainValid = object.HasTressfxSdfCollision() &&
            object.WriteTressfxSdfParams(state.skinningMatrix, frameSlot);
        state.simulationTime += timeStep;

        const uint32_t vertexGroupCount =
            (dispatch.pointCount + GuideSimulationWorkgroupSize - 1) /
            GuideSimulationWorkgroupSize;
        const uint32_t strandGroupCount =
            (dispatch.guideCount + GuideSimulationWorkgroupSize - 1) /
            GuideSimulationWorkgroupSize;

        // 严格按 TressFX 的 pass 顺序录制；VSP 关闭时完全不录制该 pass，
        // 与 TressFX 的 EnableVSP 开关对应。LocalShape 的 stiffness 为 0 时
        // shader 内早退，仍保留 dispatch 以维持固定 pass 表。
        RecordPass(commandBuffer, GuideSimulationPass::Integration,
            state.sets[PassIndex(GuideSimulationPass::Integration)][frameSlot],
            vertexGroupCount, anyDispatch);
        RecordPass(commandBuffer, GuideSimulationPass::StrandLevel,
            state.sets[PassIndex(GuideSimulationPass::StrandLevel)][frameSlot],
            strandGroupCount, anyDispatch);
        if (config.vspCoefficient > 0.0f)
            RecordPass(commandBuffer, GuideSimulationPass::Vsp,
                state.sets[PassIndex(GuideSimulationPass::Vsp)][frameSlot],
                vertexGroupCount, anyDispatch);
        RecordPass(commandBuffer, GuideSimulationPass::LocalShape,
            state.sets[PassIndex(GuideSimulationPass::LocalShape)][frameSlot],
            strandGroupCount, anyDispatch);
        RecordPass(commandBuffer, GuideSimulationPass::LengthWind,
            state.sets[PassIndex(GuideSimulationPass::LengthWind)][frameSlot],
            strandGroupCount, anyDispatch);
        // TressFX 5.0 guide shader 没有胶囊路径；这是 VL 在长度 pass 之后的追加 pass。
        if (params.colliderCount > 0)
            RecordPass(commandBuffer, GuideSimulationPass::Collision,
                state.sets[PassIndex(GuideSimulationPass::Collision)][frameSlot],
                strandGroupCount, anyDispatch);
        // 逐帧距离场：clear -> splat -> collide。三者必须相邻（同一对象的连续
        // dispatch），中间由 RecordPass 插入 compute→compute 内存 barrier。
        // 只在声明 collision.mesh、且本帧域参数有效时录制；capsule Collision
        // 可以同时存在。
        if (sdfDomainValid)
        {
            const uint32_t sdfCellGroupCount =
                (object.GetTressfxSdfCellCount() + GuideSimulationWorkgroupSize - 1) /
                GuideSimulationWorkgroupSize;
            const uint32_t sdfTriangleGroupCount =
                (object.GetTressfxSdfTriangleCount() + GuideSimulationWorkgroupSize - 1) /
                GuideSimulationWorkgroupSize;
            RecordPass(commandBuffer, GuideSimulationPass::SdfClear,
                state.sets[PassIndex(GuideSimulationPass::SdfClear)][frameSlot],
                sdfCellGroupCount, anyDispatch);
            RecordPass(commandBuffer, GuideSimulationPass::SdfSplat,
                state.sets[PassIndex(GuideSimulationPass::SdfSplat)][frameSlot],
                sdfTriangleGroupCount, anyDispatch);
            RecordPass(commandBuffer, GuideSimulationPass::SdfCollide,
                state.sets[PassIndex(GuideSimulationPass::SdfCollide)][frameSlot],
                strandGroupCount, anyDispatch);
        }
    }
    if (anyDispatch)
        RecordPostDispatchBarrier(commandBuffer);
}

void HairSimulationPass::ResetDescriptorCache() noexcept
{
    objectDescriptors.clear();
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
