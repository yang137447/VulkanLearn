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

// 每个对象需要 GuideSimulationPassCount * GuideSimulationParamBufferCount
// 个 set（9 * 2 = 18），外加 2 个 reset/换基首轮 Integration set；按 64 预留，
// 池链只在对象数量溢出时增长。
constexpr uint32_t DescriptorSetsPerPool = 64;
// SdfSplat 是单个 set 的峰值：代理顶点/索引/距离网格/代理影响流/palette 共
// 5 个 SSBO（Collision 只有 current + colliders，SdfCollide 只有 current + grid）。
constexpr uint32_t StorageBuffersPerSet = 8;
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
};
constexpr PassBinding StrandLevelBindings[] = {
    {ParamsBinding, vk::DescriptorType::eUniformBuffer},
    {CurrentPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {PreviousPreviousPositionsBinding, vk::DescriptorType::eStorageBuffer},
    {StrandLevelBinding, vk::DescriptorType::eStorageBuffer},
    {BoneWeightsBinding, vk::DescriptorType::eStorageBuffer},
    {SkinningPaletteBinding, vk::DescriptorType::eStorageBuffer},
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
    {PenetrationStatsBinding, vk::DescriptorType::eStorageBuffer},
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
                const std::shared_ptr<RenderableObject> object = draw.renderableObject.lock();
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

namespace VL::Hair
{
HairSimulationPass::~HairSimulationPass()
{
    // trace 资源必须在 backend 仍有效时释放。正常退出路径已由
    // RenderSystem::ShutdownRenderObject 调用 FinishTrace(true)；这里只处理
    // World/graph 重建退休后的兜底收尾：退休队列保证 GPU 已完成，残留 slot
    // 可以在不额外 WaitIdle 的情况下安全写出。
    FinishTraceInternal(false);
    ResetDescriptorCache();
    for (std::shared_ptr<ComputePipeline>& pipeline : pipelines)
        pipeline.reset();
    skinningPipeline.reset();
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
    skinningPipeline = factory.CreateComputePipelineCandidate("hair/tressfxSkinning");
    initialized = true;
}

void HairSimulationPass::BeginTrace(const HairTraceRequest& request)
{
    if (!initialized)
    {
        std::cerr << "[TressFX] Hair trace rejected: simulation pass is not initialized"
                  << std::endl;
        return;
    }
    if (trace.active)
    {
        std::cerr << "[TressFX] Hair trace rejected: a trace is already active ("
                  << trace.request.path << ")" << std::endl;
        return;
    }
    if (request.path.empty() || request.frames == 0 || request.guideStride == 0)
    {
        std::cerr << "[TressFX] Hair trace rejected: path must be non-empty and "
                     "frames/stride must be positive" << std::endl;
        return;
    }
    // 只保存请求；对象选择、guide 采样与 buffer 分配都推迟到 Record 见到
    // 第一个 GPU hair dispatch 时，避免在 Console/GT 线程触碰 GPU。
    trace = TraceState{};
    trace.active = true;
    trace.request = request;
    trace.requestedFrames = request.frames;
}

void HairSimulationPass::FinishTrace(bool waitForGpu)
{
    FinishTraceInternal(waitForGpu);
}

void HairSimulationPass::RequestReset(std::string objectName)
{
    // 渲染线程调用（RecordHairSimulation 内、Record 之前）；只保存值语义，
    // 下一帧 Record 再展开成逐对象标记。重复请求覆盖同一槽，不排队多次复位。
    pendingResetObject = std::move(objectName);
}

void HairSimulationPass::RequestLiveParameter(VL::Hair::HairLiveParameterRequest request)
{
    // 渲染线程调用；只入队值语义。上限防止无渲染帧时无限累积；超限先丢最旧意图，
    // 与 RenderSystem 的 GT 侧有界队列保持同一策略。
    if (pendingLiveParameters.size() >= 64)
        pendingLiveParameters.erase(pendingLiveParameters.begin());
    pendingLiveParameters.push_back(std::move(request));
}

void HairSimulationPass::StartTraceObject(RenderableObject& object)
{
    const uint32_t guideCount = object.GetTressfxGuideCount();
    const uint32_t pointsPerGuide = object.GetTressfxPointsPerGuide();
    if (guideCount == 0 || pointsPerGuide < 2)
    {
        std::cerr << "[TressFX] Hair trace rejected: object \"" << object.GetName()
                  << "\" has no GPU guide geometry" << std::endl;
        trace = TraceState{};
        return;
    }

    const uint32_t stride = trace.request.guideStride;
    const uint64_t selected64 =
        (static_cast<uint64_t>(guideCount) + stride - 1) / stride;
    const uint32_t selected = static_cast<uint32_t>(
        std::min<uint64_t>(selected64, HairTraceMaxSelectedGuides));
    if (selected == 0)
    {
        trace = TraceState{};
        return;
    }

    try
    {
        trace.object = &object;
        trace.objectName = object.GetName();
        trace.totalGuideCount = guideCount;
        trace.pointsPerGuide = pointsPerGuide;
        trace.selectedCount = selected;
        trace.selectedGuides.resize(selected);
        for (uint32_t index = 0; index < selected; ++index)
            trace.selectedGuides[index] = index * stride;

        // 选中 guide 的 rest length 只拷一次：源 buffer 是 device-local，无法直接
        // 主机读取。它放在独立 host-visible buffer，第一次 flush 时再读出来。
        trace.restBytes = static_cast<vk::DeviceSize>(selected) * pointsPerGuide *
            sizeof(float);
        std::tie(trace.restBuffer, trace.restMemory) = backend->CreateBuffer(
            trace.restBytes,
            vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "HairTraceRestLengths");
        trace.restMapped = backend->MapMemory(trace.restMemory, trace.restBytes);

        const vk::DeviceSize blockBytes = static_cast<vk::DeviceSize>(selected) *
            pointsPerGuide * sizeof(Eigen::Vector4f);
        for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
        {
            TraceSlot& target = trace.slots[slot];
            target.size = blockBytes * 3u + 2u * sizeof(float);
            std::tie(target.buffer, target.memory) = backend->CreateBuffer(
                target.size,
                vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                "HairTraceReadback:" + std::to_string(slot));
            target.mapped = backend->MapMemory(target.memory, target.size);
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "[TressFX] Hair trace rejected: " << error.what() << std::endl;
        DestroyTraceResources();
        trace = TraceState{};
        return;
    }

    std::cout << "[TressFX] Hair trace start: file=\"" << trace.request.path
              << "\" object=\"" << trace.objectName << "\" guideCount=" << guideCount
              << " selectedGuides=" << selected << " pointsPerGuide=" << pointsPerGuide
              << " stride=" << stride << " requestedFrames=" << trace.request.frames
              << " bytesPerFrame=" << (trace.slots[0].size + trace.restBytes)
              << std::endl;
}

bool HairSimulationPass::WriteTraceHeaderFromRestLengths()
{
    if (!trace.restMapped)
        return false;
    const size_t count = static_cast<size_t>(trace.selectedCount) * trace.pointsPerGuide;
    trace.restLengths.resize(count);
    const float* source = static_cast<const float*>(trace.restMapped);
    for (size_t index = 0; index < count; ++index)
    {
        const float value = source[index];
        if (!std::isfinite(value) || value < 0.0f)
        {
            std::cerr << "[TressFX] Hair trace aborted: rest length " << index
                      << " is not finite/non-negative" << std::endl;
            return false;
        }
        trace.restLengths[index] = value;
    }

    HairTraceHeader header;
    header.version = HairTraceVersion;
    header.totalGuideCount = trace.totalGuideCount;
    header.pointsPerGuide = trace.pointsPerGuide;
    header.stride = trace.request.guideStride;
    header.selectedCount = trace.selectedCount;
    header.startFrame = trace.hasStartFrame ? trace.startFrame : 0;
    header.selectedGuides = trace.selectedGuides;
    header.restLengths = trace.restLengths;

    std::string error;
    trace.writer = std::make_unique<HairTraceWriter>();
    if (!trace.writer->Open(trace.request.path, header, error))
    {
        std::cerr << "[TressFX] Hair trace aborted: " << error << std::endl;
        trace.writer.reset();
        return false;
    }
    trace.writerOpen = true;
    return true;
}

void HairSimulationPass::FlushTraceSlot(uint32_t frameSlot)
{
    TraceSlot& slot = trace.slots[frameSlot];
    if (!slot.pending)
        return;
    if (trace.writeFailed)
    {
        // 文件已经失败后不再尝试续写；剩余 slot 只清 pending，等 FinishTrace 收尾。
        slot.pending = false;
        trace.request.frames = static_cast<uint32_t>(trace.capturedFrames);
        trace.finishing = true;
        return;
    }
    if (!trace.writerOpen && !WriteTraceHeaderFromRestLengths())
    {
        // header 写不出去时立刻停止后续拷贝；已录制的 slot 不再尝试。
        trace.writeFailed = true;
        trace.finishing = true;
        trace.request.frames = static_cast<uint32_t>(trace.capturedFrames);
        slot.pending = false;
        return;
    }

    const size_t blockFloats =
        static_cast<size_t>(trace.selectedCount) * trace.pointsPerGuide * 4;
    const float* base = static_cast<const float*>(slot.mapped);
    const float* current = base;
    const float* previous = base + blockFloats;
    const float* previousPrevious = base + blockFloats * 2;
    // 末尾 2 个 float 是碰撞前 SDF/胶囊最大内部穿透深度。
    slot.info.sdfPenetrationMaxCm = base[blockFloats * 3 + 0];
    slot.info.capsulePenetrationMaxCm = base[blockFloats * 3 + 1];
    std::string error;
    if (!trace.writer->WriteFrame(
            slot.info, current, previous, previousPrevious, error))
    {
        std::cerr << "[TressFX] Hair trace aborted: " << error << std::endl;
        std::string closeError;
        trace.writer->Close(closeError);
        trace.writerOpen = false;
        trace.writeFailed = true;
        trace.finishing = true;
        trace.request.frames = static_cast<uint32_t>(trace.capturedFrames);
        slot.pending = false;
        return;
    }
    slot.pending = false;
    ++trace.flushedFrames;
    trace.bytesWritten = trace.writer->BytesWritten();
}

void HairSimulationPass::RecordTraceCopies(vk::CommandBuffer commandBuffer,
    uint32_t frameSlot,
    const HairTraceFrameInfo& info)
{
    if (!trace.active || trace.finishing || trace.object == nullptr)
        return;
    TraceSlot& slot = trace.slots[frameSlot];
    if (!slot.buffer || !trace.restBuffer)
        return;

    const vk::DeviceSize guideBytes = static_cast<vk::DeviceSize>(trace.pointsPerGuide) *
        sizeof(Eigen::Vector4f);
    const vk::DeviceSize blockBytes =
        static_cast<vk::DeviceSize>(trace.selectedCount) * guideBytes;
    const vk::Buffer sources[3] = {
        trace.object->GetTressfxPositionInfo().buffer,
        trace.object->GetTressfxPreviousPositionInfo().buffer,
        trace.object->GetTressfxPreviousPreviousPositionInfo().buffer,
    };

    // 所有模拟 dispatch（含 SDF collide）写完后，compute 的写入必须对 transfer
    // 读可见；trace 只读，不改任何求解状态。
    vk::MemoryBarrier toTransfer;
    toTransfer.setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eTransfer, {}, toTransfer, nullptr, nullptr);

    std::vector<vk::BufferCopy> regions;
    regions.reserve(static_cast<size_t>(trace.selectedCount) * 3);
    for (uint32_t state = 0; state < 3; ++state)
    {
        regions.clear();
        for (uint32_t index = 0; index < trace.selectedCount; ++index)
        {
            const vk::DeviceSize sourceOffset =
                static_cast<vk::DeviceSize>(trace.selectedGuides[index]) * guideBytes;
            const vk::DeviceSize destinationOffset =
                static_cast<vk::DeviceSize>(state) * blockBytes +
                static_cast<vk::DeviceSize>(index) * guideBytes;
            regions.push_back(
                {sourceOffset, destinationOffset, guideBytes});
        }
        commandBuffer.copyBuffer(sources[state], slot.buffer, regions);
    }

    // 末尾追加 8 B：本帧 collision/SDF collide 记录的穿透统计。统计 buffer
    // 是 host-coherent，但 trace 读的是这里的 GPU→host 拷贝，避免槽复用覆盖。
    const vk::BufferCopy statsCopy{0, blockBytes * 3u, 2u * sizeof(float)};
    commandBuffer.copyBuffer(
        trace.object->GetTressfxPenetrationStatsInfo(frameSlot).buffer,
        slot.buffer, statsCopy);

    // rest length 只拷一次，与首帧的 current/previous/prevPrev 共用同一组 barrier。
    if (!trace.restRecorded)
    {
        const vk::DeviceSize restGuideBytes =
            static_cast<vk::DeviceSize>(trace.pointsPerGuide) * sizeof(float);
        regions.clear();
        for (uint32_t index = 0; index < trace.selectedCount; ++index)
        {
            const vk::DeviceSize sourceOffset =
                static_cast<vk::DeviceSize>(trace.selectedGuides[index]) * restGuideBytes;
            const vk::DeviceSize destinationOffset =
                static_cast<vk::DeviceSize>(index) * restGuideBytes;
            regions.push_back({sourceOffset, destinationOffset, restGuideBytes});
        }
        commandBuffer.copyBuffer(trace.object->GetTressfxRestLengthInfo().buffer,
            trace.restBuffer, regions);
        trace.restRecorded = true;
    }

    // transfer 的读（源位置）和写（readback buffer）必须在下一次 compute 写入
    // 位置 buffer 之前完成；dst 同时覆盖下一帧的 compute 读写和本帧稍后的 VS 读。
    vk::MemoryBarrier toCompute;
    toCompute.setSrcAccessMask(
            vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(
            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader, {}, toCompute, nullptr, nullptr);

    slot.pending = true;
    slot.info = info;
    if (!trace.hasStartFrame)
    {
        trace.startFrame = info.frameIndex;
        trace.hasStartFrame = true;
    }
    ++trace.capturedFrames;
    if (trace.capturedFrames >= trace.request.frames)
        trace.finishing = true;
}

void HairSimulationPass::TryFinishTrace()
{
    if (!trace.active || !trace.finishing)
        return;
    if (trace.capturedFrames < trace.request.frames)
        return;
    for (const TraceSlot& slot : trace.slots)
    {
        if (slot.pending)
            return;
    }
    FinishTraceInternal(false);
}

void HairSimulationPass::DestroyTraceResources()
{
    if (backend)
    {
        for (TraceSlot& slot : trace.slots)
        {
            if (slot.mapped && slot.memory)
                backend->UnmapMemory(slot.memory);
            if (slot.buffer || slot.memory)
                backend->DestroyBuffer(slot.buffer, slot.memory);
            slot = TraceSlot{};
        }
        if (trace.restMapped && trace.restMemory)
            backend->UnmapMemory(trace.restMemory);
        if (trace.restBuffer || trace.restMemory)
            backend->DestroyBuffer(trace.restBuffer, trace.restMemory);
    }
    trace.restBuffer = nullptr;
    trace.restMemory = nullptr;
    trace.restMapped = nullptr;
    trace.restBytes = 0;
}

void HairSimulationPass::FinishTraceInternal(bool waitForGpu)
{
    if (!trace.active)
        return;
    if (waitForGpu && backend)
    {
        // 仅正常退出/显式结束时使用：BeginFrame 只等当前 slot 的 fence，最后
        // 1..2 个 slot 可能仍 pending；一次 queue idle 后 map 才是安全的。
        backend->WaitIdle();
    }
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
        FlushTraceSlot(slot);
    if (trace.writerOpen && trace.writer)
    {
        std::string error;
        if (!trace.writer->Close(error))
            std::cerr << "[TressFX] " << error << std::endl;
        trace.writerOpen = false;
    }
    if (trace.flushedFrames > 0 && trace.writer && !trace.writeFailed)
    {
        std::cout << "[TressFX] Hair trace complete: file=\"" << trace.request.path
                  << "\" frames=" << trace.flushedFrames
                  << " requestedFrames=" << trace.requestedFrames
                  << " selectedGuides=" << trace.selectedCount
                  << " pointsPerGuide=" << trace.pointsPerGuide
                  << " stride=" << trace.request.guideStride
                  << " bytes=" << trace.writer->BytesWritten() << std::endl;
    }
    else if (trace.writeFailed)
    {
        std::cerr << "[TressFX] Hair trace failed after " << trace.flushedFrames
                  << " frames: " << trace.request.path << std::endl;
    }
    else if (trace.object != nullptr)
    {
        std::cerr << "[TressFX] Hair trace finished with no captured frames: object=\""
                  << trace.objectName << "\"" << std::endl;
    }
    trace.writer.reset();
    DestroyTraceResources();
    trace = TraceState{};
}

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
    std::array<std::array<vk::DescriptorBufferInfo, PenetrationStatsBinding + 1>,
        GuideSimulationParamBufferCount>
        bindingInfos{};
    // reset 首轮 Integration 的 binding 0 换成独立参数块，其余 binding 与正常
    // Integration 相同；同样按 slot 保持地址稳定。
    std::array<vk::DescriptorBufferInfo, GuideSimulationParamBufferCount>
        resetParamInfos{};
    for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
    {
        resetParamInfos[slot] = object.GetTressfxResetParamsInfo(slot);
        std::array<vk::DescriptorBufferInfo, PenetrationStatsBinding + 1>& infos =
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
    }
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
                object.GetTressfxSdfParamsInfo(slot).buffer;
    }
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
                const std::shared_ptr<RenderableObject> object = draw.renderableObject.lock();
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

bool HairSimulationPass::HasWork(const VL::ResolvedRenderScene& scene,
    const VL::RenderScene& renderScene) const
{
    if (!initialized || !pipelines[0])
        return false;

    const SimulationOwnerMap owners = CollectSimulationOwners(scene, renderScene);
    for (const ResolvedMaterialGroup& materialGroup : scene.materialGroups)
    {
        for (const ResolvedMaterialInstanceGroup& instanceGroup : materialGroup.materialInstances)
        {
            for (const ResolvedDrawPacket& draw : instanceGroup.draws)
            {
                const std::shared_ptr<RenderableObject> object = draw.renderableObject.lock();
                if (object && object->UsesGpuHairSkinning())
                {
                    const TressfxSkinningBinding& skinning =
                        object->GetTressfxSkinningBinding();
                    if (!skinning.enabled || !HasRootSkinPose(
                        FindSkinningSourceDraw(skinning, renderScene),
                        skinning.jointIndex))
                        continue;
                    const auto cached = skinningDescriptors.find(object.get());
                    TressfxSimulationOwner owner = cached == skinningDescriptors.end()
                        ? TressfxSimulationOwner{} : cached->second.simulationOwner;
                    owner.Merge(owners.at(object.get()));
                    if (owner.HasConflict())
                    {
                        if (cached == skinningDescriptors.end() ||
                            !cached->second.reportedOwnerConflict)
                            return true;
                        continue;
                    }
                    // 蒙皮没有 dt 门槛；暂停或编辑预览的 0dt 姿态也要应用到整根发丝。
                    return true;
                }
                if (!object || !object->UsesGpuGuideSimulation())
                    continue;
                const auto cached = objectDescriptors.find(object.get());
                TressfxSimulationOwner owner = cached == objectDescriptors.end()
                    ? TressfxSimulationOwner{} : cached->second.simulationOwner;
                owner.Merge(owners.at(object.get()));
                if (owner.HasConflict())
                {
                    if (cached == objectDescriptors.end() ||
                        !cached->second.reportedOwnerConflict)
                        return true;
                    continue;
                }
                // 源调度：n = 0 的帧既不 dispatch 也不消费 reset；已有 reset 事件
                // 要留到下一个正 delta 帧，所以这里和 Record 的判断必须一致。
                const TressfxSkinningBinding& skinning =
                    object->GetTressfxSkinningBinding();
                const VL::RenderDrawPacket* sourceDraw =
                    FindSkinningSourceDraw(skinning, renderScene);
                const TressfxFrameClock fallbackClock;
                const TressfxFrameClock& clock = cached == objectDescriptors.end()
                    ? fallbackClock : cached->second.clock;
                const float frozenDelta = sourceDraw
                    ? sourceDraw->simulationDeltaSeconds
                    : renderScene.hairSimulationDeltaSeconds;
                const float frameDelta = clock.DeltaForFrame(
                    renderScene.worldGeneration, renderScene.frameIndex,
                    frozenDelta);
                if (TressfxSourceScheduleForDelta(frameDelta).substeps == 0)
                    continue;
                // n > 0 时该对象一定会录 reset 或正常步进（descriptor 失效只是
                // 首次创建），因此不再需要额外的 placement/历史判断。
                return true;
            }
        }
    }
    return false;
}

void HairSimulationPass::Record(vk::CommandBuffer commandBuffer,
    const VL::ResolvedRenderScene& scene,
    const VL::RenderScene& renderScene,
    uint32_t frameSlot)
{
    if (!initialized || !pipelines[0])
        return;
    // frameSlot 直接索引 params UBO；调用方必须传 frame-in-flight 槽（0..1）。
    if (frameSlot >= GuideSimulationParamBufferCount)
        throw std::out_of_range("TressFX guide simulation frame slot is out of range");

    // 与 solver 共用同一帧入口和图形读取 barrier，但不消费模拟时钟/历史/trace。
    RecordSkinning(commandBuffer, scene, renderScene, frameSlot);

    // BeginFrame 已经等过本槽的 fence，上一批同槽拷贝必然完成；先写文件并清
    // pending，再录制本帧新的拷贝，避免覆盖尚未读回的数据。
    if (trace.active)
    {
        FlushTraceSlot(frameSlot);
        TryFinishTrace();
    }

    struct Dispatch
    {
        std::shared_ptr<RenderableObject> object;
        uint32_t guideCount = 0;
        uint32_t pointCount = 0;
        size_t drawPacketIndex = 0;
        bool ownerConflict = false;
    };
    std::vector<Dispatch> dispatches;
    std::unordered_set<const RenderableObject*> seen;
    const SimulationOwnerMap owners = CollectSimulationOwners(scene, renderScene);
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
                    guideCount * pointsPerGuide, draw.drawPacketIndex, false});
            }
        }
    }
    // 实时参数覆盖：按冻结 draw 的 placement 身份匹配本帧的 GPU guide dispatch，
    // 不使用含有效配置的资源缓存名；写该渲染对象自己的
    // config 副本；下一段逐对象组 params 时就会读到新值。失败只报告并丢弃，
    // 不影响本帧已经规划的模拟步进。
    std::vector<VL::Hair::HairLiveParameterRequest> liveParameters;
    liveParameters.swap(pendingLiveParameters);
    for (const VL::Hair::HairLiveParameterRequest& request : liveParameters)
    {
        RenderableObject* target = nullptr;
        for (const Dispatch& dispatch : dispatches)
            if (owners.at(dispatch.object.get()).MatchesCommandTarget(request.objectName))
            {
                target = dispatch.object.get();
                break;
            }
        if (target == nullptr)
        {
            // 区分“对象不存在”和“对象存在但走 CPU backend”：后者不能被当成
            // 成功，否则作者会以为参数已经作用在 CPU 参考 solver 上。
            bool cpuBackendObject = false;
            for (const ResolvedMaterialGroup& materialGroup : scene.materialGroups)
                for (const ResolvedMaterialInstanceGroup& instanceGroup : materialGroup.materialInstances)
                    for (const ResolvedDrawPacket& draw : instanceGroup.draws)
                    {
                        const std::shared_ptr<RenderableObject> object =
                            draw.renderableObject.lock();
                        if (object && object->IsTressfx() &&
                            owners.at(object.get()).MatchesCommandTarget(request.objectName) &&
                            !object->UsesGpuGuideSimulation())
                            cpuBackendObject = true;
                    }
            std::cerr << "[TressFX] live param: object \"" << request.objectName << "\" "
                      << (cpuBackendObject
                              ? "uses the CPU backend; live parameters require GPU"
                              : "not found among GPU guide objects");
            if (!cpuBackendObject && !dispatches.empty())
            {
                // 名字是作者手写的，失败时列出本帧可用的 GPU 对象名，省一次猜。
                std::cerr << " (available:";
                for (size_t index = 0; index < dispatches.size() && index < 8; ++index)
                    std::cerr << (index == 0 ? " " : ", ")
                              << owners.at(dispatches[index].object.get()).OwnerIdentity();
                if (dispatches.size() > 8) std::cerr << ", ...";
                std::cerr << ")";
            }
            std::cerr << std::endl;
            continue;
        }
        std::string error;
        if (!target->ApplyTressfxLiveParameter(request, error))
        {
            std::cerr << "[TressFX] live param rejected: object=\"" << request.objectName
                      << "\" field=" << request.field << " reason=" << error << std::endl;
            continue;
        }
        std::cout << "[TressFX] live param: object=" << request.objectName
                  << " field=" << request.field << " value=";
        for (uint32_t index = 0; index < request.valueCount; ++index)
            std::cout << (index == 0 ? "" : ",") << request.values[index];
        std::cout << std::endl;
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

    // trace 只锁第一个 GPU hair 对象。对象在后续帧消失（World/graph 重建）时
    // 停止新拷贝，让已经录制的 slot 仍然写出，而不是留下悬空指针。
    if (trace.active)
    {
        if (trace.object == nullptr)
        {
            StartTraceObject(*dispatches.front().object);
        }
        else if (!trace.finishing)
        {
            bool present = false;
            for (const Dispatch& dispatch : dispatches)
            {
                if (dispatch.object.get() == trace.object)
                {
                    present = true;
                    break;
                }
            }
            if (!present)
            {
                if (!trace.reportedObjectLoss)
                {
                    std::cerr << "[TressFX] Hair trace stopped: object \""
                              << trace.objectName << "\" is no longer dispatched"
                              << std::endl;
                    trace.reportedObjectLoss = true;
                }
                trace.finishing = true;
                trace.request.frames = static_cast<uint32_t>(trace.capturedFrames);
            }
        }
    }

    RecordPreDispatchBarrier(commandBuffer);
    bool anyDispatch = false;
    bool captureTrace = false;
    bool resetMatched = false;
    HairTraceFrameInfo traceInfo;
    // 场景风是全局对象：每个对象组只读一次，整组 dispatch 共用同一个风向量。
    // SceneWindRuntime 已完成缓动与阵风调制，头发不再叠加 TressFX 的
    // sin(frame*0.001)² 逐帧调制（见 tressfxSimulationParams.h 的偏差说明）。
    const Eigen::Vector3f sceneWindVelocityCmPerSec =
        VL::Hair::TressfxSceneWindVelocityCmPerSec(
            renderScene.resolvedWind);
    for (Dispatch& dispatch : dispatches)
    {
        RenderableObject& object = *dispatch.object;
        ObjectDescriptors& state = EnsureObjectDescriptors(object);
        // 显式复位请求按 placement 身份展开；空名表示所有 GPU guide 对象。对象暂停/0 步
        // 帧时标记留在 ObjectDescriptors 上，下一个正 delta 帧才消费。
        if (pendingResetObject &&
            owners.at(&object).MatchesCommandTarget(*pendingResetObject, true))
        {
            state.resetRequested = true;
            resetMatched = true;
        }
        object.ResetTressfxPenetrationStats(frameSlot);
        state.simulationOwner.Merge(owners.at(&object));
        if (state.simulationOwner.HasConflict())
        {
            if (!state.reportedOwnerConflict)
            {
                std::cerr << "[TressFX] GPU simulation disabled for shared "
                          "RenderableObject \"" << object.GetName()
                          << "\": multiple sceneObjectIdentity values share "
                          "mutable guide buffers; retaining last positions" << std::endl;
                state.reportedOwnerConflict = true;
            }
            continue;
        }
        const TressfxSimulationConfig& config = object.GetTressfxSimulationConfig();

        // 根点骨骼矩阵来自同一帧的 RenderScene 冻结快照。只按场景对象身份匹配，
        // 不复制播放器或骨架状态；pose 缺失时保留上一帧矩阵并只报告一次。
        const VL::Hair::TressfxSkinningBinding& skinning =
            object.GetTressfxSkinningBinding();
        const VL::RenderDrawPacket* sourceDraw =
            FindSkinningSourceDraw(skinning, renderScene);
        const bool hasPose = HasRootSkinPose(sourceDraw, skinning.jointIndex);
        const VL::RenderDrawPacket& hairDraw =
            renderScene.drawPackets[dispatch.drawPacketIndex];
        // 根点/胶囊/代理都用全骨架 bind→pose（SkinPose::bindToPoseMatrices）：
        // 图形 VS 的 per-section palette 对“未参与该 section 的关节”保留单位
        // inverseBind，其矩阵会退化为绝对 model 变换，不能用于跨 section 的
        // 代理权重。只有旧资产没有这份全局矩阵时才退回 per-section matrices。
        if (hasPose)
        {
            UpdateTressfxFrameContext(state.frameContext,
                hairDraw.model, sourceDraw->model, *sourceDraw->skinPose,
                skinning.jointIndex, sourceDraw->simulationDeltaSeconds);
        }
        else if (skinning.enabled && !state.reportedMissingSkinPose)
        {
            std::cerr << "[TressFX] skinning pose unavailable for object \""
                      << object.GetName() << "\" source=\""
                      << skinning.sourceSceneObject << "\" bone=\""
                      << skinning.rootBone << "\"; keeping the last matrix"
                      << std::endl;
            state.reportedMissingSkinPose = true;
        }
        const bool placementChanged =
            state.modelHistory.HasPlacementChange(hairDraw.model);
        // 源调度计划：n 由本帧冻结 delta 决定；reset 只在 n > 0 的帧生效。
        SimulationFramePlan plan = PlanSimulationFrame(
            state.history, state.clock, renderScene, sourceDraw, hasPose,
            placementChanged);
        if (!plan.substeps.HasWork())
        {
            // 0 步帧不录 pass，也不提交 reset/clock/history/modelHistory；
            // 缺失 pose 的观察同样留到有实际步进的帧。
            continue;
        }
        // 显式复位：只在该帧确实有 substep 时消费（上面的 0 步分支保留标记）。
        // reset 帧三份历史直接写回 InitialPos，换基矩阵没有意义，因此与 rebase 互斥。
        if (state.resetRequested)
        {
            plan.substeps.resetFirstSubstep = true;
            plan.substeps.rebaseFirstSubstep = false;
            plan.history.action = VL::Hair::GuideSimulationFrameAction::Reset;
            plan.history.reason = VL::Hair::GuideSimulationResetReason::Requested;
            state.resetRequested = false;
        }
        if (skinning.enabled && !hasPose)
            state.history.ObserveMissingPose();
        const bool resetPositions = plan.substeps.resetFirstSubstep;
        const bool traceThisObject = trace.active && !trace.finishing &&
            trace.object == &object && trace.objectName == object.GetName();
        if (traceThisObject)
        {
            traceInfo.frameIndex = renderScene.frameIndex;
            traceInfo.worldGeneration = renderScene.worldGeneration;
            traceInfo.substeps = plan.substeps.schedule.substeps;
            traceInfo.timeStep = plan.substeps.schedule.stepSeconds;
            traceInfo.resetReason = resetPositions
                ? static_cast<uint32_t>(plan.history.reason) : 0u;
            traceInfo.dispatchCount = 0;
            traceInfo.hairModel = hairDraw.model;
            captureTrace = true;
        }
        // reset 帧三份历史直接写回 InitialPos，换基矩阵没有意义；正常帧只在
        // 首个 substep 应用一次，后续 substep 的历史已在当前局部空间。
        const Eigen::Matrix4f historyToCurrentLocal =
            plan.substeps.rebaseFirstSubstep
            ? state.modelHistory.TransformToCurrentLocal(hairDraw.model)
            : Eigen::Matrix4f::Identity();
        if (!hasPose && skinning.enabled && placementChanged)
        {
            // 丢 pose 时保留的是上次世界姿态；hair 局部坐标变化也要一起换基，
            // 否则缓存根点/代理会被当前 model 重复带走。
            state.frameContext.skinningMatrix =
                historyToCurrentLocal * state.frameContext.skinningMatrix;
            for (Eigen::Matrix4f& joint : state.frameContext.jointMatrices)
                joint = historyToCurrentLocal * joint;
        }
        const std::vector<Eigen::Matrix4f>* jointMatrices =
            state.frameContext.jointMatrices.empty()
                ? nullptr : &state.frameContext.jointMatrices;
        if (jointMatrices)
        {
            // 胶囊骨骼与根骨共用同一个 pose：每个绑定胶囊按自己的关节矩阵变换
            // a/b，未绑定槽保留作者绑定空间的静态值。radius 不缩放（见
            // TransformTressfxCapsuleByBone 的注释）。reset 帧也要写：源 reset
            // 之后同一 substep 仍会跑后续约束/风/碰撞。
            if (!skinning.capsuleJointIndices.empty())
            {
                bool anyBoundCapsule = false;
                for (int32_t joint : skinning.capsuleJointIndices)
                    anyBoundCapsule |= joint >= 0;
                if (anyBoundCapsule)
                {
                    object.WriteTressfxColliders(
                        VL::Hair::TransformTressfxCapsulesByBones(
                            config.capsules, skinning.capsuleJointIndices,
                            *jointMatrices),
                        frameSlot);
                }
            }
            // SDF splat 的 palette 是 hair 对象自己的逐帧 buffer（全局
            // bind→pose）；只在对象声明了 collision.mesh + 权重转移时存在。
            object.WriteTressfxSdfProxyPalette(*jointMatrices, frameSlot);
        }
        // 多骨：每周期的全骨架 bind→pose palette 写入对象自己的槽；没有
        // .tfxbone 时权重 buffer 全零、以下 bit2 不置位。
        if (jointMatrices)
            object.WriteTressfxSkinningPalette(*jointMatrices, frameSlot);
        // 正常参数块：dt 恒为源 solver step 0.0125，motion.y = 0；同一帧所有
        // substep 共用这一份。风向量已含场景阵风，不再乘仿真帧调制。
        GuideSimulationParams params = MakeGuideSimulationParams(
            config,
            sceneWindVelocityCmPerSec,
            dispatch.guideCount,
            object.GetTressfxPointsPerGuide(),
            dispatch.pointCount,
            TressfxSourceSolverStepSeconds,
            state.frameContext.skinningMatrix,
            false,
            Eigen::Matrix4f::Identity(),
            false,
            hairDraw.model);
        // motion.w 的 bit2：Integration/StrandLevel 是否读取逐 strand 多骨权重。
        // 没有 pose/palette 的帧回退单根骨矩阵，避免读到全零 palette。
        if (!skinning.strandBones.empty() && jointMatrices && !jointMatrices->empty())
            params.motion.w() += 4.0f;
        object.WriteTressfxSimulationParams(params, frameSlot);
        // reset 首轮 / 换基首轮 Integration 使用独立参数块。正常 params 永远
        // motion.y = 0，避免同一 UBO 在命令执行时只剩最后一次写入。
        if (plan.substeps.FirstSubstepUsesSeparateParams())
        {
            GuideSimulationParams firstSubstepParams = params;
            if (resetPositions)
            {
                // 源 reset 在读取 dt 之前就写满三份历史并 return。
                firstSubstepParams.controls.x() = 0.0f;
                firstSubstepParams.motion.y() = 1.0f;
            }
            else
            {
                firstSubstepParams.historyToCurrentLocal = historyToCurrentLocal;
            }
            object.WriteTressfxResetParams(firstSubstepParams, frameSlot);
        }
        // SDF 域每帧从蒙皮后的代理包围盒重算；有逐顶点权重时用同一套 LBS，
        // 否则回退单骨根矩阵。非有限包围盒返回 false，该帧跳过三个 SDF dispatch。
        const bool sdfDomainValid = object.HasTressfxSdfCollision() &&
            object.WriteTressfxSdfParams(state.frameContext.skinningMatrix, jointMatrices,
                frameSlot);
        const uint32_t vertexGroupCount =
            (dispatch.pointCount + GuideSimulationWorkgroupSize - 1) /
            GuideSimulationWorkgroupSize;
        const uint32_t strandGroupCount =
            (dispatch.guideCount + GuideSimulationWorkgroupSize - 1) /
            GuideSimulationWorkgroupSize;
        // trace 元数据里的 dispatch 数只统计该对象的 pass；RecordPass 内部累加。
        uint32_t objectDispatchCount = 0;

        // 逐帧距离场：clear -> splat 每帧一次，放在 substep 循环之前；源每帧
        // 重建一次 SDF，SDF collide 则每个 substep 一次。三者同属该对象的连续
        // dispatch，RecordPass 会插入 compute→compute 内存 barrier。
        const uint32_t sdfCellGroupCount =
            (object.GetTressfxSdfCellCount() + GuideSimulationWorkgroupSize - 1) /
            GuideSimulationWorkgroupSize;
        const uint32_t sdfTriangleGroupCount =
            (object.GetTressfxSdfTriangleCount() + GuideSimulationWorkgroupSize - 1) /
            GuideSimulationWorkgroupSize;
        if (sdfDomainValid)
        {
            RecordPass(commandBuffer, GuideSimulationPass::SdfClear,
                state.sets[PassIndex(GuideSimulationPass::SdfClear)][frameSlot],
                sdfCellGroupCount, anyDispatch, objectDispatchCount);
            RecordPass(commandBuffer, GuideSimulationPass::SdfSplat,
                state.sets[PassIndex(GuideSimulationPass::SdfSplat)][frameSlot],
                sdfTriangleGroupCount, anyDispatch, objectDispatchCount);
        }

        // 源主路径：每帧 0/1/2 个 substep，每个 pass 收到的 dt 恒为 0.0125。
        // reset 只作用于首轮 Integration（写满三份历史），同一轮后续
        // StrandLevel/VSP/LocalShape/LengthWind/SDF collide 仍执行；第二轮正常。
        for (uint32_t substep = 0; substep < plan.substeps.schedule.substeps;
             ++substep)
        {
            const bool firstSubstep = substep == 0;
            // 严格按 TressFX 的 pass 顺序录制；VSP 关闭时完全不录制该 pass，
            // 与 TressFX 的 EnableVSP 开关对应。LocalShape 的 stiffness 为 0 时
            // shader 内早退，仍保留 dispatch 以维持固定 pass 表。
            const vk::DescriptorSet integrationSet =
                firstSubstep && plan.substeps.FirstSubstepUsesSeparateParams()
                ? state.resetSets[frameSlot]
                : state.sets[PassIndex(GuideSimulationPass::Integration)][frameSlot];
            RecordPass(commandBuffer, GuideSimulationPass::Integration,
                integrationSet, vertexGroupCount, anyDispatch, objectDispatchCount);
            RecordPass(commandBuffer, GuideSimulationPass::StrandLevel,
                state.sets[PassIndex(GuideSimulationPass::StrandLevel)][frameSlot],
                strandGroupCount, anyDispatch, objectDispatchCount);
            if (config.vspCoefficient > 0.0f)
                RecordPass(commandBuffer, GuideSimulationPass::Vsp,
                    state.sets[PassIndex(GuideSimulationPass::Vsp)][frameSlot],
                    vertexGroupCount, anyDispatch, objectDispatchCount);
            RecordPass(commandBuffer, GuideSimulationPass::LocalShape,
                state.sets[PassIndex(GuideSimulationPass::LocalShape)][frameSlot],
                strandGroupCount, anyDispatch, objectDispatchCount);
            RecordPass(commandBuffer, GuideSimulationPass::LengthWind,
                state.sets[PassIndex(GuideSimulationPass::LengthWind)][frameSlot],
                strandGroupCount, anyDispatch, objectDispatchCount);
            // TressFX 5.0 guide shader 没有胶囊路径；这是 VL 在长度 pass 之后的追加 pass。
            if (params.colliderCount > 0)
                RecordPass(commandBuffer, GuideSimulationPass::Collision,
                    state.sets[PassIndex(GuideSimulationPass::Collision)][frameSlot],
                    strandGroupCount, anyDispatch, objectDispatchCount);
            // 源 SDF collide 每个 substep 一次，在长度/胶囊之后。
            if (sdfDomainValid)
                RecordPass(commandBuffer, GuideSimulationPass::SdfCollide,
                    state.sets[PassIndex(GuideSimulationPass::SdfCollide)][frameSlot],
                    strandGroupCount, anyDispatch, objectDispatchCount);
        }

        if (traceThisObject)
            traceInfo.dispatchCount = objectDispatchCount;

        // 每帧只提交一次：逐对象仿真帧计数（Guides.Frame 镜像）每渲染帧 +1、
        // 不随 substep 增加；风的时间相位来自 renderScene.windTimeSeconds，不读
        // 这个计数。reset/clock/history 与 modelHistory 都只在整帧录制完成后消费。
        state.modelHistory.Commit(hairDraw.model);
        state.history.Commit(hasPose,
            hasPose ? sourceDraw->skinPose->playbackIdentity : 0,
            hasPose ? sourceDraw->skinPose->discontinuity : 0);
        state.clock.Consume(renderScene.worldGeneration, renderScene.frameIndex,
            sourceDraw ? sourceDraw->simulationDeltaSeconds
                       : renderScene.hairSimulationDeltaSeconds);
        ++state.simulationFrame;
        if (resetPositions)
        {
            // reset 帧会继续录制同轮后续 pass（以及 n == 2 时的第二轮）；日志每帧一次。
            std::cout << "[TressFX] ResetPositions: object=\"" << object.GetName()
                      << "\" reason=" << GuideSimulationResetReasonName(plan.history.reason)
                      << " playbackIdentity="
                      << (hasPose ? sourceDraw->skinPose->playbackIdentity : 0)
                      << " discontinuity="
                      << (hasPose ? sourceDraw->skinPose->discontinuity : 0)
                      << " frame=" << renderScene.frameIndex
                      << " substeps=" << plan.substeps.schedule.substeps
                      << std::endl;
        }
    }
    if (pendingResetObject)
    {
        // 未命中的名字按“下一帧一次”语义丢弃并报告；保留到未来帧会让一次误拼的
        // 控制台命令在很久以后突然复位。
        if (!resetMatched)
            std::cerr << "[TressFX] Hair reset ignored: no GPU guide object matches \""
                      << *pendingResetObject << "\"" << std::endl;
        pendingResetObject.reset();
    }
    // 该对象所有 dispatch（含 SDF collide）录完后才 copy；copy 前后各一个显式
    // compute→transfer / transfer→compute barrier，见 RecordTraceCopies。
    if (captureTrace)
        RecordTraceCopies(commandBuffer, frameSlot, traceInfo);
    if (anyDispatch)
        RecordPostDispatchBarrier(commandBuffer);
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
