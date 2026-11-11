#include "particles/particleRendererInternal.h"

namespace VL
{


void ParticleRenderer::RecordSimulation(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const RenderScene& renderScene,
    vk::Extent2D renderExtent)
{
    // frameIndex 与 elapsed 都保留跨帧累计值；elapsed 只累加非负的
    // 模拟 dt。统计重置发生在任何早退之前，便于诊断空场景。
    const uint64_t previousFrameIndex = frameStats.frameIndex;
    const double previousElapsedSeconds = frameStats.elapsedSeconds;
    frameStats = ParticleFrameStats{};
    frameStats.frameIndex = previousFrameIndex + 1;
    const float dt = std::clamp(
        renderScene.simulationDeltaSeconds, 0.0f, 0.1f);
    if (std::isfinite(dt) && dt > 0.0f)
    {
        frameStats.elapsedSeconds =
            previousElapsedSeconds + static_cast<double>(dt);
    }
    else
    {
        frameStats.elapsedSeconds = previousElapsedSeconds;
    }

    // 请求无论本帧是否可执行都只消费一次；隐藏/禁用/空场景不会把旧命令
    // 带到后续帧。
    std::vector<PendingParticleRequest> runtimeRequests =
        std::move(pendingParticleRequests);
    pendingParticleRequests.clear();

    if (!initialized || emitters.empty())
    {
        return;
    }

    // 上一帧顶点着色器读取过粒子池，compute 也读取粒子池与 pair 表；跨提交的
    // semaphore 不表达 storage buffer 的 memory dependency，因此用一条全局
    // MemoryBarrier 把 vertex read -> compute read/write 一起隔开，覆盖两个 SSBO。
    vk::MemoryBarrier readToWrite;
    readToWrite
        .setSrcAccessMask(vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(
            vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eVertexShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        readToWrite,
        nullptr,
        nullptr);

    // 粒子时间来自 WorldSnapshot，由脚本子系统按 Pause/timeScale 计算；
    // 不再使用眼适应的 wall-clock delta，否则编辑器暂停时粒子仍会推进。
    const std::array<Eigen::Vector4f, 6> frustumPlanes =
        BuildParticleFrustumPlanes(
            renderScene.camera.viewProjection);
    const float projectionScaleY =
        std::abs(renderScene.camera.projection(1, 1));
    const float viewportHeightPixels =
        static_cast<float>(renderExtent.height);
    UpdateTranslucencyVolume(renderScene);
    // parentEmitter 子节点读取父节点本帧的 spawn/death/collision 记录，
    // 因此模拟顺序必须显式按依赖深度排序；unordered_map 迭代顺序不能
    // 代表发射器树。深度相同的节点仍按 key 排序，保证确定性。
    const auto parentKeyOf = [&](const std::string& childKey)
        -> std::string {
        const auto childIt = emitters.find(childKey);
        if (childIt == emitters.end() ||
            childIt->second->asset.spawnSource.mode != "parentEmitter")
        {
            return {};
        }
        const size_t separator = childKey.find_last_of('|');
        if (separator == std::string::npos)
        {
            return {};
        }
        const std::string parentKey =
            childKey.substr(0, separator + 1) +
            EscapeParticleEmitterKeySegment(
                childIt->second->asset.spawnSource.parentEmitter);
        return emitters.find(parentKey) != emitters.end()
            ? parentKey
            : std::string();
    };
    std::unordered_map<std::string, size_t> emitterDepths;
    std::unordered_set<std::string> depthVisiting;
    std::function<size_t(const std::string&)> computeDepth =
        [&](const std::string& emitterKey) -> size_t {
        const auto cached = emitterDepths.find(emitterKey);
        if (cached != emitterDepths.end())
        {
            return cached->second;
        }
        // 循环引用时不递归，按根节点处理；资产校验会拒绝缺失父节点，
        // 但运行时代码仍不能因为作者错误而栈溢出。
        if (!depthVisiting.insert(emitterKey).second)
        {
            return 0;
        }
        const std::string parentKey = parentKeyOf(emitterKey);
        const size_t depth = parentKey.empty()
            ? 0
            : computeDepth(parentKey) + 1;
        depthVisiting.erase(emitterKey);
        emitterDepths.emplace(emitterKey, depth);
        return depth;
    };
    std::vector<std::string> simulationOrder;
    simulationOrder.reserve(emitters.size());
    for (const auto& [key, emitterPtr] : emitters)
    {
        (void)emitterPtr;
        simulationOrder.push_back(key);
        (void)computeDepth(key);
    }
    std::sort(
        simulationOrder.begin(),
        simulationOrder.end(),
        [&](const std::string& lhs, const std::string& rhs)
        {
            const size_t lhsDepth = emitterDepths.at(lhs);
            const size_t rhsDepth = emitterDepths.at(rhs);
            return lhsDepth != rhsDepth
                ? lhsDepth < rhsDepth
                : lhs < rhs;
        });
    // 骨骼粒子姿态采样结果的单帧缓存：同一 (资产, 片段, 时间) 只采样一次，
    // 多个发射器共享同一份模型矩阵。
    const Animation::SkeletalAsset* skeletalPoseFrameAsset = nullptr;
    std::size_t skeletalPoseFrameClip = 0;
    float skeletalPoseFrameTime = 0.0f;
    std::shared_ptr<const std::vector<Eigen::Matrix4f>>
        skeletalPoseFrameModels;
    for (const std::string& key : simulationOrder)
    {
        const auto emitterIt = emitters.find(key);
        if (emitterIt == emitters.end())
        {
            continue;
        }
        EmitterResources& emitter = *emitterIt->second;
        ++frameStats.totalEmitters;
        frameStats.totalParticles += emitter.asset.maxParticles;
        // decal gather 输出只在上一次使用本 image 的提交完成后才可读；
        // 本函数前面已经等过上一提交 fence，因此这里解析的是安全快照。
        ReadbackDecalGather(emitter, imageIndex);
        if (emitter.frameUniforms.size() <= imageIndex)
        {
            if (emitter.asset.spawnPerUnit > 0.0f)
            {
                emitter.lastSpawnUnitPosition =
                    emitter.model.block<3, 1>(0, 3);
                emitter.hasSpawnUnitPosition = true;
                emitter.pendingSpawnUnitDistance = 0.0f;
            }
            emitter.culled = true;
            ClearParticleRuntimeCounters(commandBuffer, emitter);
            ++frameStats.culledEmitters;
            continue;
        }
        // 每 image 每帧回收上一轮 query，并重置本轮时间戳。即使随后被
        // 剔除，也保持计时器状态与帧边界同步。
        emitter.gpuTimer.BeginFrame(commandBuffer, imageIndex);
        const Eigen::Vector3f currentSpawnUnitPosition =
            emitter.model.block<3, 1>(0, 3);
        const ParticleBoundSpawnAttributes boundSpawnAttributes =
            BindParticleSpawnAttributes(
                emitter.asset, renderScene.particleDataChannels);
        const auto trackSpawnUnitPosition = [&](bool simulate) {
            if (boundSpawnAttributes.spawnPerUnit <= 0.0f)
            {
                emitter.pendingSpawnUnitDistance = 0.0f;
                return;
            }
            if (!emitter.hasSpawnUnitPosition ||
                emitter.needsClear || emitter.prewarm)
            {
                emitter.lastSpawnUnitPosition =
                    currentSpawnUnitPosition;
                emitter.hasSpawnUnitPosition = true;
                emitter.pendingSpawnUnitDistance = 0.0f;
                return;
            }
            if (!simulate)
            {
                emitter.lastSpawnUnitPosition =
                    currentSpawnUnitPosition;
                emitter.pendingSpawnUnitDistance = 0.0f;
                return;
            }
            emitter.pendingSpawnUnitDistance +=
                (currentSpawnUnitPosition -
                    emitter.lastSpawnUnitPosition).norm();
            emitter.lastSpawnUnitPosition =
                currentSpawnUnitPosition;
        };
        // 作者隐藏、发射器禁用与视锥剔除共用"冻结"路径：不推进 elapsed/prewarm，
        // 不清池、不排序、不 dispatch，也不进入 RecordGraphics 的绘制列表。
        if (!emitter.visible || !emitter.enabled)
        {
            trackSpawnUnitPosition(false);
            emitter.culled = true;
            ClearParticleRuntimeCounters(commandBuffer, emitter);
            ++frameStats.culledEmitters;
            continue;
        }
        const ParticleEmitterCullingResult culling =
            CullParticleEmitter(
                ParticleEmitterCullingInput{
                    emitter.worldCenter,
                    emitter.boundsRadius,
                    emitter.asset.maxDrawDistance,
                    emitter.asset.fadeDistance,
                    emitter.asset.minScreenSize},
                frustumPlanes,
                renderScene.camera.position,
                renderScene.camera.forward,
                projectionScaleY,
                viewportHeightPixels);
        // decal 没有粒子图形 draw，旧的 CPU 路径也从不按粒子包围球剔除；
        // GPU 迁移后继续推进模拟，避免大型贴花因小于粒子 bounds 而消失。
        emitter.culled =
            !culling.visible &&
            emitter.asset.renderMode != "decal";
        if (emitter.culled)
        {
            // 冻结整个发射器状态：不推进时间、不写 UBO、不清池、不 dispatch。
            trackSpawnUnitPosition(false);
            ClearParticleRuntimeCounters(commandBuffer, emitter);
            ++frameStats.culledEmitters;
            continue;
        }
        ++frameStats.visibleEmitters;
        emitter.culled = false;
        trackSpawnUnitPosition(true);

        // 骨骼网格粒子：GPU skinning。CPU 只按当前姿态写每 image 的 palette
        // （jointModel · inverseBind），顶点着色器在绘制时完成蒙皮。
        //
        // 姿态时间来自 emitter 自己的 elapsedSeconds（不是一个全局时钟），
        // 同一资产的不同发射器因此可以各自停在不同帧。采样结果按
        // (资产, 片段, 时间) 缓存，同帧多次命中只算一次。
        //
        // palette 是逐 swapchain image 的缓冲，"已写入"判定也必须逐 image
        // 记录。姿态未变时只能跳过 palette 重写，不能 continue：固定步长、
        // UBO 写入、dispatch 与绘制都在后面，rate=0 的发射器同样必须推进
        // 模拟。历史实现用 continue 跳过整段更新，导致静止姿态下粒子冻结，
        // 资源侧只能把 rate 写成 1.0 绕过。
        if (!emitter.skeletalSections.empty() && emitter.skeletalAsset)
        {
            const auto& asset = *emitter.skeletalAsset;
            float poseTime = emitter.skeletalPoseTime;
            if (std::abs(emitter.skeletalPoseRate) > 1.0e-6f)
            {
                const float duration =
                    asset.clips.empty() ||
                        emitter.skeletalClipIndex >= asset.clips.size()
                    ? 0.0f
                    : (asset.clips.at(emitter.skeletalClipIndex).runtime
                        ? asset.clips.at(emitter.skeletalClipIndex)
                              .runtime->duration()
                        : 0.0f);
                poseTime = WrapParticleSkeletalPoseTime(
                    emitter.skeletalPoseTime,
                    emitter.elapsedSeconds,
                    emitter.skeletalPoseRate,
                    duration,
                    emitter.skeletalPoseLoop);
            }
            const ParticleSkeletalPoseKey poseKey{
                &asset,
                emitter.skeletalClipIndex,
                poseTime};
            if (imageIndex >= emitter.skeletalPoseKeys.size())
            {
                emitter.skeletalPoseKeys.resize(imageIndex + 1);
            }
            const ParticleSkeletalPoseKey& imagePoseKey =
                emitter.skeletalPoseKeys[imageIndex];
            // 只有本 image 的 palette 已经写过同一姿态时才可跳过重写。
            const bool poseUnchanged =
                imagePoseKey == poseKey &&
                imagePoseKey.asset != nullptr &&
                !emitter.skeletalSections.front().skinPalettes.empty();
            if (!poseUnchanged)
            {
                if (skeletalPoseFrameAsset != poseKey.asset ||
                    skeletalPoseFrameClip != poseKey.clipIndex ||
                    skeletalPoseFrameTime != poseKey.time)
                {
                    skeletalPoseFrameModels = SampleParticleSkeletalModels(
                        asset, poseKey.clipIndex, poseTime, false);
                    skeletalPoseFrameAsset = poseKey.asset;
                    skeletalPoseFrameClip = poseKey.clipIndex;
                    skeletalPoseFrameTime = poseKey.time;
                }
                const std::vector<Eigen::Matrix4f>& models =
                    *skeletalPoseFrameModels;
                for (EmitterResources::SkeletalSection& section :
                     emitter.skeletalSections)
                {
                    if (imageIndex >= section.skinPalettes.size())
                    {
                        continue;
                    }
                    const size_t jointCount = std::max<size_t>(
                        section.inverseBinds.size(), 1u);
                    const vk::DeviceSize paletteBytes =
                        static_cast<vk::DeviceSize>(jointCount) *
                        sizeof(Eigen::Matrix4f) * 2u;
                    void* mapped = backend->MapMemory(
                        section.skinPalettes.at(imageIndex).memory,
                        paletteBytes);
                    if (mapped == nullptr)
                    {
                        continue;
                    }
                    auto* writer = static_cast<Eigen::Matrix4f*>(mapped);
                    for (size_t joint = 0; joint < jointCount; ++joint)
                    {
                        Eigen::Matrix4f palette =
                            Eigen::Matrix4f::Identity();
                        if (joint < models.size() &&
                            joint < section.inverseBinds.size())
                        {
                            palette =
                                models[joint] * section.inverseBinds[joint];
                        }
                        // 第二份矩阵是上一帧姿态，供 shader 需要时做逐顶点
                        // 插值；当前实现只用 current，previous 写同一值保持
                        // 布局稳定。
                        writer[joint] = palette;
                        writer[jointCount + joint] = palette;
                    }
                    backend->UnmapMemory(
                        section.skinPalettes.at(imageIndex).memory);
                }
                emitter.skeletalPoseKeys[imageIndex] = poseKey;
            }
        }

        // 固定步长：帧间余量进入 accumulator，只推进完整步；最多 8 步/帧
        // 防止长时间停顿后一次补太多。余量保留到下一帧，避免时间漂移。
        ParticleFixedStepState fixedStepState{
            emitter.fixedStepAccumulator,
            emitter.fixedStepIndex};
        const ParticleFixedStepTick fixedStepTick =
            AdvanceParticleFixedStep(
                dt,
                emitter.asset.fixedTimeStep,
                8u,
                fixedStepState);
        emitter.fixedStepAccumulator = fixedStepState.accumulator;
        emitter.fixedStepIndex = fixedStepState.stepIndex;
        const float simulationStepSeconds =
            fixedStepTick.stepSeconds;
        const uint32_t simulationSteps = fixedStepTick.steps;
        const float simulationFrameSeconds =
            simulationStepSeconds *
            static_cast<float>(simulationSteps);
        float spawnPerUnitRate = 0.0f;
        if (simulationFrameSeconds > 0.0f &&
            emitter.pendingSpawnUnitDistance > 0.0f)
        {
            spawnPerUnitRate = ParticleSpawnPerUnitRate(
                boundSpawnAttributes.spawnPerUnit,
                emitter.pendingSpawnUnitDistance,
                simulationFrameSeconds);
            emitter.pendingSpawnUnitDistance = 0.0f;
        }
        if (emitter.asset.fixedTimeStep > 0.0f)
        {
            emitter.elapsedSeconds +=
                simulationFrameSeconds;
        }
        else
        {
            emitter.elapsedSeconds += dt;
        }

        // mesh/billboard 照明只取场景第一个方向光：方向光的 direction 是
        // 光线传播方向，表面指向光源需要取反。颜色使用线性 RGB，不乘 lux
        // 强度，避免粒子颜色与灯光量级错位；作者关闭时保持旧 unlit 输出。
        const bool wantsMeshLighting =
            emitter.asset.renderMode == "mesh" &&
            emitter.asset.meshLighting;
        // ribbon 与 billboard 共用同一份场景光照合同（ribbon 用面向相机的
        // 视线法线），因此这里必须同时接纳两种 renderMode；否则
        // particleRibbon.frag 的 billboardLightParams.x 永远为 0。
        const bool wantsBillboardLighting =
            (emitter.asset.renderMode == "billboard" ||
             emitter.asset.renderMode == "ribbon") &&
            emitter.asset.billboardLighting;
        bool meshLightingEnabled = false;
        bool billboardLightingEnabled = false;
        Eigen::Vector3f lightDirection =
            Eigen::Vector3f(0.0f, -1.0f, 0.0f);
        Eigen::Vector3f lightColor = Eigen::Vector3f::Ones();
        if (wantsMeshLighting || wantsBillboardLighting)
        {
            for (const LightSnapshot& light : renderScene.lights)
            {
                if (light.type != LightSnapshotType::Directional)
                    continue;
                // 零方向/零光色会让 GGX 结果退化成黑块；文档承诺回退 unlit，
                // 因此在启用前同时检查方向与颜色，而不是只检查存在性。
                if (light.direction.squaredNorm() <= 1.0e-8f ||
                    light.color.maxCoeff() <= 0.0f)
                    continue;
                lightDirection = -light.direction;
                lightColor = light.color;
                meshLightingEnabled = wantsMeshLighting;
                billboardLightingEnabled = wantsBillboardLighting;
                break;
            }
        }
        emitter.lightingEnabled = meshLightingEnabled;
        emitter.lightDirection = lightDirection;
        emitter.lightColor = lightColor;
        emitter.ambient = emitter.asset.meshAmbient;

        FrameUniforms uniforms{};
        const float curveTangentScale = ParticleCurveTangentScale(
            emitter.asset.curveInterpolation);
        float lodRenderScale = 1.0f;
        FillEmitterSimulationUniformsPrimary(
            emitter, uniforms, renderScene, boundSpawnAttributes,
            culling, key, spawnPerUnitRate, simulationStepSeconds,
            viewportHeightPixels, curveTangentScale, lodRenderScale);
        FillEmitterSimulationUniformsRendering(
            emitter, uniforms, boundSpawnAttributes,
            meshLightingEnabled, billboardLightingEnabled,
            lightDirection, lightColor, curveTangentScale);
        FillEmitterSimulationUniformsAuxiliary(
            emitter, uniforms, renderScene, simulationSteps);
        std::memcpy(
            emitter.frameUniforms.at(imageIndex).mapped,
            &uniforms,
            sizeof(uniforms));

        RecordEmitterClearAndCounters(commandBuffer, emitter);

        // beam 是纯渲染模式：弧的形状完全来自 UBO，不消费粒子池、
        // 不参与局部/全局排序。跳过 simulate/sort/finalize，避免为零
        // 渲染的粒子池做无意义计算；计数与 overdraw 缓冲仍已清零。
        if (emitter.asset.renderMode == "beam")
        {
            emitter.prewarm = false;
            continue;
        }

        RecordEmitterRuntimeRequests(
            commandBuffer, emitter, runtimeRequests);
        simulatePipeline->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            simulatePipeline->GetPipelineLayout(),
            0,
            emitter.computeSets.at(imageIndex),
            nullptr);
        const uint32_t groupCount =
            (emitter.asset.maxParticles + 255u) / 256u;
        simulatePipeline->Dispatch(
            commandBuffer,
            groupCount,
            1,
            1);

        // decal 只产出 DBuffer 快照，不进入粒子排序/绘制/光照；模拟后
        // 立刻 gather 并复制到 host-visible 读回缓冲。
        if (emitter.asset.renderMode == "decal")
        {
            RecordDecalGather(
                commandBuffer,
                emitter,
                imageIndex,
                lodRenderScale);
            emitter.prewarm = false;
            continue;
        }

        // light 发射器只产光，不进入排序/绘制。emit 读取 simulate 刚写入
        // 的粒子池，并写全局 light 输出 SSBO；两个 compute 之间必须显式
        // 建立 shaderWrite -> shaderRead 依赖。
        if (emitter.asset.renderMode == "light")
        {
            if (particleLightEmitPipeline &&
                emitter.lightEmitSets.size() > imageIndex)
            {
                InsertBufferBarrier(
                    commandBuffer,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderWrite,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eShaderRead |
                        vk::AccessFlagBits::eShaderWrite);
                particleLightEmitPipeline->Bind(commandBuffer);
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eCompute,
                    particleLightEmitPipeline->GetPipelineLayout(),
                    0,
                    emitter.lightEmitSets.at(imageIndex),
                    nullptr);
                particleLightEmitPipeline->Dispatch(
                    commandBuffer,
                    groupCount,
                    1,
                    1);
            }
            emitter.prewarm = false;
            continue;
        }

        // 参与全局排序的发射器跳过自己的 bitonic 排序，统一在帧末做一次
        // 跨发射器排序。
        const bool globalSorted = IsGlobalSortEmitter(key);
        if (emitter.asset.sortParticles && !globalSorted)
        {
            ++frameStats.sortedEmitters;
            frameStats.sortCapacityTotal += emitter.sortCapacity;
            frameStats.sortDispatchCount +=
                ParticleSortDispatchCount(emitter.sortCapacity);
            // 模拟写入 particle buffer，sort init 读取同一段数据并写入 pairA。
            InsertBufferBarrier(
                commandBuffer,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderWrite,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderRead |
                    vk::AccessFlagBits::eShaderWrite);
            sortInitPipeline->Bind(commandBuffer);
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute,
                sortInitPipeline->GetPipelineLayout(),
                0,
                emitter.sortInitSets.at(imageIndex),
                nullptr);
            const uint32_t sortGroupCount =
                (emitter.sortCapacity + 255u) / 256u;
            sortInitPipeline->Dispatch(
                commandBuffer,
                sortGroupCount,
                1,
                1);

            bool sourceIsA = true;
            for (uint32_t k = 2;
                 k <= emitter.sortCapacity;
                 k <<= 1)
            {
                for (uint32_t j = k >> 1; j > 0; j >>= 1)
                {
                    // 上一 pass 仍在读取 sortPass；先结束读再覆盖该 UBO 区域。
                    InsertBufferBarrier(
                        commandBuffer,
                        vk::PipelineStageFlagBits::eComputeShader,
                        vk::AccessFlagBits::eShaderRead,
                        vk::PipelineStageFlagBits::eTransfer,
                        vk::AccessFlagBits::eTransferWrite);
                    const Eigen::Vector4f sortPass(
                        static_cast<float>(k),
                        static_cast<float>(j),
                        static_cast<float>(emitter.sortCapacity),
                        0.0f);
                    commandBuffer.updateBuffer(
                        emitter.frameUniforms.at(imageIndex).buffer,
                        offsetof(FrameUniforms, sortPass),
                        sizeof(sortPass),
                        &sortPass);
                    // 同时发布上一 pass 的 pair 写入和本次 sortPass 传输写入。
                    InsertBufferBarrier(
                        commandBuffer,
                        vk::PipelineStageFlagBits::eComputeShader |
                            vk::PipelineStageFlagBits::eTransfer,
                        vk::AccessFlagBits::eShaderWrite |
                            vk::AccessFlagBits::eTransferWrite,
                        vk::PipelineStageFlagBits::eComputeShader,
                        vk::AccessFlagBits::eShaderRead |
                            vk::AccessFlagBits::eShaderWrite);

                    sortPipeline->Bind(commandBuffer);
                    commandBuffer.bindDescriptorSets(
                        vk::PipelineBindPoint::eCompute,
                        sortPipeline->GetPipelineLayout(),
                        0,
                        sourceIsA
                            ? emitter.sortAtoBSets.at(imageIndex)
                            : emitter.sortBtoASets.at(imageIndex),
                        nullptr);
                    sortPipeline->Dispatch(
                        commandBuffer,
                        sortGroupCount,
                        1,
                        1);
                    sourceIsA = !sourceIsA;
                }
            }
        }

