#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>

#include "hair/tressfxSimulationParams.h"
#include "hair/tressfxSimulationHistory.h"
#include "hair/tressfxFrameContext.h"
#include "hair/tressfxSimulationOwner.h"
#include "hair/tressfxLiveParams.h"
#include "hair/hairTraceFormat.h"

class ComputePipeline;
class PipelineFactory;
class RenderableObject;

namespace VL
{
class RendererBackendVulkan;
struct ResolvedRenderScene;
struct RenderScene;

namespace Hair
{
// TressFX 5.0 guide solver 的 pass 顺序，严格对应源 .usf：
//   Integration -> StrandLevel -> VSP -> LocalShape -> LengthConstraintsWind
// 之后是 VL 追加的碰撞阶段：胶囊 Collision（源 guide shader 没有），以及
// `collision.mesh` 存在时每帧重建距离场的 SdfClear -> SdfSplat -> SdfCollide。
// 最后三个是 VL 对 TressFXSimulationSDFGen* 与
// TressFXSimulationCollideStrandsVerticesWithSDF 的移植；它们共用同一张 pass 表，
// Host 按对象是否声明 collision.mesh 决定是否录制。
enum class GuideSimulationPass : uint32_t
{
    Integration = 0,
    StrandLevel,
    Vsp,
    LocalShape,
    LengthWind,
    Collision,
    SdfClear,
    SdfSplat,
    SdfCollide,
    Count,
};
inline constexpr uint32_t GuideSimulationPassCount =
    static_cast<uint32_t>(GuideSimulationPass::Count);

// TressFX guide 模拟的 GPU 执行者。每个 tressfx 对象持有一组 device-local buffer：
// current（与 graphics Object Set 2 binding 3 共用）、previous、prevPrev、rest(w=逆质量)、
// restLengths、strandLevel；外加每帧改写的 params UBO 和静态 collider SSBO。
// 声明 collision.mesh 的对象还持有 uint 距离网格、代理顶点/索引和 64-byte SDF
// 参数块（device-local，创建一次）。本类只拥有该 pass 的 compute pipeline、
// 各自的 descriptor pool、逐对象的 per-pass descriptor set 缓存和 per-object
// simulationFrame（Guides.Frame 计数镜像，不参与风相位），不拥有对象 buffer。
//
// 生命周期与 RenderGraph 的 AutoExposure 一致：随图候选创建，SwapState 移交给活动图，
// Shutdown 时按 GPU epoch 退休。pipeline 走 CreateComputePipelineCandidate，不注册
// hot reload participant，因此 shader 热重载不会替换它们。
class HairSimulationPass
{
public:
    ~HairSimulationPass();
    HairSimulationPass() = default;
    HairSimulationPass(const HairSimulationPass&) = delete;
    HairSimulationPass& operator=(const HairSimulationPass&) = delete;

    void Initialize(PipelineFactory& factory, RendererBackendVulkan& backend);
    // 判断本帧是否存在 GPU guide simulation 或关闭模拟后的骨骼蒙皮。上层必须在写
    // hairSimulation 的 timestamp 之前调用：没有任何 dispatch 的帧不能占用 query 槽位，
    // 否则先写 begin、再丢弃、再复用同一 query 会违反"查询必须先重置"的规则。
    bool HasWork(const VL::ResolvedRenderScene& scene,
        const VL::RenderScene& renderScene) const;
    // frameSlot 是 frame-in-flight 槽；调用方在每帧 pass 循环之前调用一次。
    // renderScene 是同一帧冻结的前端快照：simulation.skinning 从它的 draw packet 读取
    // SkinPose，不从头重建场景查询，也不在 pass 里持有播放器。
    void Record(vk::CommandBuffer commandBuffer,
        const VL::ResolvedRenderScene& scene,
        const VL::RenderScene& renderScene,
        uint32_t frameSlot);

    // 一次性 GPU 读回（正常 Console `hair trace`）。BeginTrace 只记录值语义请求，
    // 真正的 Vulkan buffer 在渲染线程下一次 Record 选中第一个 GPU hair 对象时分配；
    // FinishTrace(true) 会等待队列（仅退出/显式结束路径），用于冲刷仍 pending 的
    // 最后 1..2 个 frame slot。未请求时这两条路径零开销。
    void BeginTrace(const HairTraceRequest& request);
    void FinishTrace(bool waitForGpu);
    bool IsTraceActive() const noexcept { return trace.active; }

    // 显式复位（Console `hair reset` / 毛发视口 Reset 按钮）。只登记目标对象名
    // （空 = 所有 GPU guide 对象）；真正的 ResetPositions 在下一个正 delta 帧
    // 由 Record 消费，复用既有 motion.y/resetParams 分支，不新建 buffer。
    void RequestReset(std::string objectName);

