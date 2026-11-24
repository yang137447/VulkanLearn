#pragma once

#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <nlohmann/json.hpp>

#include "hair/rod/rodTypes.h"
#include "hair/rod/rodProgressiveSupport.h"
#include "hair/rod/rodSectionInertia.h"
#include "shaderReflect.h"
#include "mesh/loader/common/meshAssetLoader.h"

namespace VL::Hair
{
void HairSimulationPass::RecordRod(vk::CommandBuffer commandBuffer,
    const std::shared_ptr<RenderableObject>& object,
    const VL::RenderScene& renderScene, size_t drawPacketIndex,
    uint32_t frameSlot, bool& anyDispatch)
{
    if (object->GetTressfxPointsPerGuide() != 32)
        throw std::runtime_error("CosseratXpbdV1 GPU requires exactly 32 points per guide");
    const auto& solverConfig = object->GetTressfxSimulationConfig();
    const uint32_t algorithmVersion = TressfxSolverAlgorithmVersion(solverConfig.solver);
    const bool progressive = solverConfig.solver == TressfxSimulationSolver::CosseratImplicitV3;
    const uint32_t profileMathVersion = TressfxSupportProfileMathVersion(solverConfig.solver);
    const uint32_t kineticMathVersion = progressive ? Rod::SectionInertiaKineticMathVersion : 1;
    const uint32_t numericEncodingVersion = progressive ? 3 : 1;
    ObjectDescriptors& tressfx = EnsureObjectDescriptors(*object);
    if (tressfx.simulationOwner.HasConflict())
        return;
    const auto& draw = renderScene.drawPackets[drawPacketIndex];
    if (!RodGpuIsRigid(draw.model))
        throw std::runtime_error("CosseratXpbdV1 requires a rigid hair placement");
    const auto& skinning = object->GetTressfxSkinningBinding();
    const auto* source = FindSkinningSourceDraw(skinning, renderScene);
    const bool hasPose = HasRootSkinPose(source, skinning.jointIndex);
    if (hasPose)
        UpdateTressfxFrameContext(tressfx.frameContext, draw.model, source->model,
            *source->skinPose, skinning.jointIndex, source->simulationDeltaSeconds);
    else if (skinning.enabled)
        tressfx.history.ObserveMissingPose();
    RodGpuPose pose;
    pose.root = draw.model * tressfx.frameContext.skinningMatrix;
    for (const auto& joint : tressfx.frameContext.jointMatrices)
        pose.palette.push_back(draw.model * joint);
    if (!RodGpuIsRigid(pose.root))
        throw std::runtime_error("CosseratXpbdV1 root transform must be rigid");
    auto found = rodObjects.find(object.get());
    if (found == rodObjects.end())
    {
        auto candidate = std::make_shared<RodGpuObject>();
        candidate->owner = object;
        candidate->backend = backend;
        candidate->current = object->GetTressfxPositionInfo().buffer;
        candidate->guides = object->GetTressfxSimulationGuideCount();
        candidate->algorithmVersion = algorithmVersion;
        candidate->profileMathVersion = profileMathVersion;
        candidate->kineticMathVersion = kineticMathVersion;
        candidate->numericEncodingVersion = numericEncodingVersion;
        const auto& section = object->GetImported()->modelResource.sections[object->GetImportedSectionIndex()];
        const uint32_t firstGuide = section.tressfxSimulationGuideIndices.empty() ? 0u :
            section.tressfxSimulationGuideIndices.front();
        candidate->fixedWorldOrigin = (pose.root *
            section.vertices[firstGuide * 32u * 2u].position.homogeneous()).head<3>() * .01f;
        candidate->paletteCapacity = static_cast<uint32_t>(
            std::max<size_t>(1, pose.palette.size()));
        candidate->state.Create(*backend, static_cast<vk::DeviceSize>(candidate->guides) *
            RodGpuGuideVec4Count * 16, false, "RodWorldState: " + object->GetName());
        if (progressive)
        {
            candidate->references.Create(*backend, static_cast<vk::DeviceSize>(candidate->guides) *
                RodGpuReferenceVec4Count * 16, false, "RodFixedWorldRestReferences");
            candidate->frameDeltas.Create(*backend, static_cast<vk::DeviceSize>(candidate->guides) *
                31 * 16, false, "RodMaterialFrameDeltas");
            candidate->lowParts.Create(*backend, static_cast<vk::DeviceSize>(candidate->guides) *
                63 * 16, false, "RodCompensatedPositionAndMaterialDelta");
            candidate->failureMatrix.Create(*backend, 932 * 16, false, "RodFailureMatrix");
            candidate->failureCandidate.Create(*backend, 416 * 16, false, "RodFailureCandidate");
            if (const char* capture = std::getenv("VL_ROD_CAPTURE_SOURCE_GUIDE"))
            {
                const uint32_t sourceGuide = static_cast<uint32_t>(std::stoul(capture));
                const auto& selected = section.tressfxSimulationGuideIndices;
                const auto position = std::find(selected.begin(), selected.end(), sourceGuide);
                if (position != selected.end())
                    candidate->captureFailureSlot = static_cast<uint32_t>(position - selected.begin());
            }
        }
        if (object->HasTressfxSdfCollision())
            candidate->sdfHistory.Create(*backend,
                128 + static_cast<vk::DeviceSize>(object->GetTressfxSdfCellCount()) * 4,
                false, "RodSdfHistory");
        const std::array<vk::DescriptorPoolSize, 2> sizes{{
            {vk::DescriptorType::eUniformBuffer, 40},
            {vk::DescriptorType::eStorageBuffer, 160}}};
        vk::DescriptorPoolCreateInfo poolInfo;
        poolInfo.setPoolSizes(sizes).setMaxSets(8);
        candidate->pool = backend->CreateDescriptorPool(poolInfo, "RodDescriptors");
        std::cout << "[RodGPU] create descriptor pool=0x" << std::hex <<
            reinterpret_cast<uintptr_t>(static_cast<VkDescriptorPool>(candidate->pool)) <<
            std::dec << std::endl;
        for (uint32_t slot = 0; slot < GuideSimulationParamBufferCount; ++slot)
        {
            candidate->params[slot].Create(*backend, sizeof(RodGpuParams), true, "RodParams");
            if (progressive)
                candidate->implicitParams[slot].Create(*backend, 32, true, "RodImplicitV3Controls");
            candidate->samples[slot].Create(*backend, sizeof(Eigen::Matrix4f) *
                (RodGpuMaxSteps + 1) * candidate->paletteCapacity, true, "RodPoseSamples");
            candidate->readback[slot].Create(*backend,
                static_cast<vk::DeviceSize>(candidate->guides) * 64 +
                    (RodGpuGuideVec4Count + RodGpuReferenceVec4Count + 31 + 932 + 416) * 16,
                true, "RodDiagnosticsReadback");
            std::array<vk::DescriptorBufferInfo, 33> infos{};
            infos[0] = object->GetTressfxParamsInfo(slot);
            infos[1] = object->GetTressfxPositionInfo();
            infos[2] = object->GetTressfxPreviousPositionInfo();
            infos[3] = object->GetTressfxPreviousPreviousPositionInfo();
            infos[4] = object->GetTressfxRestPositionInfo();
            infos[7] = object->GetTressfxColliderInfo(slot);
            infos[8] = object->GetTressfxSdfParamsInfo(slot);
            infos[11] = object->GetTressfxSdfGridInfo();
            infos[14] = object->GetTressfxBoneWeightsInfo();
            infos[19] = object->GetTressfxSimulationGuideInfo();
            infos[20] = object->GetTressfxFollowGuideInfo();
            infos[21] = object->GetTressfxFollowOffsetInfo();
            infos[22] = object->GetTressfxFollowFrameRefInfo();
            if (progressive)
            {
                infos[28] = candidate->failureMatrix.Info();
                infos[29] = candidate->failureCandidate.Info();
                infos[30] = candidate->lowParts.Info();
            }
            infos[23] = candidate->state.Info();
            infos[24] = candidate->params[slot].Info();
            infos[25] = candidate->samples[slot].Info();
            infos[26] = candidate->sdfHistory.Info();
            infos[27] = candidate->implicitParams[slot].Info();
            infos[31] = candidate->references.Info();
            infos[32] = candidate->frameDeltas.Info();
            for (uint32_t kind = 0; kind < 4; ++kind)
            {
                const bool sdf = object->HasTressfxSdfCollision();
                if (kind == 1 && !object->HasTressfxFollowBinding())
                    continue;
                if (kind == 2 && !sdf)
                    continue;
                // kind 3 只服务 V3：非 V3 的 SDF 求解由 rodSolveSdf 走 kind 0。
                if (kind == 3 && !(progressive && sdf))
                    continue;
                auto& pipeline = rodPipelines[kind == 3 ? 6 : kind == 2 ? 3 : kind == 1 ?
                    (progressive ? 5 : 2) : progressive ? 4 : (sdf ? 1 : 0)];
                const auto layout = pipeline->GetDescriptorSetLayouts().at(0);
                vk::DescriptorSetAllocateInfo allocation;
                allocation.setDescriptorPool(candidate->pool).setSetLayouts(layout);
                std::vector<vk::DescriptorSet> allocated(1);
                backend->AllocateDescriptorSets(allocation, allocated);
                (kind == 0 ? candidate->solveSets[slot] : kind == 3 ?
                    candidate->solveSdfSets[slot] : kind == 1 ?
                    candidate->followSets[slot] : candidate->sdfHistorySets[slot]) =
                    allocated.front();
                std::vector<vk::WriteDescriptorSet> writes;
                for (const auto& reflected : pipeline->GetShaderBindings())
                {
                    const uint32_t binding = reflected.binding;
                    if (reflected.set != 0 || binding >= infos.size() || !infos[binding].buffer)
                        throw std::runtime_error("Rod GPU shader descriptor has no owned buffer");
                    vk::WriteDescriptorSet write;
                    write.setDstSet(allocated.front()).setDstBinding(binding)
                        .setDescriptorType(reflected.type)
                        .setBufferInfo(infos[binding]);
                    writes.push_back(write);
                }
                backend->UpdateDescriptorSets(writes);
            }
        }
        candidate->ClearTimeline(pose);
        candidate->world = renderScene.worldGeneration;
        found = rodObjects.emplace(object.get(), std::move(candidate)).first;
    }
    RodGpuObject& state = *found->second;
    if (state.owner.lock() != object || state.current != object->GetTressfxPositionInfo().buffer ||
        pose.palette.size() > state.paletteCapacity || state.algorithmVersion != algorithmVersion ||
        state.profileMathVersion != profileMathVersion || state.kineticMathVersion != kineticMathVersion ||
        state.numericEncodingVersion != numericEncodingVersion)
        throw std::runtime_error("Rod GPU resource identity changed outside graph transaction");
    if (state.readbackPending[frameSlot] &&
        state.readbackGeneration[frameSlot] != state.simulationGeneration)
        state.readbackPending[frameSlot] = false;
    if (state.readbackPending[frameSlot])
    {
        std::ofstream output("artifacts/hair-landing-20261119/gpu-runtime-diagnostics.csv", std::ios::app);
        output << std::setprecision(10);
        const float* values = static_cast<const float*>(state.readback[frameSlot].mapped);
        double accepted = state.receivedSeconds;
        // 每帧重新求最大值：gpuStatus 现在是逐帧报告量，不再是永久粘滞的冻结开关。
        // `accepted` 仍是「所有 guide 都接受」的最小模拟时间，因此有 guide 停住时
        // 该列会停在它的时间上——这是有意的信息，调度不再依赖它。
        state.gpuStatus = 0;
        float maxBlockDefect = 0.0f;
        float maxPredictorDefect = 0.0f;
        uint32_t failedGuides = 0;
        uint32_t maxIterations = 0;
        // 超过硬帧轮数阈值的 guide 数：默认门判据（单个 guide 不再能钉住整个对象）。
        uint32_t hardGuides = 0;
        std::ofstream failures("artifacts/hair-landing-20261119/gpu-failed-guides.csv", std::ios::app);
        const auto& section = object->GetImported()->modelResource.sections[
            object->GetImportedSectionIndex()];
        for (uint32_t guide = 0; guide < state.guides; ++guide)
        {
            const float* diagnostic = values + guide * 16;
            accepted = std::min(accepted, static_cast<double>(diagnostic[12]));
            state.gpuStatus = std::max(state.gpuStatus, static_cast<uint32_t>(diagnostic[0]));
            maxBlockDefect = std::max(maxBlockDefect, diagnostic[1]);
            maxPredictorDefect = std::max(maxPredictorDefect, diagnostic[2]);
            maxIterations = std::max(maxIterations, static_cast<uint32_t>(diagnostic[3]));
            hardGuides += static_cast<uint32_t>(diagnostic[3]) >
                RodGpuV3HardFrameIterations ? 1u : 0u;
            failedGuides += diagnostic[0] != 0.0f ? 1 : 0;
            if (diagnostic[0] != 0.0f)
            {
                const uint32_t sourceGuide = section.tressfxSimulationGuideIndices.empty() ?
                    guide : section.tressfxSimulationGuideIndices[guide];
                failures << state.readbackFrame[frameSlot] << ',' << guide << ',' <<
                    sourceGuide << ',' << diagnostic[0] << ',' << diagnostic[3] << ',' <<
                    diagnostic[7] << ',' << diagnostic[12] << '\n';
            }
        }
        state.acceptedSeconds = accepted;
        state.maxIterationsLastFence = maxIterations;
        state.hardGuidesLastFence = hardGuides;
        if (failedGuides != 0 && !state.failureEvidenceWritten &&
            state.captureFailureSlot != UINT32_MAX &&
            values[state.captureFailureSlot * 16] != 0.0f)
        {
            const std::string prefix = "artifacts/hair-landing-20261119/root-failure-encoding" +
                std::to_string(state.numericEncodingVersion) + "-" +
                std::to_string(state.readbackFrame[frameSlot]);
            const auto save = [&](const char* suffix, const void* data, size_t bytes)
            {
                std::ofstream output(prefix + suffix, std::ios::binary);
                output.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
                if (!output)
                    throw std::runtime_error("Rod failure evidence write failed");
            };
            const size_t exportOffset = state.guides * 64 +
                (RodGpuGuideVec4Count + RodGpuReferenceVec4Count + 31) * 16;
            save(".matrix.bin", reinterpret_cast<const char*>(values) + exportOffset, 932 * 16);
            save(".candidate.bin", reinterpret_cast<const char*>(values) + exportOffset + 932 * 16, 416 * 16);
            save(".params.bin", state.params[frameSlot].mapped, sizeof(RodGpuParams));
            save(".palettes.bin", state.samples[frameSlot].mapped,
                (RodGpuMaxSteps + 1) * state.paletteCapacity * sizeof(Eigen::Matrix4f));
            save(".controls.bin", state.implicitParams[frameSlot].mapped, 32);
            state.failureEvidenceWritten = true;
        }
        // 【行为变更】`gpuStatus` 现在只用于报告，不再改写求解器侧的时间债。
        // 旧策略把“有一个 guide 失败”折算成整个对象的时间债，再配合下面的
        // `gpuStatus == 0` 门控把 steps 归零。现在单 guide 失败由 shader 侧
        // 不发布 + 下帧重试处理，债只反映调度（收到多少时间、派发多少步）。
        // 列序（前缀不可变，脚本按位置读）：
        //   frame,guides,gpuStatus,maxBlockDefect,maxPredictorDefect,accepted,
        //   received,dispatched,debt,failedGuides
        // 之后是本轮新增的可观测量：
        //   substeps（本行读回所属帧实际派发的 substep 数）,
        //   maxIterationsLastFence（本行读回里单 guide 最大牛顿轮数）,
        //   hardGuides（轮数 > RodGpuV3HardFrameIterations 的 guide 数，默认门判据）,
        //   droppedSeconds（累计被 debt ceiling 丢弃的仿真秒数）。
        output << state.readbackFrame[frameSlot] << ',' << state.guides << ',' <<
            state.gpuStatus << ',' << maxBlockDefect << ',' << maxPredictorDefect << ',' <<
            accepted << ',' << state.receivedSeconds << ',' << state.dispatchedSeconds << ',' <<
            state.debtSeconds << ',' << failedGuides << ',' <<
            state.readbackSteps[frameSlot] << ',' << maxIterations << ',' <<
            hardGuides << ',' << state.droppedSeconds << '\n';
        if (trace.active)
        {
            std::ofstream binary("artifacts/hair-landing-20261119/gpu-first-guide-" +
                std::to_string(state.readbackFrame[frameSlot]) + ".bin", std::ios::binary);
            binary.write(reinterpret_cast<const char*>(values + state.guides * 16),
                RodGpuGuideVec4Count * 16);
            if (progressive)
            {
                const auto* references = values + state.guides * 16 + RodGpuGuideVec4Count * 4;
                std::ofstream referenceFile("artifacts/hair-landing-20261119/gpu-first-guide-" +
                    std::to_string(state.readbackFrame[frameSlot]) + ".refs.bin", std::ios::binary);
                referenceFile.write(reinterpret_cast<const char*>(references), RodGpuReferenceVec4Count * 16);
                std::ofstream deltaFile("artifacts/hair-landing-20261119/gpu-first-guide-" +
                    std::to_string(state.readbackFrame[frameSlot]) + ".delta.bin", std::ios::binary);
                deltaFile.write(reinterpret_cast<const char*>(references + RodGpuReferenceVec4Count * 4), 31 * 16);
            }
        }
        state.readbackPending[frameSlot] = false;
    }
    const float delta = source ? source->simulationDeltaSeconds :
        renderScene.hairSimulationDeltaSeconds;
    const auto history = tressfx.history.Plan(hasPose,
        hasPose ? source->skinPose->playbackIdentity : 0,
        hasPose ? source->skinPose->discontinuity : 0, delta);
    const bool explicitReset = tressfx.resetRequested;
    if (state.world != renderScene.worldGeneration || explicitReset ||
        history.action == GuideSimulationFrameAction::Reset)
    {
        state.ClearTimeline(pose);
        state.lastResetReason = static_cast<uint32_t>(explicitReset ?
            GuideSimulationResetReason::Requested : history.reason);
        state.world = renderScene.worldGeneration;
        tressfx.resetRequested = false;
    }
    const bool newFrame = state.lastFrame != renderScene.frameIndex;
    if (newFrame)
    {
        if (std::isfinite(delta) && delta > 0.0f)
        {
            state.intervals.push_back({state.receivedSeconds, state.receivedSeconds + delta,
                state.previousPose, pose});
            state.receivedSeconds += delta;
            state.debtSeconds += delta;
            // debt ceiling：超出部分主动丢弃，而不是无限追赶。每帧最多只能退休
            // RodGpuRetirementCeilingSeconds = 33.3 ms（默认 1/120 s × 4），而单帧
            // 加入上限是 100 ms，没有上限时 debt 只能单调增长，唯一泄压阀是
            // Reset（可见的头发跳变）。丢弃是有意的取舍：头发总量上会比角色慢一点，
            // 但帧时间不再锯齿上涨。
            if (state.debtSeconds > RodGpuMaxDebtSeconds)
            {
                const bool firstDrop = state.droppedSeconds == 0.0;
                const double dropped = state.debtSeconds - RodGpuMaxDebtSeconds;
                state.droppedSeconds += dropped;
                state.debtSeconds = RodGpuMaxDebtSeconds;
                // 丢弃必须可观测；逐帧打印会成为新的噪声源，所以首帧一次、
                // 之后每 120 帧最多一行。完整逐帧计数在诊断 CSV 的 droppedSeconds 列。
                if (firstDrop || (renderScene.frameIndex % 120u) == 0u)
                    std::cout << "[RodGPU] debt ceiling: droppedThisFrame=" <<
                        dropped * 1000.0 << " ms droppedTotal=" <<
                        state.droppedSeconds * 1000.0 << " ms debt=" <<
                        state.debtSeconds * 1000.0 << " ms frame=" <<
                        renderScene.frameIndex << std::endl;
            }
        }
        state.previousPose = pose;
        state.lastFrame = renderScene.frameIndex;
    }
    // 单 guide 的失败不再冻结整个对象：失败的 guide 因为不发布而保持上一次接受的
    // 状态，下一帧重试；其余 guide 照常推进。gpuStatus 只作报告（见上面的读回）。
    // 另外按上一批读回的牛顿轮数限制单帧 substep：还在瞬态（硬帧）时只派发
    // RodGpuV3HardFrameSteps，把“每 guide 数十轮 × 子步上限 × 数千 guide”摊到多帧，
    // 避免单次 dispatch 超过 OS 提交上限；稳态（0–8 轮）仍是满策略子步上限
    // RodGpuSubstepCap()（默认 4，可被 VL_ROD_SUBSTEP_CAP 覆盖）。
    // 默认判据用「超阈值 guide 计数 > 1%（至少 1 个）」，而不是 max-over-guides：
    // 后者在 bind pose 下被一个永不收敛的 guide 永久钉死为 1 substep。
    // VL_ROD_GATE_MODE=legacy 可恢复旧判据做 A/B。
    const uint32_t hardGuideThreshold = std::max(RodGpuV3HardFrameGuideFloor,
        (state.guides * RodGpuV3HardFrameGuidePermille + 999u) / 1000u);
    const bool hardFrame = RodGpuLegacyGateMode() ?
        state.maxIterationsLastFence > RodGpuV3HardFrameIterations :
        state.hardGuidesLastFence > hardGuideThreshold;
    const uint32_t substepBudget = progressive && hardFrame ?
        RodGpuV3HardFrameSteps : RodGpuSubstepCap();
    uint32_t steps = newFrame && delta > 0.0f &&
        !(progressive && state.reset) ? std::min(substepBudget,
        static_cast<uint32_t>(std::floor((state.debtSeconds + 1.0e-9) / RodGpuStepSeconds()))) : 0;
    // 成本测量的诊断开关：固定每帧 substep 数，绕开自适应上限与债务的耦合。
    // 自适应上限会在 guide 变多时把硬帧压到 1 substep、把省下的工作量记成 debt，
    // 于是「成本随 guide 数缩放」的读数会假性次线性——那是仿真在慢放，不是变快。
    // 打开这个开关后，每 substep 成本 = hairSimulation / substeps 才是可比量的。
    // 只在设置环境变量时生效，正常路径零影响。
    if (const char* fixed = std::getenv("VL_ROD_FIXED_SUBSTEPS"))
    {
        const long requested = std::strtol(fixed, nullptr, 10);
        // 允许 0：不派发任何 substep，但整条 pass 照跑，用于量固定开销。
        // 夹到策略上限：固定子步只绕过「自适应上限与债务的耦合」，不能突破
        // 每帧退休上限（也防止越界写 roots/capsules 数组）。
        if (requested >= 0)
            steps = std::min(static_cast<uint32_t>(requested), RodGpuSubstepCap());
    }
    RodGpuParams params;
    params.hairToWorld = draw.model;
    params.worldToHair = draw.model.inverse();
    const Rod::Material material;
    params.material = Eigen::Vector4f(static_cast<float>(material.radius),
        static_cast<float>(material.density), static_cast<float>(material.youngModulus),
        static_cast<float>(material.shearModulus));
    // 诊断：VL_ROD_ITERATION_CAP>0 时把每个 substep 的牛顿轮数上限定为该值
    // （经 params.support.w 传到 shader，默认 64 不变）。用于把「少数 guide 吃满
    // 64 轮」的长尾成本单独切出来：稳态里没有 guide 超过每 substep 1 轮，所以
    // 8 这个上限实际上只作用于卡住的那几根。
    static const float rodIterationCap = [] {
        const char* value = std::getenv("VL_ROD_ITERATION_CAP");
        const double parsed = value != nullptr ? std::strtod(value, nullptr) : 0.0;
        return parsed >= 1.0 ? static_cast<float>(parsed) : 0.0f;
    }();
    params.support = Eigen::Vector4f(static_cast<float>(material.supportOffsetRatio),
        static_cast<float>(material.supportDampingRatio), static_cast<float>(material.referenceGravity),
        rodIterationCap);
    params.gravityStep.head<3>() = object->GetTressfxSimulationConfig().gravity * 0.01f;
    params.gravityStep.w() = static_cast<float>(RodGpuStepSeconds());
    if (progressive)
    {
        std::array<Eigen::Vector4f, 2> controls{
            Eigen::Vector4f(state.fixedWorldOrigin.x(), state.fixedWorldOrigin.y(),
                state.fixedWorldOrigin.z(), 1.0e-4f),
            Eigen::Vector4f(1.0e-6f, 1.0e-6f,
                state.captureFailureSlot == UINT32_MAX ? 0.0f : static_cast<float>(state.captureFailureSlot),
                state.captureFailureSlot == UINT32_MAX ? 0.0f : 1.0f)};
        std::memcpy(state.implicitParams[frameSlot].mapped, controls.data(), 32);
    }
    params.windDrag.head<3>() = TressfxSceneWindVelocityCmPerSec(renderScene.resolvedWind) * 0.01f;
    params.windDrag.w() = static_cast<float>(Rod::EffectiveBundleDragAccelerationPerSpeed(material));
    params.counts = {state.guides, steps, state.paletteCapacity, RodGpuSweeps};
    params.flags = {state.reset ? 1u : 0u, object->HasTressfxFollowBinding() ? 1u : 0u,
        !skinning.strandBones.empty() && !pose.palette.empty() ? 1u : 0u, RodGpuSweeps << 1u};
    params.timing = Eigen::Vector4f(static_cast<float>(state.receivedSeconds),
        static_cast<float>(state.debtSeconds - steps * RodGpuStepSeconds()),
        state.sdfHistoryValid ? 1.0f : 0.0f, 0.0f);
    for (uint32_t sample = 0; sample <= steps; ++sample)
    {
        const RodGpuPose sampled = state.Sample(
            state.dispatchedSeconds + sample * RodGpuStepSeconds());
        params.roots[sample] = sampled.root;
        auto* palette = static_cast<Eigen::Matrix4f*>(state.samples[frameSlot].mapped);
        for (uint32_t joint = 0; joint < state.paletteCapacity; ++joint)
            palette[sample * state.paletteCapacity + joint] = joint < sampled.palette.size()
                ? sampled.palette[joint] : Eigen::Matrix4f::Identity();
        const auto& colliders = object->GetTressfxSimulationConfig().capsules;
        for (size_t collider = 0; collider < colliders.size(); ++collider)
        {
            Eigen::Matrix4f transform = draw.model;
            if (collider < skinning.capsuleJointIndices.size())
            {
                const int32_t joint = skinning.capsuleJointIndices[collider];
                if (joint >= 0 && static_cast<size_t>(joint) < sampled.palette.size())
                    transform = sampled.palette[joint];
            }
            auto transformed = TransformTressfxCapsuleByBone(colliders[collider], transform);
            transformed.a *= 0.01f;
            transformed.b *= 0.01f;
            transformed.radius *= 0.01f;
            params.capsules[sample][collider] = MakeGuideColliderGpu(transformed);
        }
    }
    if (trace.active)
    {
        nlohmann::json metadata;
        metadata["frame"] = renderScene.frameIndex;
        metadata["algorithm"] = TressfxSimulationSolverName(solverConfig.solver);
        metadata["algorithmVersion"] = algorithmVersion;
        metadata["profileMathVersion"] = TressfxSupportProfileMathVersion(solverConfig.solver);
        metadata["kineticMathVersion"] = kineticMathVersion;
        metadata["numericEncodingVersion"] = numericEncodingVersion;
        metadata["simulationGeneration"] = state.simulationGeneration;
        metadata["resetIsZeroPhysicalTime"] = progressive && state.reset;
        metadata["statePositionSpace"] = progressive ? "u from fixed authored-world-rest; no root advection" : "world-SI";
        metadata["pipelineHotReloadPolicy"] = "graph-owned, no in-place compute replacement";
        metadata["fixedWorldOriginSI"] = {state.fixedWorldOrigin.x(),
            state.fixedWorldOrigin.y(), state.fixedWorldOrigin.z()};
        metadata["snapshot"] = "post-frame; binary flushed two frame slots later";
        metadata["reset"] = state.reset;
        metadata["stepSeconds"] = RodGpuStepSeconds();
        metadata["sweeps"] = RodGpuSweeps;
        metadata["substeps"] = steps;
        metadata["receivedSeconds"] = state.receivedSeconds;
        metadata["dispatchedSeconds"] = state.dispatchedSeconds;
        metadata["acceptedSecondsLastFence"] = state.acceptedSeconds;
        metadata["retainedDebtSeconds"] = state.debtSeconds;
        metadata["gravity"] = {params.gravityStep.x(), params.gravityStep.y(), params.gravityStep.z()};
        metadata["supportEnabled"] = true;
        metadata["windVelocitySI"] = {params.windDrag.x(), params.windDrag.y(), params.windDrag.z()};
        metadata["dragRatePerSpeedSI"] = params.windDrag.w();
        metadata["capsuleCount"] = object->GetTressfxSimulationConfig().capsules.size();
        metadata["sdfPresent"] = object->HasTressfxSdfCollision();
        const auto& section = object->GetImported()->modelResource.sections[
            object->GetImportedSectionIndex()];
        const uint32_t firstGuide = section.tressfxSimulationGuideIndices.empty() ?
            0u : section.tressfxSimulationGuideIndices.front();
        metadata["firstSourceGuide"] = firstGuide;
        metadata["referenceRootWorldColumnMajor"] = nlohmann::json::array();
        for (uint32_t component = 0; component < 16; ++component)
            metadata["referenceRootWorldColumnMajor"].push_back(state.referencePose.root.data()[component]);
        for (const auto& joint : state.referencePose.palette)
        {
            nlohmann::json matrix = nlohmann::json::array();
            for (uint32_t component = 0; component < 16; ++component)
                matrix.push_back(joint.data()[component]);
            metadata["referencePaletteWorldColumnMajor"].push_back(matrix);
        }
        for (uint32_t point = 0; point < 32; ++point)
        {
            const auto& rest = section.vertices[(firstGuide * 32 + point) * 2].position;
            metadata["firstGuideRestSI"].push_back({rest.x() * 0.01, rest.y() * 0.01, rest.z() * 0.01});
            metadata["firstGuideMassMarkers"].push_back(
                section.tressfxInverseMass[firstGuide * 32 + point]);
        }
        if (firstGuide < skinning.strandBones.size())
        {
            const auto packed = TressfxPackStrandBoneWeights(
                skinning.strandBones[firstGuide], skinning.jointIndex);
            for (uint32_t influence = 0; influence < 4; ++influence)
            {
                metadata["firstGuideJoints"].push_back(packed.joints[influence]);
                metadata["firstGuideWeights"].push_back(packed.weights[influence]);
            }
        }
        for (uint32_t sample = 0; sample <= steps; ++sample)
        {
            const RodGpuPose sampled = state.Sample(
                state.dispatchedSeconds + sample * RodGpuStepSeconds());
            nlohmann::json matrix = nlohmann::json::array();
            for (uint32_t entry = 0; entry < 16; ++entry)
                matrix.push_back(sampled.root.data()[entry]);
            metadata["rootsColumnMajor"].push_back(matrix);
            nlohmann::json palette = nlohmann::json::array();
            for (const auto& joint : sampled.palette)
            {
                nlohmann::json jointMatrix = nlohmann::json::array();
                for (uint32_t entry = 0; entry < 16; ++entry)
                    jointMatrix.push_back(joint.data()[entry]);
                palette.push_back(jointMatrix);
            }
            metadata["palettesColumnMajor"].push_back(palette);
        }
        std::ofstream output("artifacts/hair-landing-20261119/gpu-frame-" +
            std::to_string(renderScene.frameIndex) + ".json");
        output << metadata.dump(2);
    }
    const std::vector<Eigen::Matrix4f>* palette = tressfx.frameContext.jointMatrices.empty()
        ? nullptr : &tressfx.frameContext.jointMatrices;
    object->WriteTressfxSkinningPalette(tressfx.frameContext.jointMatrices, frameSlot);
    GuideSimulationParams oldParams = MakeGuideSkinningParams(
        object->GetTressfxGuideCount(), 32, tressfx.frameContext.skinningMatrix, params.flags[2] != 0u);
    oldParams.colliderCount = static_cast<uint32_t>(object->GetTressfxSimulationConfig().capsules.size());
    oldParams.controls.x() = static_cast<float>(RodGpuStepSeconds());
    object->WriteTressfxSimulationParams(oldParams, frameSlot);
    uint32_t dispatchCount = 0;
    const bool sdfValid = object->HasTressfxSdfCollision() &&
        object->WriteTressfxSdfParams(tressfx.frameContext.skinningMatrix, palette, frameSlot);
    if (sdfValid)
    {
        object->WriteTressfxSdfProxyPalette(tressfx.frameContext.jointMatrices, frameSlot);
        params.flags[3] |= 1u;
        RecordPass(commandBuffer, GuideSimulationPass::SdfClear,
            tressfx.sets[PassIndex(GuideSimulationPass::SdfClear)][frameSlot],
            (object->GetTressfxSdfCellCount() + 63) / 64, anyDispatch, dispatchCount);
        RecordPass(commandBuffer, GuideSimulationPass::SdfSplat,
            tressfx.sets[PassIndex(GuideSimulationPass::SdfSplat)][frameSlot],
            (object->GetTressfxSdfTriangleCount() + 63) / 64, anyDispatch, dispatchCount);
    }
    std::memcpy(state.params[frameSlot].mapped, &params, sizeof(params));
    if (anyDispatch)
        RecordInterPassBarrier(commandBuffer);
    // 有 SDF 代理场时 V3 走声明了 SDF 绑定的那条 pipeline（kind 3 的 descriptor set）；
    // 该帧 SDF 域无效时只是 flags.w 的 bit0 不置位，shader 内不做 SDF 查询。
    const bool sdfSolve = object->HasTressfxSdfCollision();
    auto& solver = rodPipelines[progressive ? (sdfSolve ? 6 : 4) :
        (sdfSolve ? 1 : 0)];
    solver->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
        solver->GetPipelineLayout(), 0, progressive && sdfSolve ?
            state.solveSdfSets[frameSlot] : state.solveSets[frameSlot], nullptr);
    solver->Dispatch(commandBuffer, state.guides, 1, 1);
    anyDispatch = true;
    if (object->HasTressfxFollowBinding())
    {
        RecordInterPassBarrier(commandBuffer);
        auto& follow = rodPipelines[progressive ? 5 : 2];
        follow->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
            follow->GetPipelineLayout(), 0, state.followSets[frameSlot], nullptr);
        follow->Dispatch(commandBuffer,
            object->GetTressfxGuideCount(), 1, 1);
    }
    if (sdfValid)
    {
        RecordInterPassBarrier(commandBuffer);
        rodPipelines[3]->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
            rodPipelines[3]->GetPipelineLayout(), 0, state.sdfHistorySets[frameSlot], nullptr);
        rodPipelines[3]->Dispatch(commandBuffer,
            (object->GetTressfxSdfCellCount() + 63) / 64, 1, 1);
        // 本帧结尾才写入历史；下一帧的 solver 才能安全采样。
        state.sdfHistoryValid = true;
    }
    vk::MemoryBarrier transfer;
    transfer.setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eTransfer, {}, transfer, nullptr, nullptr);
    std::vector<vk::BufferCopy> copies;
    copies.reserve(state.guides);
    for (uint32_t guide = 0; guide < state.guides; ++guide)
        copies.emplace_back((static_cast<vk::DeviceSize>(guide) * RodGpuGuideVec4Count +
            RodGpuGuideVec4Count - 4) * 16, static_cast<vk::DeviceSize>(guide) * 64, 64);
    copies.emplace_back(0, static_cast<vk::DeviceSize>(state.guides) * 64,
        RodGpuGuideVec4Count * 16);
    commandBuffer.copyBuffer(state.state.buffer, state.readback[frameSlot].buffer, copies);
    if (progressive)
    {
        const vk::DeviceSize referenceOffset = static_cast<vk::DeviceSize>(state.guides) * 64 +
            RodGpuGuideVec4Count * 16;
        commandBuffer.copyBuffer(state.references.buffer, state.readback[frameSlot].buffer,
            vk::BufferCopy(0, referenceOffset, RodGpuReferenceVec4Count * 16));
        commandBuffer.copyBuffer(state.frameDeltas.buffer, state.readback[frameSlot].buffer,
            vk::BufferCopy(0, referenceOffset + RodGpuReferenceVec4Count * 16, 31 * 16));
        const vk::DeviceSize failureOffset = referenceOffset +
            (RodGpuReferenceVec4Count + 31) * 16;
        commandBuffer.copyBuffer(state.failureMatrix.buffer, state.readback[frameSlot].buffer,
            vk::BufferCopy(0, failureOffset, 932 * 16));
        commandBuffer.copyBuffer(state.failureCandidate.buffer, state.readback[frameSlot].buffer,
            vk::BufferCopy(0, failureOffset + 932 * 16, 416 * 16));
    }
    state.readbackPending[frameSlot] = true;
    state.readbackFrame[frameSlot] = renderScene.frameIndex;
    state.readbackGeneration[frameSlot] = state.simulationGeneration;
    state.readbackSteps[frameSlot] = steps;
    state.dispatchedSeconds += steps * RodGpuStepSeconds();
    state.debtSeconds -= steps * RodGpuStepSeconds();
    while (!state.intervals.empty() && state.intervals.front().end <= state.dispatchedSeconds + 1.0e-9)
        state.intervals.pop_front();
    state.reset = false;
    state.initialized = true;
    tressfx.history.Commit(hasPose, hasPose ? source->skinPose->playbackIdentity : 0,
        hasPose ? source->skinPose->discontinuity : 0);
    if (!state.reported)
    {
        std::cout << "[RodGPU] solver=" << TressfxSimulationSolverName(solverConfig.solver) <<
            " algorithmVersion=" << algorithmVersion << " profileMathVersion=" <<
            profileMathVersion << " kineticMathVersion=" << kineticMathVersion <<
            " numericEncodingVersion=" << numericEncodingVersion << " guides=" << state.guides
            << " stateBytes=" << state.state.size << " step=" << RodGpuStepSeconds()
            << " substepCap=" << RodGpuSubstepCap()
            << " maxSteps=" << RodGpuMaxSteps << " sweeps=" << RodGpuSweeps << std::endl;
        state.reported = true;
    }
}
}