        // GPU 收尾：把原子 alive 计数写回 indirect args.instanceCount。
        // 后续 graphics 直接 drawIndirect，不再使用 CPU 侧容量/存活猜测。
        InsertBufferBarrier(
            commandBuffer,
            vk::PipelineStageFlagBits::eComputeShader,
            vk::AccessFlagBits::eShaderWrite,
            vk::PipelineStageFlagBits::eComputeShader,
            vk::AccessFlagBits::eShaderRead |
                vk::AccessFlagBits::eShaderWrite);
        finalizeCountPipeline->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            finalizeCountPipeline->GetPipelineLayout(),
            0,
            emitter.finalizeSets.at(imageIndex),
            nullptr);
        finalizeCountPipeline->Dispatch(commandBuffer, 1, 1, 1);

        // prewarm 只允许第一帧；若录制后提交失败，下一帧会按正常发射补齐，
        // 不会重复把整池粒子重置成同一寿命分布。
        emitter.prewarm = false;
    }

    RecordGlobalParticleSort(
        commandBuffer, imageIndex, renderScene);
    BuildParticleDrawOrder(imageIndex, renderScene);
    CollectParticleFrameGpuStats();
}

void ParticleRenderer::RecordEmitterClearAndCounters(
    vk::CommandBuffer commandBuffer,
    EmitterResources& emitter)
{
        if (emitter.needsClear)
        {
            commandBuffer.fillBuffer(
                emitter.particles.buffer,
                0,
                VK_WHOLE_SIZE,
                0);
            vk::BufferMemoryBarrier clearToCompute;
            clearToCompute
                .setBuffer(emitter.particles.buffer)
                .setOffset(0)
                .setSize(VK_WHOLE_SIZE)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(
                    vk::AccessFlagBits::eShaderRead |
                    vk::AccessFlagBits::eShaderWrite);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eComputeShader,
                {},
                nullptr,
                clearToCompute,
                nullptr);
            emitter.needsClear = false;
        }

        // 每帧清零 alive/spawn/death/collision 计数。上一帧 finalize 读过
        // counters，因此先做 shaderWrite -> transferWrite 依赖。
        {
            vk::MemoryBarrier computeToTransfer;
            computeToTransfer
                .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eComputeShader,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                computeToTransfer,
                nullptr,
                nullptr);
            commandBuffer.fillBuffer(
                emitter.counters.buffer,
                0,
                sizeof(uint32_t) * 5u,
                0);
            commandBuffer.fillBuffer(
                emitter.debugCounters.buffer,
                0,
                sizeof(ParticleOverdrawCounters),
                0);
            // parentEmitter 子节点只消费当前帧事件，header 每帧清零。
            commandBuffer.fillBuffer(
                emitter.spawnEvents.buffer,
                0,
                sizeof(uint32_t) * 4u,
                0);
            // 骨骼完整 mesh 用 direct drawIndexed（实例数=maxParticles），
            // 未使用槽位必须指向无效索引，vertex shader 才会裁掉。
            if (!emitter.skeletalSections.empty())
            {
                commandBuffer.fillBuffer(
                    emitter.aliveIndices.buffer,
                    0,
                    VK_WHOLE_SIZE,
                    0xFFFFFFFFu);
            }
            vk::BufferMemoryBarrier clearToCompute;
            clearToCompute
                .setBuffer(emitter.counters.buffer)
                .setOffset(0)
                .setSize(VK_WHOLE_SIZE)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(
                    vk::AccessFlagBits::eShaderRead |
                    vk::AccessFlagBits::eShaderWrite);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eComputeShader,
                {},
                nullptr,
                {clearToCompute,
                 vk::BufferMemoryBarrier(
                     vk::AccessFlagBits::eTransferWrite,
                     vk::AccessFlagBits::eShaderRead |
                         vk::AccessFlagBits::eShaderWrite,
                     VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED,
                     emitter.spawnEvents.buffer,
                     0,
                     VK_WHOLE_SIZE),
                 vk::BufferMemoryBarrier(
                     vk::AccessFlagBits::eTransferWrite,
                     vk::AccessFlagBits::eShaderRead,
                     VK_QUEUE_FAMILY_IGNORED,
                     VK_QUEUE_FAMILY_IGNORED,
                     emitter.aliveIndices.buffer,
                     0,
                     VK_WHOLE_SIZE)},
                nullptr);
            vk::BufferMemoryBarrier debugClearToGraphics;
            debugClearToGraphics
                .setBuffer(emitter.debugCounters.buffer)
                .setOffset(0)
                .setSize(VK_WHOLE_SIZE)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
                .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eFragmentShader,
                {},
                nullptr,
                debugClearToGraphics,
                nullptr);
        }
}

