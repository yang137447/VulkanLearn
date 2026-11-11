#pragma once

#include <array>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "animation/skinPose.h"
#include "hair/hairSimulationPass.h"
#include "hair/tressfxSourceSchedule.h"
#include "render/frontend/renderScene.h"
#include "renderableObject.h"
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
// 代理逐顶点蒙皮：CSR 影响流（静态转移表）+ 全骨架 bind→pose palette。
// palette 由 hair 对象自己按 frame-in-flight 持有：图形 VS 的 per-section
// palette 对未参与该 section 的关节没有有效 inverseBind，不能跨 section 使用。
constexpr uint32_t SdfProxyInfluenceBinding = 12;
constexpr uint32_t SdfPaletteBinding = 13;
// 多骨蒙皮：逐 strand 4×(joint,weight) + 全骨架 bind→pose palette。只被
// Integration/StrandLevel 声明；未声明 .tfxbone 时 buffer 全零、motion.w bit2=0。
constexpr uint32_t BoneWeightsBinding = 14;
constexpr uint32_t SkinningPaletteBinding = 15;
// 碰撞前穿透深度统计（x=胶囊、y=SDF，uint 位模式），只被两个 collide pass
// 原子写、trace 读取；未开 trace 时也清零，不影响渲染。
constexpr uint32_t PenetrationStatsBinding = 16;
// 分组求解：每组 8 float 的解析配置 + per-guide 组号（与图形 Set 2 binding 5
// 共用同一 buffer）。只有 Integration/StrandLevel/Vsp/LocalShape 声明。
constexpr uint32_t GroupSimulationConfigBinding = 17;
constexpr uint32_t GuideGroupIdBinding = 18;
// 路线 B：模拟 slot -> 原始 guide 索引；follow 插值使用后三个。
constexpr uint32_t SimulationGuideIndexBinding = 19;
constexpr uint32_t FollowGuideIndexBinding = 20;
constexpr uint32_t FollowOffsetBinding = 21;
constexpr uint32_t FollowFrameRefBinding = 22;

