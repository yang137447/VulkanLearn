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
                const std::shared_ptr<RenderableObject> object = draw.renderableObject;
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
        uint32_t simulationGuideCount = 0;
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
                std::shared_ptr<RenderableObject> object = draw.renderableObject;
                if (!object || !object->UsesGpuGuideSimulation())
                    continue;
                // 同一对象可能因材质分组出现在多个 draw 中；重复 dispatch 会多步进一次。
                if (!seen.insert(object.get()).second)
                    continue;
                const uint32_t guideCount = object->GetTressfxGuideCount();
                const uint32_t pointsPerGuide = object->GetTressfxPointsPerGuide();
                const uint32_t simulationGuideCount =
                    object->GetTressfxSimulationGuideCount();
                // 先算完再 move：braced-init-list 从左到右求值，move 之后 object 已为空。
                dispatches.push_back({std::move(object), guideCount,
                    guideCount * pointsPerGuide, simulationGuideCount,
                    draw.drawPacketIndex, false});
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
                            draw.renderableObject;
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
        uint32_t simulationGuides = 0;
        uint32_t sdfObjects = 0;
        for (const Dispatch& dispatch : dispatches)
        {
            totalGuides += dispatch.guideCount;
            totalPoints += dispatch.pointCount;
            simulationGuides += dispatch.simulationGuideCount;
            if (dispatch.object->HasTressfxSdfCollision())
                ++sdfObjects;
        }
        std::cout << "[TressFX] GPU guide simulation dispatch: objects="
                  << dispatches.size() << " guides=" << totalGuides
                  << " points=" << totalPoints
                  << " simGuides=" << simulationGuides
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
            hairDraw.model,
            dispatch.simulationGuideCount);
        // motion.w 的 bit2：Integration/StrandLevel 是否读取逐 strand 多骨权重。
        // 没有 pose/palette 的帧回退单根骨矩阵，避免读到全零 palette。
        if (!skinning.strandBones.empty() && jointMatrices && !jointMatrices->empty())
            params.motion.w() += 4.0f;
        // 每帧重写当前槽：正常帧内容不变，实时覆盖修改对象副本后也能在
        // 当前 fence 已完成的槽里安全生效。
        object.WriteTressfxGroupSimulationConfigs(frameSlot);
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
        const uint32_t simulationPointCount =
            dispatch.simulationGuideCount * object.GetTressfxPointsPerGuide();
        const uint32_t vertexGroupCount =
            (simulationPointCount + GuideSimulationWorkgroupSize - 1) /
            GuideSimulationWorkgroupSize;
        const uint32_t strandGroupCount =
            (dispatch.simulationGuideCount + GuideSimulationWorkgroupSize - 1) /
            GuideSimulationWorkgroupSize;
        const uint32_t followGroupCount =
            (dispatch.pointCount + GuideSimulationWorkgroupSize - 1) /
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
            if (object.HasTressfxGroupVsp())
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
        // 路线 B：follow 在全部 guide 模拟完成后重建到同一份 40k 位置 buffer。
        if (object.HasTressfxFollowBinding())
            RecordPass(commandBuffer,
                GuideSimulationPass::FollowInterpolation,
                state.sets[PassIndex(GuideSimulationPass::FollowInterpolation)][frameSlot],
                followGroupCount, anyDispatch, objectDispatchCount);

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

} // namespace VL::Hair