    // 实时参数覆盖（Console `hair set` / 编辑器面板）。只登记值语义；Record 开头
    // 按对象名应用到该渲染对象自己的 config 副本，下一帧组 params UBO 时生效。
    // GPU 之外的对象在 Record 中拒绝并报告，不静默丢弃。
    void RequestLiveParameter(VL::Hair::HairLiveParameterRequest request);

private:
    // 关闭动力学的对象独立持有 descriptor；不借用 solver 的历史或 SDF 绑定。
    // buffer 仍归 RenderableObject，弱引用只用于识别退休后被复用的对象地址。
    struct SkinningDescriptors
    {
        std::weak_ptr<RenderableObject> owner;
        std::array<vk::DescriptorSet, GuideSimulationParamBufferCount> sets{};
        vk::Buffer currentPositions = nullptr;
        vk::Buffer restPositions = nullptr;
        vk::Buffer boneWeights = nullptr;
        std::array<vk::Buffer, GuideSimulationParamBufferCount> params{};
        std::array<vk::Buffer, GuideSimulationParamBufferCount> palette{};
        TressfxFrameContext frameContext;
        TressfxSimulationOwner simulationOwner;
        bool reportedOwnerConflict = false;
        bool reportedFirstDispatch = false;
    };

    struct ObjectDescriptors
    {
        // [pass][frame-in-flight slot]。每个 pass 的 shader 只声明自己用到的 binding，
        // 因此 descriptor set layout 按 pipeline 独立，不能跨 pass 复用同一个 set。
        std::array<std::array<vk::DescriptorSet, GuideSimulationParamBufferCount>,
            GuideSimulationPassCount>
            sets{};
        // reset/换基首轮 Integration 专用 set：binding 0 指向独立的 resetParams，
        // 其余位置 buffer 与正常 Integration 相同。每个 slot 一个，随对象创建。
        std::array<vk::DescriptorSet, GuideSimulationParamBufferCount> resetSets{};
        vk::Buffer currentPositions = nullptr;
        vk::Buffer previousPositions = nullptr;
        vk::Buffer previousPreviousPositions = nullptr;
        vk::Buffer restPositions = nullptr;
        vk::Buffer restLengths = nullptr;
        vk::Buffer strandLevel = nullptr;
        // 胶囊 SSBO 按 frame-in-flight 分槽（绑定骨骼后内容逐帧变化）。
        std::array<vk::Buffer, GuideSimulationParamBufferCount> colliders{};
        // collision.mesh 的四个 buffer；SDF params 按 frame-in-flight 分槽
        // （域每帧变化）。未声明时全为空，用于缓存项校验。
        std::array<vk::Buffer, GuideSimulationParamBufferCount> sdfParams{};
        vk::Buffer sdfGrid = nullptr;
        vk::Buffer sdfTriangles = nullptr;
        vk::Buffer sdfIndices = nullptr;
        // SDF splat 的代理影响流（静态）与逐 frame-slot 的全骨架 palette
        // buffer（句柄静态、内容每帧由 RenderableObject 写，用于缓存校验）。
        vk::Buffer sdfProxyInfluences = nullptr;
        std::array<vk::Buffer, GuideSimulationParamBufferCount> sdfPalette{};
        std::array<vk::Buffer, GuideSimulationParamBufferCount> params{};
        // 分组求解：每槽一份解析配置，组号 buffer 与图形顶点阶段共用。
        std::array<vk::Buffer, GuideSimulationParamBufferCount> groupConfigs{};
        vk::Buffer guideGroupIds = nullptr;
        // reset/换基首轮 Integration 的独立参数块（同布局），与 params 分开持有。
        std::array<vk::Buffer, GuideSimulationParamBufferCount> resetParams{};
        // 多骨：逐 strand 4×(joint,weight) 与每槽全骨架 palette。
        vk::Buffer boneWeights = nullptr;
        std::array<vk::Buffer, GuideSimulationParamBufferCount> skinningPalette{};
        std::array<vk::Buffer, GuideSimulationParamBufferCount> penetrationStats{};
        // TressFX Guides.Frame 的计数镜像：每渲染帧 +1（含 reset 帧），同一帧的
        // 所有 substep 共用。运行时的风时间相位不再从这里取，而是来自
        // renderScene.windTimeSeconds（场景阵风）。
        uint32_t simulationFrame = 0;
        // simulation.skinning 的逐对象缓存：pose/源 draw 缺失时保留上一帧矩阵继续渲染，
        // 不用单位矩阵覆盖，也不销毁 pass/descriptor。初始单位矩阵等于历史 rest pin。
        TressfxFrameContext frameContext;
        GuideSimulationHistory history;
        TressfxFrameClock clock;
        TressfxModelHistory modelHistory;
        TressfxSimulationOwner simulationOwner;
        bool reportedMissingSkinPose = false;
        bool reportedOwnerConflict = false;
        bool reportedNormalRebase = false;
        bool reportedPausedRebase = false;
        // 显式复位请求：只在该对象真正有 substep 的帧消费；暂停/0 步帧保留，
        // 使 `hair reset` 不会因为当帧没有物理推进而丢失。
        bool resetRequested = false;
    };