// 每个对象需要 GuideSimulationPassCount * GuideSimulationParamBufferCount
// 个 set（9 * 2 = 18），外加 2 个 reset/换基首轮 Integration set；按 64 预留，
// 池链只在对象数量溢出时增长。
constexpr uint32_t DescriptorSetsPerPool = 64;
// SdfSplat 是单个 set 的峰值：代理顶点/索引/距离网格/代理影响流/palette 共
// 5 个 SSBO（Collision 只有 current + colliders，SdfCollide 只有 current + grid）。
constexpr uint32_t StorageBuffersPerSet = 12;
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
    {BoneWeightsBinding, vk::DescriptorType::eStorageBuffer},
    {SkinningPaletteBinding, vk::DescriptorType::eStorageBuffer},
    {GroupSimulationConfigBinding, vk::DescriptorType::eStorageBuffer},
    {GuideGroupIdBinding, vk::DescriptorType::eStorageBuffer},
    {SimulationGuideIndexBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding StrandLevelBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {StrandLevelBinding, vk::DescriptorType::eStorageBuffer},
    {BoneWeightsBinding, vk::DescriptorType::eStorageBuffer},
    {SkinningPaletteBinding, vk::DescriptorType::eStorageBuffer},
    {GroupSimulationConfigBinding, vk::DescriptorType::eStorageBuffer},
    {GuideGroupIdBinding, vk::DescriptorType::eStorageBuffer},
    {SimulationGuideIndexBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding VspBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {StrandLevelBinding, vk::DescriptorType::eStorageBuffer},
    {GroupSimulationConfigBinding, vk::DescriptorType::eStorageBuffer},
    {GuideGroupIdBinding, vk::DescriptorType::eStorageBuffer},
    {SimulationGuideIndexBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding LocalShapeBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {RestPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {StrandLevelBinding, vk::DescriptorType::eStorageBuffer},
    {GroupSimulationConfigBinding, vk::DescriptorType::eStorageBuffer},
    {GuideGroupIdBinding, vk::DescriptorType::eStorageBuffer},
    {SimulationGuideIndexBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding LengthWindBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {RestLengthsBinding, vk::DescriptorType::eStorageBuffer},
    {SimulationGuideIndexBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding CollisionBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {CollidersBinding, vk::DescriptorType::eStorageBuffer},
    {PenetrationStatsBinding, vk::DescriptorType::eStorageBuffer},
    {SimulationGuideIndexBinding, vk::DescriptorType::eStorageBuffer},
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
    {SdfProxyInfluenceBinding, vk::DescriptorType::eStorageBuffer},
    {SdfPaletteBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding SdfCollideBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {SdfParamsBinding, vk::DescriptorType::eUniformBuffer},
    {SdfGridBinding, vk::DescriptorType::eStorageBuffer},
    {PenetrationStatsBinding, vk::DescriptorType::eStorageBuffer},
    {SimulationGuideIndexBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding FollowInterpolationBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {FollowGuideIndexBinding, vk::DescriptorType::eStorageBuffer},
    {FollowOffsetBinding, vk::DescriptorType::eStorageBuffer},
    {FollowFrameRefBinding, vk::DescriptorType::eStorageBuffer},
};
constexpr PassBinding SkinningBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {RestPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {BoneWeightsBinding, vk::DescriptorType::eStorageBuffer},
    {SkinningPaletteBinding, vk::DescriptorType::eStorageBuffer},
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
    {"hair/tressfxSimFollowInterpolation", FollowInterpolationBindings,
        static_cast<uint32_t>(std::size(FollowInterpolationBindings))},
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
constexpr bool PassUsesFollow(VL::Hair::GuideSimulationPass pass)
{
    return pass == VL::Hair::GuideSimulationPass::FollowInterpolation;
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

const VL::RenderDrawPacket* FindSkinningSourceDraw(
    const VL::Hair::TressfxSkinningBinding& skinning,
    const VL::RenderScene& renderScene)
{
    if (skinning.enabled)
    {
        for (const VL::RenderDrawPacket& packet : renderScene.drawPackets)
        {
            if (packet.sceneObjectIdentity == skinning.sourceSceneObject)
                return &packet;
        }
    }
    return nullptr;
}

bool HasRootSkinPose(const VL::RenderDrawPacket* sourceDraw, uint32_t rootJoint)
{
    if (!sourceDraw || !sourceDraw->skinPose)
        return false;
    const VL::Animation::SkinPose& pose = *sourceDraw->skinPose;
    const std::vector<Eigen::Matrix4f>& palette = pose.bindToPoseMatrices.empty()
        ? pose.matrices : pose.bindToPoseMatrices;
    return rootJoint < palette.size();
}

using SimulationOwnerMap =
    std::unordered_map<const RenderableObject*, VL::Hair::TressfxSimulationOwner>;

SimulationOwnerMap CollectSimulationOwners(
    const VL::ResolvedRenderScene& scene, const VL::RenderScene& renderScene)
{
    SimulationOwnerMap owners;
    for (const VL::ResolvedMaterialGroup& materialGroup : scene.materialGroups)
        for (const VL::ResolvedMaterialInstanceGroup& instanceGroup :
            materialGroup.materialInstances)
            for (const VL::ResolvedDrawPacket& draw : instanceGroup.draws)
            {
                const std::shared_ptr<RenderableObject> object = draw.renderableObject;
                // CPU/off 也保留正式 placement 身份，供实时命令的拒绝诊断使用。
                if (object && object->IsTressfx())
                    owners[object.get()].Observe(renderScene.drawPackets[
                        draw.drawPacketIndex].sceneObjectIdentity);
            }
    return owners;
}

// HasWork 与 Record 共用无副作用计划：timestamp 只包围实际 dispatch，
// 同一冻结帧的重复重绘不再次消费 GT dt。调度本身复刻源：n = f(frozenDelta)，
// n == 0 时本帧不录任何 pass，也不消费 reset/clock/history（reset 留到下一正 delta）。
struct SimulationFramePlan
{
    VL::Hair::GuideSubstepPlan substeps;
    VL::Hair::GuideSimulationFramePlan history;
};

SimulationFramePlan PlanSimulationFrame(
    const VL::Hair::GuideSimulationHistory& history,
    const VL::Hair::TressfxFrameClock& clock,
    const VL::RenderScene& renderScene,
    const VL::RenderDrawPacket* sourceDraw,
    bool hasPose,
    bool placementChanged)
{
    const float frozenDelta = sourceDraw ? sourceDraw->simulationDeltaSeconds
                                       : renderScene.hairSimulationDeltaSeconds;
    // 已消费的帧返回 0：本帧按源调度就是 n = 0，不重复步进。
    const float frameDelta = clock.DeltaForFrame(
        renderScene.worldGeneration, renderScene.frameIndex, frozenDelta);
    const VL::Hair::TressfxSourceSchedule schedule =
        VL::Hair::TressfxSourceScheduleForDelta(frameDelta);
    SimulationFramePlan plan;
    // history.Plan 只把“本帧是否推进”作为 > 0 的判据；reset 原因由调用方在 n > 0
    // 时消费。n == 0 时计划函数不改任何状态，下一帧会再次返回同一个 reset 原因。
    plan.history = history.Plan(hasPose,
        hasPose ? sourceDraw->skinPose->playbackIdentity : 0,
        hasPose ? sourceDraw->skinPose->discontinuity : 0,
        schedule.framePhysicalSeconds);
    plan.substeps = VL::Hair::PlanGuideSubsteps(frameDelta,
        plan.history.action == VL::Hair::GuideSimulationFrameAction::Reset,
        placementChanged);
    return plan;
}
} // namespace