void ParticleRenderer::RecordEmitterRuntimeRequests(
    vk::CommandBuffer commandBuffer,
    EmitterResources& emitter,
    const std::vector<PendingParticleRequest>& runtimeRequests)
{
        // 上一个 emitter 的 spawnEvents/粒子池写入必须先对当前 dispatch
        // 可见；parentEmitter 子节点读取父节点同一帧的事件记录。
        InsertBufferBarrier(
            commandBuffer,
            vk::PipelineStageFlagBits::eComputeShader,
            vk::AccessFlagBits::eShaderWrite,
            vk::PipelineStageFlagBits::eComputeShader,
            vk::AccessFlagBits::eShaderRead |
                vk::AccessFlagBits::eShaderWrite);

        // 本 emitter 的 kill/spawn 请求在 dispatch 前覆盖写；粒子上传的是
        // 模拟空间位置，localSpace 发射器在这里把 console 的世界坐标转回去。
        if (!runtimeRequests.empty() &&
            emitter.runtimeRequests.mapped != nullptr)
        {
            auto* header = static_cast<uint32_t*>(
                emitter.runtimeRequests.mapped);
            auto* records = reinterpret_cast<
                ParticleRuntimeRequestGpu*>(header + 4);
            uint32_t requestCount = 0;
            Eigen::Matrix4f inverseModel =
                Eigen::Matrix4f::Identity();
            if (emitter.asset.localSpace)
            {
                inverseModel = emitter.model.inverse();
            }
            for (const PendingParticleRequest& request :
                 runtimeRequests)
            {
                if (request.emitterKey != emitter.key ||
                    requestCount >= 16u)
                {
                    continue;
                }
                ParticleRuntimeRequestGpu& out =
                    records[requestCount++];
                out.positionLifetime = Eigen::Vector4f::Zero();
                out.kind = request.kill ? 0u : 1u;
                out.particleIndex = request.particleIndex;
                out.reserved0 = 0;
                out.reserved1 = 0;
                if (!request.kill)
                {
                    Eigen::Vector3f position = request.position;
                    if (emitter.asset.localSpace)
                    {
                        position = (inverseModel *
                            Eigen::Vector4f(
                                position.x(),
                                position.y(),
                                position.z(),
                                1.0f)).head<3>();
                    }
                    out.positionLifetime = Eigen::Vector4f(
                        position.x(),
                        position.y(),
                        position.z(),
                        request.lifetime);
                }
            }
            header[0] = requestCount;
            header[1] = 0;
            header[2] = 0;
            header[3] = 0;
        }
        else
        {
            auto* header = static_cast<uint32_t*>(
                emitter.runtimeRequests.mapped);
            if (header != nullptr)
            {
                header[0] = 0;
            }
        }
}