    struct DescriptorPoolBudget
    {
        vk::DescriptorPool pool = nullptr;
        uint32_t remainingSets = 0;
    };

    // 每个 frame-in-flight 槽一份 host-visible readback buffer。布局为
    // state -> selectedGuide -> point 连续 float4；文件写入时再按
    // guide -> point -> state 交错（见 hairTraceFormat.h）。
    struct TraceSlot
    {
        vk::Buffer buffer = nullptr;
        vk::DeviceMemory memory = nullptr;
        void* mapped = nullptr;
        vk::DeviceSize size = 0;
        bool pending = false;
        HairTraceFrameInfo info;
    };

    // 只读、单次会话的 trace 状态。所有成员只由渲染线程（Record）和
    // 渲染线程停止后的 FinishTrace 访问。
    struct TraceState
    {
        bool active = false;
        bool finishing = false;
        bool restRecorded = false;
        bool writerOpen = false;
        bool writeFailed = false;
        bool reportedObjectLoss = false;
        uint64_t capturedFrames = 0;
        uint64_t flushedFrames = 0;
        uint64_t requestedFrames = 0;
        uint64_t startFrame = 0;
        bool hasStartFrame = false;
        uint64_t bytesWritten = 0;
        HairTraceRequest request;
        std::string objectName;
        RenderableObject* object = nullptr;
        uint32_t totalGuideCount = 0;
        uint32_t pointsPerGuide = 0;
        uint32_t selectedCount = 0;
        std::vector<uint32_t> selectedGuides;
        std::vector<float> restLengths;
        std::unique_ptr<HairTraceWriter> writer;
        std::array<TraceSlot, GuideSimulationParamBufferCount> slots{};
        vk::Buffer restBuffer = nullptr;
        vk::DeviceMemory restMemory = nullptr;
        void* restMapped = nullptr;
        vk::DeviceSize restBytes = 0;
    };

    vk::DescriptorPool AcquireDescriptorPool(uint32_t pass, uint32_t setCount);
    ObjectDescriptors& EnsureObjectDescriptors(RenderableObject& object);
    bool MatchesObjectDescriptors(
        const RenderableObject& object, const ObjectDescriptors& descriptors) const;
    void CreateObjectDescriptors(
        RenderableObject& object,
        ObjectDescriptors& descriptors);
    void RecordPass(vk::CommandBuffer commandBuffer,
        GuideSimulationPass pass,
        vk::DescriptorSet descriptorSet,
        uint32_t groupCount,
        bool& anyDispatch,
        uint32_t& objectDispatchCount);
    void ResetDescriptorCache() noexcept;
    SkinningDescriptors& EnsureSkinningDescriptors(
        const std::shared_ptr<RenderableObject>& object);
    void RecordSkinning(vk::CommandBuffer commandBuffer,
        const VL::ResolvedRenderScene& scene,
        const VL::RenderScene& renderScene,
        uint32_t frameSlot);

    // trace 的录制/回读辅助。未激活时都在 Record 开头一次布尔判断后返回。
    void StartTraceObject(RenderableObject& object);
    void FlushTraceSlot(uint32_t frameSlot);
    bool WriteTraceHeaderFromRestLengths();
    void RecordTraceCopies(vk::CommandBuffer commandBuffer,
        uint32_t frameSlot,
        const HairTraceFrameInfo& info);
    void DestroyTraceResources();
    void FinishTraceInternal(bool waitForGpu);
    void TryFinishTrace();

    RendererBackendVulkan* backend = nullptr;
    std::array<std::shared_ptr<ComputePipeline>, GuideSimulationPassCount> pipelines{};
    std::shared_ptr<ComputePipeline> skinningPipeline;
    // 最后一条池链供独立蒙皮；源 guide solver 的九阶段枚举与调度保持不变。
    std::array<std::vector<DescriptorPoolBudget>, GuideSimulationPassCount + 1> descriptorPools;
    std::unordered_map<const RenderableObject*, ObjectDescriptors> objectDescriptors;
    std::unordered_map<const RenderableObject*, SkinningDescriptors> skinningDescriptors;
    TraceState trace;
    // 渲染线程独占：RequestReset 在 RecordHairSimulation 内、Record 之前调用；
    // 下一帧 Record 把它展开成逐对象 resetRequested 后清空。
    std::optional<std::string> pendingResetObject;
    // 实时参数覆盖队列：渲染线程独占；Record 取走并清空。有界，避免无渲染帧时累积。
    std::vector<VL::Hair::HairLiveParameterRequest> pendingLiveParameters;
    bool initialized = false;
    bool reportedFirstDispatch = false;
};

} // namespace Hair
} // namespace VL