void ParticleRenderer::RecordGlobalParticleSort(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const RenderScene& renderScene)
{
    // 跨发射器 gather + 一次全局 bitonic + finalize。只有存在参与发射器
    // 时才执行；否则池保持 inactive。
    if (globalSortPool.active)
    {
        globalSortPool.gpuTimer.BeginFrame(commandBuffer, imageIndex);
    }
    RecordGlobalGather(commandBuffer, imageIndex);
    RecordGlobalSort(commandBuffer, imageIndex, renderScene);

    vk::MemoryBarrier writeToRead;
    writeToRead
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(
            vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eIndirectCommandRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eFragmentShader |
            vk::PipelineStageFlagBits::eDrawIndirect,
        {},
        writeToRead,
        nullptr,
        nullptr);
}

void ParticleRenderer::BuildParticleDrawOrder(
    uint32_t imageIndex,
    const RenderScene& renderScene)
{
    std::vector<ParticleEmitterDepthKey> depthKeys;
    depthKeys.reserve(emitters.size() + 1u);
    for (const auto& [key, emitterPtr] : emitters)
    {
        const EmitterResources& emitter = *emitterPtr;
        ParticleEmitterDepthKey depthKey;
        depthKey.key = key;
        depthKey.worldCenter = emitter.worldCenter;
        depthKey.worldRadius = emitter.boundsRadius;
        depthKey.sortOrder = emitter.asset.sortOrder;
        depthKeys.push_back(std::move(depthKey));
    }
    // 全局 alpha 池不是一个真实 emitter，但必须作为一个 draw item 参与
    // 同一份排序；否则它只能在所有逐发射器之后录制，重新出现“烟永远
    // 盖住碎块/弹壳”的问题。
    // 只统计本帧可真实 gather 的成员（缺失/被剔除/本 image UBO 未就绪
    // 的成员不会产生顶点，不能影响整池位置）。池整体取最小 sortOrder
    // 与池的背景边缘深度，近似边界见 TryMakeParticleGlobalSortDrawKey。
    if (globalSortPool.active)
    {
        std::vector<ParticleEmitterDepthKey> globalMembers;
        globalMembers.reserve(globalSortPool.emitterKeys.size());
        for (const std::string& memberKey : globalSortPool.emitterKeys)
        {
            const auto memberIt = emitters.find(memberKey);
            if (memberIt == emitters.end())
            {
                continue;
            }
            const EmitterResources& member = *memberIt->second;
            if (member.culled ||
                member.frameUniforms.size() <= imageIndex)
            {
                continue;
            }
            ParticleEmitterDepthKey memberKeyData;
            memberKeyData.key = memberKey;
            memberKeyData.worldCenter = member.worldCenter;
            memberKeyData.worldRadius = member.boundsRadius;
            memberKeyData.sortOrder = member.asset.sortOrder;
            globalMembers.push_back(std::move(memberKeyData));
        }
        ParticleEmitterDepthKey globalKey;
        if (TryMakeParticleGlobalSortDrawKey(
                globalMembers,
                renderScene.camera.position,
                renderScene.camera.forward,
                globalKey))
        {
            depthKeys.push_back(std::move(globalKey));
        }
    }
    const std::vector<size_t> sortedIndices =
        ComputeParticleEmitterDrawOrder(
            depthKeys,
            renderScene.camera.position,
            renderScene.camera.forward);
    drawOrder.clear();
    drawOrder.reserve(sortedIndices.size());
    for (const size_t index : sortedIndices)
    {
        drawOrder.push_back(std::move(depthKeys[index].key));
    }
}

void ParticleRenderer::CollectParticleFrameGpuStats()
{
    // 计时快照来自各 emitter 在上一轮同 image 的 query；按 key 排序发布，
    // 避免 unordered_map 迭代顺序让 stats JSON 抖动。
    frameStats.emitterGpu.clear();
    frameStats.emitterGpu.reserve(emitters.size() + 1u);
    for (const auto& [key, emitterPtr] : emitters)
    {
        const ParticleGpuTimingSnapshot& snapshot =
            emitterPtr->gpuTimer.GetSnapshot();
        frameStats.emitterGpu.push_back(ParticleFrameStats::EmitterGpu{
            key,
            snapshot.supported,
            snapshot.lastMilliseconds,
            snapshot.averageMilliseconds,
            snapshot.maxMilliseconds,
            snapshot.sampleCount});
    }
    if (globalSortPool.active)
    {
        const ParticleGpuTimingSnapshot& snapshot =
            globalSortPool.gpuTimer.GetSnapshot();
        frameStats.emitterGpu.push_back(ParticleFrameStats::EmitterGpu{
            "__global__",
            snapshot.supported,
            snapshot.lastMilliseconds,
            snapshot.averageMilliseconds,
            snapshot.maxMilliseconds,
            snapshot.sampleCount});
    }
    std::sort(
        frameStats.emitterGpu.begin(),
        frameStats.emitterGpu.end(),
        [](const ParticleFrameStats::EmitterGpu& left,
           const ParticleFrameStats::EmitterGpu& right)
        {
            return left.key < right.key;
        });
}

} // namespace VL
