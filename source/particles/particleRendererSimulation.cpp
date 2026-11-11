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
        uniforms.curveInterpolationParams = Eigen::Vector4f(
            static_cast<float>(ParticleCurveInterpolationCode(
                emitter.asset.curveInterpolation)),
            0.0f,
            0.0f,
            0.0f);
        uniforms.viewProjection =
            renderScene.camera.viewProjection;
        uniforms.view = renderScene.camera.view;
        uniforms.invProjection =
            renderScene.camera.projection.inverse();
        uniforms.cameraPosition = Eigen::Vector4f(
            renderScene.camera.position.x(),
            renderScene.camera.position.y(),
            renderScene.camera.position.z(),
            1.0f);
        uniforms.cameraRight = Eigen::Vector4f(
            renderScene.camera.right.x(),
            renderScene.camera.right.y(),
            renderScene.camera.right.z(),
            0.0f);
        uniforms.cameraUp = Eigen::Vector4f(
            renderScene.camera.up.x(),
            renderScene.camera.up.y(),
            renderScene.camera.up.z(),
            0.0f);
        uniforms.localToWorld = emitter.model;
        uniforms.gravityDrag = Eigen::Vector4f(
            boundSpawnAttributes.gravity.x(),
            boundSpawnAttributes.gravity.y(),
            boundSpawnAttributes.gravity.z(),
            emitter.asset.drag);
        uniforms.velocityMinLifetimeMin = Eigen::Vector4f(
            emitter.asset.velocityMin.x(),
            emitter.asset.velocityMin.y(),
            emitter.asset.velocityMin.z(),
            emitter.asset.lifetimeMin);
        uniforms.velocityMaxLifetimeMax = Eigen::Vector4f(
            emitter.asset.velocityMax.x(),
            emitter.asset.velocityMax.y(),
            emitter.asset.velocityMax.z(),
            emitter.asset.lifetimeMax);
        // LOD 同时缩放连续发射预算与渲染尺寸；池容量不变。
        // burst/prewarm 的整池填充在 shader 里不读这个预算，因此不受影响。
        const float lodDistance =
            (emitter.worldCenter - renderScene.camera.position).norm();
        const ParticleLodState lod =
            EffectiveParticleLod(emitter.asset, lodDistance);
        uniforms.simulation = Eigen::Vector4f(
            (boundSpawnAttributes.spawnRate + spawnPerUnitRate) *
                lod.spawnRateScale * emitter.spawnRateScale,
            simulationStepSeconds,
            emitter.elapsedSeconds,
            emitter.prewarm ? 1.0f : 0.0f);
        uniforms.render = Eigen::Vector4f(
            boundSpawnAttributes.sizeStart,
            boundSpawnAttributes.sizeEnd,
            emitter.asset.softness,
            boundSpawnAttributes.opacity * culling.opacityScale);
        // 只传 renderScale/blend；alpha 仍由 culling.opacityScale 独占，
        // 避免 LOD 与距离 fade 重复相乘。
        uniforms.lodParams = Eigen::Vector4f(
            lod.renderScale,
            lod.blend,
            0.0f,
            0.0f);
        uniforms.shape = Eigen::Vector4f(
            emitter.asset.shapeRadius,
            emitter.asset.burst ? 1.0f : 0.0f,
            emitter.asset.localSpace ? 1.0f : 0.0f,
            0.0f);
        uniforms.softParams = Eigen::Vector4f(
            emitter.asset.softParticleDistance,
            emitter.asset.softParticles ? 1.0f : 0.0f,
            emitter.asset.softParticleTapRadius,
            emitter.asset.softParticleThickness);
        uniforms.colorStart = emitter.asset.colorStart;
        uniforms.colorEnd = emitter.asset.colorEnd;
        const bool hasColorKeys =
            !emitter.asset.colorKeys.empty();
        const size_t colorKeyCount = hasColorKeys
            ? std::min<size_t>(
                emitter.asset.colorKeys.size(),
                kMaxParticleCurveKeys)
            : 2;
        uniforms.colorTangentIn.fill(Eigen::Vector4f::Zero());
        uniforms.colorTangentOut.fill(Eigen::Vector4f::Zero());
        for (size_t index = 0; index < colorKeyCount; ++index)
        {
            if (hasColorKeys)
            {
                uniforms.colorKeys[index] =
                    emitter.asset.colorKeys[index].color;
                uniforms.colorKeyTimes[index / 4][index % 4] =
                    emitter.asset.colorKeys[index].time;
                uniforms.colorTangentIn[index] =
                    emitter.asset.colorKeys[index].tangentIn *
                    curveTangentScale;
                uniforms.colorTangentOut[index] =
                    emitter.asset.colorKeys[index].tangentOut *
                    curveTangentScale;
            }
            else
            {
                uniforms.colorKeys[index] = index == 0
                    ? emitter.asset.colorStart
                    : emitter.asset.colorEnd;
                uniforms.colorKeyTimes[index / 4][index % 4] =
                    index == 0 ? 0.0f : 1.0f;
            }
        }

        const bool hasSizeKeys =
            !emitter.asset.sizeKeys.empty();
        const size_t sizeKeyCount = hasSizeKeys
            ? std::min<size_t>(
                emitter.asset.sizeKeys.size(),
                kMaxParticleCurveKeys)
            : 2;
        for (size_t index = 0; index < sizeKeyCount; ++index)
        {
            if (hasSizeKeys)
            {
                uniforms.sizeKeys[index] = Eigen::Vector4f(
                    emitter.asset.sizeKeys[index].time,
                    emitter.asset.sizeKeys[index].size,
                    emitter.asset.sizeKeys[index].tangentIn *
                        curveTangentScale,
                    emitter.asset.sizeKeys[index].tangentOut *
                        curveTangentScale);
            }
            else
            {
                uniforms.sizeKeys[index] = Eigen::Vector4f(
                    index == 0 ? 0.0f : 1.0f,
                    index == 0
                        ? boundSpawnAttributes.sizeStart
                        : boundSpawnAttributes.sizeEnd,
                    0.0f,
                    0.0f);
            }
        }
        uniforms.curveParams = Eigen::Vector4f(
            static_cast<float>(colorKeyCount),
            static_cast<float>(sizeKeyCount),
            static_cast<float>(emitter.asset.flipbookColumns),
            static_cast<float>(emitter.asset.flipbookRows));
        uniforms.flipbookParams = Eigen::Vector4f(
            emitter.asset.flipbookFramesPerSecond,
            emitter.asset.texturePath.empty() ? 0.0f : 1.0f,
            static_cast<float>(emitter.asset.flipbookStartFrame),
            static_cast<float>(emitter.asset.flipbookEndFrame));
        uniforms.flipbookParams2 = Eigen::Vector4f(
            emitter.asset.flipbookLoop ? 1.0f : 0.0f,
            emitter.asset.flipbookRandomStartFrame ? 1.0f : 0.0f,
            0.0f,
            0.0f);
        uniforms.flipbookParams5 = Eigen::Vector4f(
            emitter.asset.flipbookPerParticle ? 1.0f : 0.0f,
            emitter.asset.flipbookBlend ? 1.0f : 0.0f,
            emitter.asset.flipbookPlayRate,
            0.0f);
        uniforms.flipbookPlayRateKeys.fill(
            Eigen::Vector4f(0.0f, 1.0f, 0.0f, 0.0f));
        uniforms.flipbookUvOffsetKeys.fill(Eigen::Vector4f::Zero());
        uniforms.flipbookUvScaleKeys.fill(
            Eigen::Vector4f(0.0f, 0.0f, 1.0f, 1.0f));
        uniforms.flipbookRateKeyTimes.fill(Eigen::Vector4f::Zero());
        uniforms.flipbookUvKeyTimes.fill(Eigen::Vector4f::Zero());
        const size_t rateKeyCount = std::min<size_t>(
            emitter.asset.flipbookPlayRateKeys.size(),
            kMaxParticleCurveKeys);
        for (size_t index = 0; index < rateKeyCount; ++index)
        {
            const ParticleFlipbookRateKey& key =
                emitter.asset.flipbookPlayRateKeys[index];
            uniforms.flipbookPlayRateKeys[index] = Eigen::Vector4f(
                key.time, key.rate, 0.0f, 0.0f);
            uniforms.flipbookRateKeyTimes[index / 4][index % 4] =
                key.time;
        }
        const size_t offsetKeyCount = std::min<size_t>(
            emitter.asset.flipbookUvOffsetKeys.size(),
            kMaxParticleCurveKeys);
        for (size_t index = 0; index < offsetKeyCount; ++index)
        {
            const ParticleFlipbookUvKey& key =
                emitter.asset.flipbookUvOffsetKeys[index];
            uniforms.flipbookUvOffsetKeys[index] = Eigen::Vector4f(
                key.time, 0.0f, key.value.x(), key.value.y());
            uniforms.flipbookUvKeyTimes[index / 4][index % 4] =
                key.time;
        }
        const size_t scaleKeyCount = std::min<size_t>(
            emitter.asset.flipbookUvScaleKeys.size(),
            kMaxParticleCurveKeys);
        for (size_t index = 0; index < scaleKeyCount; ++index)
        {
            const ParticleFlipbookUvKey& key =
                emitter.asset.flipbookUvScaleKeys[index];
            uniforms.flipbookUvScaleKeys[index] = Eigen::Vector4f(
                key.time, 0.0f, key.value.x(), key.value.y());
            // offset/scale 共享 UV 时间轴；两张表同时存在时作者应使用
            // 相同 time（loader 分别校验，渲染取 scale 的 time）。
            uniforms.flipbookUvKeyTimes[index / 4][index % 4] =
                key.time;
        }
        uniforms.flipbookParams6 = Eigen::Vector4f(
            static_cast<float>(rateKeyCount),
            static_cast<float>(offsetKeyCount),
            static_cast<float>(scaleKeyCount),
            emitter.asset.flipbookStartFrameJitter);
        uniforms.textureModeParams = Eigen::Vector4f(
            emitter.asset.eooPacked ? 1.0f : 0.0f,
            emitter.asset.eooEmissiveStrength,
            0.0f,
            0.0f);
        const auto spawnModeCode = [](const std::string& mode) {
            if (mode == "skeletalTris") return 1.0f;
            if (mode == "skeletalBones") return 2.0f;
            if (mode == "parentEmitter") return 3.0f;
            return 0.0f;
        };
        const auto parentEventCode = [](const std::string& event) {
            if (event == "spawn") return 0.0f;
            if (event == "collision") return 2.0f;
            return 1.0f;
        };
        // 作者没写 spawnSource 时，骨骼粒子模式默认从自己的骨骼网格采样；
        // 载入的网格不是骨骼网格时把模式降为 none，让粒子回落到 shape 采样，
        // 而不是永远读一个空的 spawn 源。
        std::string spawnMode = emitter.asset.spawnSource.mode;
        const bool skeletalRenderMode =
            emitter.asset.renderMode == "skeletalMeshTris" ||
            emitter.asset.renderMode == "skeletalMeshBones";
        if (emitter.skeletalSections.empty() &&
            (spawnMode == "skeletalTris" || spawnMode == "skeletalBones"))
        {
            spawnMode = "none";
        }
        uniforms.spawnSourceParams = Eigen::Vector4f(
            spawnModeCode(spawnMode),
            parentEventCode(emitter.asset.spawnSource.parentEvent),
            static_cast<float>(emitter.asset.spawnSource.perEvent),
            emitter.asset.spawnSource.normalVelocityScale);
        uniforms.spawnSourceOffset = Eigen::Vector4f(
            emitter.asset.spawnSource.offset.x(),
            emitter.asset.spawnSource.offset.y(),
            emitter.asset.spawnSource.offset.z(),
            0.0f);
        uniforms.burstTiming = Eigen::Vector4f(
            emitter.asset.burstDelay,
            emitter.asset.burstPhase,
            0.0f,
            0.0f);
        uniforms.spawnEventOutput = Eigen::Vector4f(
            emitter.emitSpawnEvents ? 1.0f : 0.0f,
            0.0f,
            0.0f,
            0.0f);
        uniforms.dissolveParams = Eigen::Vector4f(
            emitter.asset.dissolve.start,
            emitter.asset.dissolve.end,
            emitter.asset.dissolve.edgeWidth,
            emitter.asset.dissolve.noiseScale);
        uniforms.dissolveParams2 = Eigen::Vector4f(
            emitter.asset.dissolve.edgeStrength,
            emitter.asset.dissolve.noiseSpeed,
            emitter.asset.dissolve.enabled ? 1.0f : 0.0f,
            0.0f);
        // 溶解外推沿用作者的溶解区间：可见性在 lifetime × start 开始下降，
        // 到 lifetime × end 消失；这里把同一段归一化寿命映射成外壳推进量，
        // 让面片在变透明的同时微微膨胀，而不是先鼓起来再消失。
        const float dissolveStart = std::clamp(
            emitter.asset.dissolve.start, 0.0f, 1.0f);
        const float dissolveSpan = std::max(
            std::clamp(emitter.asset.dissolve.end, 0.0f, 1.0f) -
                dissolveStart,
            1.0e-4f);
        // 寿命在这一帧的平均推进位置即外推阶段；由 GPU 逐粒子计算会多占
        // 一个 varying，这里用发射器平均寿命估计，视觉上足够连续。
        const float averageLifetime = 0.5f *
            (emitter.asset.lifetimeMin + emitter.asset.lifetimeMax);
        const float puffPhase = averageLifetime > 1.0e-4f
            ? std::clamp(
                (emitter.elapsedSeconds - dissolveStart * averageLifetime) /
                    (dissolveSpan * averageLifetime),
                0.0f,
                1.0f)
            : 0.0f;
        uniforms.puffParams = Eigen::Vector4f(
            puffPhase,
            emitter.asset.dissolve.enabled
                ? std::clamp(
                    emitter.asset.dissolve.edgeWidth * 4.0f,
                    0.0f,
                    1.0f)
                : 0.0f,
            emitter.asset.dissolve.enabled ? 1.0f : 0.0f,
            0.0f);
        uniforms.dissolveEdgeColor = Eigen::Vector4f(
            emitter.asset.dissolve.edgeColor.x(),
            emitter.asset.dissolve.edgeColor.y(),
            emitter.asset.dissolve.edgeColor.z(),
            0.0f);
        uniforms.translucencyVolumeMin = Eigen::Vector4f(
            -4000.0f, -1500.0f, -4000.0f,
            1.0f);
        uniforms.translucencyVolumeInvExtent = Eigen::Vector4f(
            1.0f / 8000.0f,
            1.0f / 8000.0f,
            1.0f / 8000.0f,
            0.12f);
        uniforms.sortParams = Eigen::Vector4f(
            emitter.asset.sortParticles ? 1.0f : 0.0f,
            static_cast<float>(emitter.asset.maxParticles),
            static_cast<float>(emitter.sortCapacity),
            0.0f);
        uniforms.sortPass = Eigen::Vector4f(
            0.0f,
            0.0f,
            static_cast<float>(emitter.sortCapacity),
            0.0f);
        uniforms.screenParams = Eigen::Vector4f(
            viewportHeightPixels,
            emitter.asset.maxScreenSize,
            // prewarm 的稳态池填充使用作者 spawnRate 乘实例倍率；LOD 只
            // 影响 simulation.x 的连续发射预算。
            boundSpawnAttributes.spawnRate * emitter.spawnRateScale,
            globalSortPool.active
                ? static_cast<float>(
                    globalSortPool.textureSlotByKey.count(key) > 0
                    ? globalSortPool.textureSlotByKey.at(key)
                    : 0u)
                : 0.0f);
        uniforms.flipbookFrameDurations = emitter.flipbookTable.durations;
        uniforms.flipbookFramePivots = emitter.flipbookTable.pivots;
        uniforms.flipbookParams3 = emitter.flipbookTable.flags;
        uniforms.flipbookFrameUVs = emitter.flipbookTable.uvs;
        const Eigen::Vector3f collisionPlaneNormal =
            NormalizedParticleCollisionPlaneNormal(emitter.asset);
        uniforms.collisionParams = Eigen::Vector4f(
            static_cast<float>(ParticleCollisionTypeValue(
                emitter.asset.collisionType)),
            emitter.asset.collisionBounce,
            emitter.asset.collisionFriction,
            emitter.asset.collisionRadius);
        uniforms.collisionPlane = Eigen::Vector4f(
            collisionPlaneNormal.x(),
            collisionPlaneNormal.y(),
            collisionPlaneNormal.z(),
            emitter.asset.collisionPlaneOffset);
        uniforms.collisionBoxMin = Eigen::Vector4f(
            emitter.asset.collisionBoxMin.x(),
            emitter.asset.collisionBoxMin.y(),
            emitter.asset.collisionBoxMin.z(),
            emitter.asset.killOnCollision ? 1.0f : 0.0f);
        uniforms.collisionBoxMax = Eigen::Vector4f(
            emitter.asset.collisionBoxMax.x(),
            emitter.asset.collisionBoxMax.y(),
            emitter.asset.collisionBoxMax.z(),
            0.0f);
        uniforms.collisionParams2 = Eigen::Vector4f(
            emitter.asset.collisionCcd ? 1.0f : 0.0f,
            emitter.asset.collisionCapsuleRadius,
            0.0f,
            0.0f);
        uniforms.collisionCapsuleA = Eigen::Vector4f(
            emitter.asset.collisionCapsuleA.x(),
            emitter.asset.collisionCapsuleA.y(),
            emitter.asset.collisionCapsuleA.z(),
            0.0f);
        uniforms.collisionCapsuleB = Eigen::Vector4f(
            emitter.asset.collisionCapsuleB.x(),
            emitter.asset.collisionCapsuleB.y(),
            emitter.asset.collisionCapsuleB.z(),
            0.0f);
        uniforms.burstParams = Eigen::Vector4f(
            emitter.asset.burstInterval,
            static_cast<float>(EffectiveParticleBurstCount(
                emitter.asset.maxParticles,
                boundSpawnAttributes.burstCount)),
            0.0f,
            0.0f);
        uniforms.rotationParams = Eigen::Vector4f(
            emitter.asset.rotation,
            emitter.asset.rotationSpeed,
            emitter.asset.rotationRandom ? 1.0f : 0.0f,
            0.0f);
        uniforms.flipbookFrameRotations =
            emitter.flipbookTable.rotations;
        uniforms.flipbookParams4 =
            emitter.flipbookTable.rotationFlags;
        uniforms.flipbookPivotKeys =
            emitter.flipbookTable.pivotKeys;
        uniforms.flipbookPivotTangentIn =
            emitter.flipbookTable.pivotTangentIn;
        uniforms.flipbookPivotTangentOut =
            emitter.flipbookTable.pivotTangentOut;
        uniforms.flipbookRotationKeys =
            emitter.flipbookTable.rotationKeys;
        uniforms.flipbookTimelineParams =
            emitter.flipbookTable.timelineParams;
        uniforms.ribbonParams = Eigen::Vector4f(
            static_cast<float>(
                ClampParticleRibbonSegments(
                    emitter.asset.ribbonSegments)),
            emitter.asset.ribbonFade,
            emitter.asset.ribbonWidth,
            // 只有 ribbon 需要 16 槽历史搬移；其它模式保持原模拟成本。
            emitter.asset.renderMode == "ribbon" ? 1.0f : 0.0f);
        uniforms.ribbonShapeParams = Eigen::Vector4f(
            emitter.asset.ribbonPivot,
            emitter.asset.ribbonTwist,
            0.0f,
            0.0f);
        uniforms.ribbonUVParams = Eigen::Vector4f(
            emitter.asset.ribbonUVTiling,
            static_cast<float>(std::min<size_t>(
                emitter.asset.ribbonWidthKeys.size(),
                kMaxParticleCurveKeys)),
            static_cast<float>(std::min<size_t>(
                emitter.asset.ribbonColorKeys.size(),
                kMaxParticleCurveKeys)),
            0.0f);
        uniforms.ribbonWidthKeys.fill(Eigen::Vector4f::Zero());
        uniforms.ribbonWidthKeyTimes.fill(Eigen::Vector4f::Zero());
        uniforms.ribbonColorTangentIn.fill(Eigen::Vector4f::Zero());
        uniforms.ribbonColorTangentOut.fill(Eigen::Vector4f::Zero());
        for (size_t keyIndex = 0;
             keyIndex < emitter.asset.ribbonWidthKeys.size() &&
             keyIndex < kMaxParticleCurveKeys;
             ++keyIndex)
        {
            const ParticleRibbonWidthKey& key =
                emitter.asset.ribbonWidthKeys[keyIndex];
            uniforms.ribbonWidthKeys[keyIndex] =
                Eigen::Vector4f(
                    key.time,
                    key.width,
                    key.tangentIn * curveTangentScale,
                    key.tangentOut * curveTangentScale);
            uniforms.ribbonWidthKeyTimes[keyIndex / 4][keyIndex % 4] =
                key.time;
        }
        uniforms.ribbonColorKeys.fill(Eigen::Vector4f::Zero());
        uniforms.ribbonColorKeyTimes.fill(Eigen::Vector4f::Zero());
        for (size_t keyIndex = 0;
             keyIndex < emitter.asset.ribbonColorKeys.size() &&
             keyIndex < kMaxParticleCurveKeys;
             ++keyIndex)
        {
            const ParticleRibbonColorKey& key =
                emitter.asset.ribbonColorKeys[keyIndex];
            uniforms.ribbonColorKeys[keyIndex] = key.color;
            uniforms.ribbonColorTangentIn[keyIndex] =
                key.tangentIn * curveTangentScale;
            uniforms.ribbonColorTangentOut[keyIndex] =
                key.tangentOut * curveTangentScale;
            uniforms.ribbonColorKeyTimes[keyIndex / 4][keyIndex % 4] =
                key.time;
        }
        uniforms.meshParams = Eigen::Vector4f(
            static_cast<float>(ParticleMeshAlignmentCode(
                emitter.asset.meshAlignment)),
            emitter.asset.skeletalBoneRadius,
            emitter.asset.meshUseSizeCurve ? 1.0f : 0.0f,
            emitter.asset.renderMode == "mesh" &&
                    !emitter.staticSections.empty()
                ? static_cast<float>(emitter.staticSections.size())
                : 1.0f);
        uniforms.meshScale = Eigen::Vector4f(
            emitter.asset.meshScale.x(),
            emitter.asset.meshScale.y(),
            emitter.asset.meshScale.z(),
            emitter.asset.meshSizeScale);
        uniforms.colorTint = emitter.colorTint;
        const int billboardAlignmentCode =
            ParticleBillboardAlignmentCode(
                emitter.asset.billboardAlignment);
        uniforms.billboardParams = Eigen::Vector4f(
            static_cast<float>(billboardAlignmentCode),
            0.0f,
            0.0f,
            0.0f);
        uniforms.shapeParams = Eigen::Vector4f(
            static_cast<float>(
                ParticleShapeCode(emitter.asset.shapeType)),
            emitter.asset.shapeHeight,
            emitter.asset.shapeInnerRadius,
            0.0f);
        uniforms.shapeExtent = Eigen::Vector4f(
            emitter.asset.shapeExtent.x(),
            emitter.asset.shapeExtent.y(),
            emitter.asset.shapeExtent.z(),
            0.0f);
        uniforms.curlParams = Eigen::Vector4f(
            emitter.asset.curlNoiseStrength,
            emitter.asset.curlNoiseFrequency,
            emitter.asset.curlNoiseSpeed,
            0.0f);
        uniforms.vortexParams = Eigen::Vector4f(
            emitter.asset.vortexStrength,
            emitter.asset.vortexRadius,
            0.0f,
            0.0f);
        uniforms.vortexAxis = Eigen::Vector4f(
            emitter.asset.vortexAxis.x(),
            emitter.asset.vortexAxis.y(),
            emitter.asset.vortexAxis.z(),
            0.0f);
        uniforms.attractorParams = Eigen::Vector4f(
            emitter.asset.attractorStrength,
            emitter.asset.attractorRadius,
            0.0f,
            0.0f);
        uniforms.attractorPosition = Eigen::Vector4f(
            emitter.asset.attractorPosition.x(),
            emitter.asset.attractorPosition.y(),
            emitter.asset.attractorPosition.z(),
            0.0f);
        uniforms.wind = Eigen::Vector4f(
            boundSpawnAttributes.wind.x(),
            boundSpawnAttributes.wind.y(),
            boundSpawnAttributes.wind.z(),
            0.0f);
        // 每帧整表重写：空表也必须清掉上一份 UBO 内容，count=0 才是
        // “恒为 1”的唯一信号。
        uniforms.lifetimeScaleKeys.fill(Eigen::Vector4f::Zero());
        const size_t lifetimeScaleCount = std::min<size_t>(
            emitter.asset.lifetimeScaleKeys.size(),
            kMaxParticleCurveKeys);
        for (size_t keyIndex = 0;
             keyIndex < lifetimeScaleCount;
             ++keyIndex)
        {
            const ParticleLifetimeScaleKey& key =
                emitter.asset.lifetimeScaleKeys[keyIndex];
            uniforms.lifetimeScaleKeys[keyIndex] =
                Eigen::Vector4f(key.time, key.scale, 0.0f, 0.0f);
        }
        uniforms.lifetimeScaleParams = Eigen::Vector4f(
            static_cast<float>(lifetimeScaleCount),
            0.0f,
            0.0f,
            0.0f);
        uniforms.meshLightParams = Eigen::Vector4f(
            meshLightingEnabled ? 1.0f : 0.0f,
            emitter.asset.meshAmbient,
            0.0f,
            0.0f);
        uniforms.meshLightDirection = Eigen::Vector4f(
            lightDirection.x(),
            lightDirection.y(),
            lightDirection.z(),
            0.0f);
        uniforms.meshLightColor = Eigen::Vector4f(
            lightColor.x(),
            lightColor.y(),
            lightColor.z(),
            0.0f);
        uniforms.materialBaseColor = emitter.material.valid
            ? emitter.material.baseColor
            : Eigen::Vector4f::Ones();
        uniforms.materialEmissiveStrength = Eigen::Vector4f(
            emitter.material.emissive.x(),
            emitter.material.emissive.y(),
            emitter.material.emissive.z(),
            emitter.material.emissiveStrength);
        uniforms.materialPbr = Eigen::Vector4f(
            emitter.material.roughness,
            emitter.material.metallic,
            emitter.material.valid ? 1.0f : 0.0f,
            0.0f);
        uniforms.billboardLightParams = Eigen::Vector4f(
            billboardLightingEnabled ? 1.0f : 0.0f,
            emitter.asset.billboardAmbient,
            // z 只缩放直接光：sprite 是体积感卡片，全量逐像素直接光会过曝；
            // 全局 alpha 池通过 gather record 原样携带该字段。
            std::clamp(
                emitter.asset.billboardLightingStrength,
                0.0f,
                1.0f),
            0.0f);
        uniforms.billboardLightParams2 = Eigen::Vector4f(
            std::clamp(emitter.asset.billboardSpecular, 0.0f, 1.0f),
            std::clamp(emitter.asset.billboardRoughness, 0.02f, 1.0f),
            std::clamp(
                emitter.asset.billboardForwardScattering,
                0.0f,
                2.0f),
            std::clamp(emitter.asset.billboardDensity, 0.0f, 0.25f));
        // beam 端点是发射器局部空间作者数据，渲染前统一乘 emitter.model；
        // localSpace 对 beam 没有额外含义。
        const Eigen::Vector3f beamStartWorld =
            (emitter.model * Eigen::Vector4f(
                emitter.asset.beam.start.x(),
                emitter.asset.beam.start.y(),
                emitter.asset.beam.start.z(),
                1.0f)).head<3>();
        Eigen::Vector3f beamEndWorld =
            (emitter.model * Eigen::Vector4f(
                emitter.asset.beam.end.x(),
                emitter.asset.beam.end.y(),
                emitter.asset.beam.end.z(),
                1.0f)).head<3>();
        if (emitter.asset.beam.hitScan.enabled)
        {
            const float authoredRayLength =
                (beamEndWorld - beamStartWorld).norm();
            const auto hit = IntersectParticlePlane(
                beamStartWorld,
                beamEndWorld,
                emitter.asset.beam.hitScan.planeNormal,
                emitter.asset.beam.hitScan.planeOffset,
                std::min(
                    emitter.asset.beam.hitScan.maxDistance,
                    authoredRayLength));
            if (hit)
            {
                beamEndWorld = hit->position;
            }
        }
        // 端点已经被 model 的尺度放大；宽度/噪声/抖动是局部空间长度，
        // 必须使用同一最大列缩放，否则非 1 缩放下包围球与画面不一致。
        float beamScale = 0.0f;
        const Eigen::Matrix3f beamLinear =
            emitter.model.block<3, 3>(0, 0);
        for (int column = 0; column < 3; ++column)
        {
            beamScale = std::max(
                beamScale,
                beamLinear.col(column).norm());
        }
        uniforms.beamStart = Eigen::Vector4f(
            beamStartWorld.x(),
            beamStartWorld.y(),
            beamStartWorld.z(),
            static_cast<float>(emitter.asset.beam.segments));
        uniforms.beamEnd = Eigen::Vector4f(
            beamEndWorld.x(),
            beamEndWorld.y(),
            beamEndWorld.z(),
            emitter.asset.beam.width * beamScale);
        uniforms.beamNoise = Eigen::Vector4f(
            emitter.asset.beam.noiseAmplitude * beamScale,
            emitter.asset.beam.noiseFrequency,
            emitter.asset.beam.noiseSpeed,
            emitter.asset.beam.endpointJitter * beamScale);
        uniforms.beamParams = Eigen::Vector4f(
            static_cast<float>(emitter.asset.beam.count),
            static_cast<float>(emitter.asset.beam.branchCount),
            emitter.asset.beam.branchStart,
            emitter.asset.beam.branchLength);
        uniforms.beamBranch = Eigen::Vector4f(
            emitter.asset.beam.branchSpread * beamScale,
            0.0f,
            0.0f,
            0.0f);
        // 曲线与逐弧覆盖只对 beam 有意义；其它模式保持 count=0、
        // 恒等 tint/宽度，避免误读未初始化数组。
        uniforms.beamColorKeys.fill(Eigen::Vector4f::Zero());
        uniforms.beamColorKeyTimes.fill(Eigen::Vector4f::Zero());
        uniforms.beamWidthKeys.fill(Eigen::Vector4f::Zero());
        uniforms.beamColorTangentIn.fill(Eigen::Vector4f::Zero());
        uniforms.beamColorTangentOut.fill(Eigen::Vector4f::Zero());
        uniforms.beamCurveParams = Eigen::Vector4f::Zero();
        uniforms.beamArcColorStart.fill(
            Eigen::Vector4f(1.0f, 1.0f, 1.0f, 0.0f));
        uniforms.beamArcColorEnd.fill(
            Eigen::Vector4f(1.0f, 1.0f, 1.0f, 0.0f));
        uniforms.beamArcParams.fill(Eigen::Vector4f(1.0f, 1.0f, 0.0f, 0.0f));
        if (emitter.asset.renderMode == "beam")
        {
            const ParticleBeamAsset& beam = emitter.asset.beam;
            for (size_t index = 0;
                 index < beam.colorKeys.size();
                 ++index)
            {
                uniforms.beamColorKeys[index] =
                    beam.colorKeys[index].color;
                uniforms.beamColorTangentIn[index] =
                    beam.colorKeys[index].tangentIn *
                    curveTangentScale;
                uniforms.beamColorTangentOut[index] =
                    beam.colorKeys[index].tangentOut *
                    curveTangentScale;
                uniforms.beamColorKeyTimes[index / 4][index % 4] =
                    beam.colorKeys[index].time;
            }
            for (size_t index = 0;
                 index < beam.widthKeys.size();
                 ++index)
            {
                uniforms.beamWidthKeys[index] = Eigen::Vector4f(
                    beam.widthKeys[index].time,
                    beam.widthKeys[index].width,
                    beam.widthKeys[index].tangentIn *
                        curveTangentScale,
                    beam.widthKeys[index].tangentOut *
                        curveTangentScale);
            }
            for (size_t index = 0; index < beam.arcs.size(); ++index)
            {
                const ParticleBeamArc& arc = beam.arcs[index];
                uniforms.beamArcColorStart[index] = Eigen::Vector4f(
                    arc.colorStart.x(),
                    arc.colorStart.y(),
                    arc.colorStart.z(),
                    0.0f);
                uniforms.beamArcColorEnd[index] = Eigen::Vector4f(
                    arc.colorEnd.x(),
                    arc.colorEnd.y(),
                    arc.colorEnd.z(),
                    0.0f);
                uniforms.beamArcParams[index] = Eigen::Vector4f(
                    arc.widthScale,
                    arc.intensity,
                    arc.noisePhase,
                    0.0f);
            }
            uniforms.beamCurveParams = Eigen::Vector4f(
                static_cast<float>(beam.colorKeys.size()),
                static_cast<float>(beam.widthKeys.size()),
                0.0f,
                0.0f);
        }
        // fixed billboard 基在发射器局部空间作者化；上传前乘 emitter.model
        // 的线性部分，使其跟随粒子系统/武器的旋转与缩放。其它 alignment
        // 上传默认基；shader 即使误读也拿到有效向量。
        Eigen::Vector3f fixedNormal(0.0f, 0.0f, 1.0f);
        Eigen::Vector3f fixedUp(0.0f, 1.0f, 0.0f);
        if (billboardAlignmentCode == 2)
        {
            const Eigen::Matrix3f linear =
                emitter.model.block<3, 3>(0, 0);
            fixedNormal = linear * emitter.asset.billboardNormal;
            fixedUp = linear * emitter.asset.billboardUp;
            if (!(fixedNormal.squaredNorm() > 1.0e-6f))
            {
                fixedNormal = Eigen::Vector3f(0.0f, 0.0f, 1.0f);
            }
            else
            {
                fixedNormal.normalize();
            }
            if (!(fixedUp.squaredNorm() > 1.0e-6f))
            {
                fixedUp = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
            }
            else
            {
                fixedUp.normalize();
            }
            if (fixedNormal.cross(fixedUp).squaredNorm() <= 1.0e-6f)
            {
                fixedUp = std::abs(fixedNormal.y()) < 0.99f
                    ? Eigen::Vector3f(0.0f, 1.0f, 0.0f)
                    : Eigen::Vector3f(1.0f, 0.0f, 0.0f);
            }
        }
        uniforms.billboardFixedNormal = Eigen::Vector4f(
            fixedNormal.x(),
            fixedNormal.y(),
            fixedNormal.z(),
            0.0f);
        uniforms.billboardFixedUp = Eigen::Vector4f(
            fixedUp.x(),
            fixedUp.y(),
            fixedUp.z(),
            0.0f);
        // 只有 distortion emitter 消费该字段；其它模式保持 0，shader 即使
        // 误读也得到零偏移。
        uniforms.distortionParams = Eigen::Vector4f(
            emitter.asset.renderMode == "distortion"
                ? emitter.asset.distortionStrength
                : 0.0f,
            // y = 真实纹理可用；加载失败时强制走程序化方向场。
            emitter.textureFallback ? 0.0f : 1.0f,
            0.0f,
            0.0f);
        uniforms.particleLightParams = Eigen::Vector4f(
            static_cast<float>(emitter.lightSlotBase),
            static_cast<float>(emitter.lightSlotCapacity),
            emitter.asset.light.intensity,
            emitter.asset.light.radius);
        uniforms.particleLightShadowParams = Eigen::Vector4f(
            emitter.particleLightCastShadows
                ? static_cast<float>(emitter.particleLightShadowSlice)
                : -1.0f,
            1.0f,
            emitter.particleLightShadowFar,
            emitter.particleLightShadowBias);
        uniforms.particleLightIesParams = Eigen::Vector4f(
            static_cast<float>(emitter.iesProfileIndex),
            emitter.iesProfileIndex > 0 ? 1.0f : 0.0f,
            0.0f,
            0.0f);
        // 逐粒子灯亮度/颜色曲线：key 按作者顺序写进 UBO，GPU 端用与
        // EvaluateParticleLightBrightness 相同的分段线性公式求值；空表时
        // key count 为 0，GPU 亮度恒为 1、颜色乘数恒为白（等价于引入曲线
        // 前的行为）。
        for (size_t keyIndex = 0;
             keyIndex < emitter.asset.light.brightnessKeys.size() &&
             keyIndex < kMaxParticleCurveKeys;
             ++keyIndex)
        {
            const ParticleLightBrightnessKey& key =
                emitter.asset.light.brightnessKeys[keyIndex];
            // xy = (lifeT, brightness)；zw 保留。
            uniforms.particleLightBrightnessKeys[keyIndex] =
                Eigen::Vector4f(
                    key.time,
                    key.brightness,
                    0.0f,
                    0.0f);
            uniforms.particleLightBrightnessColors[keyIndex] =
                Eigen::Vector4f(
                    key.color.x(),
                    key.color.y(),
                    key.color.z(),
                    1.0f);
        }
        uniforms.particleLightBrightnessParams = Eigen::Vector4f(
            static_cast<float>(
                emitter.asset.light.brightnessKeys.size()),
            0.0f,
            0.0f,
            0.0f);
        for (int cascade = 0; cascade < 4; ++cascade)
        {
            uniforms.shadowLightViewProj[cascade] =
                shadowCascadeData.valid
                ? shadowCascadeData.lightViewProj[cascade]
                : Eigen::Matrix4f::Identity();
            uniforms.shadowCascadeBias[cascade] =
                shadowCascadeData.valid
                ? shadowCascadeData.bias[cascade]
                : Eigen::Vector4f::Zero();
        }
        uniforms.shadowCascadeSplits =
            shadowCascadeData.valid
            ? shadowCascadeData.cascadeSplits
            : Eigen::Vector4f::Zero();
        uniforms.shadowParams = Eigen::Vector4f(
            shadowCascadeData.valid
                ? shadowCascadeData.csmParameters.z()
                : 0.0f,
            shadowCascadeData.valid
                ? shadowCascadeData.csmParameters.w()
                : 0.0f,
            shadowCascadeData.valid ? 1.0f : 0.0f,
            emitter.asset.billboardShadows ? 1.0f : 0.0f);
        // seed 只保留 24 位，保证 float 往返后仍精确；其余输入都由
        // fixedStepIndex 驱动，不依赖 wall clock。
        uniforms.determinismParams = Eigen::Vector4f(
            static_cast<float>(emitter.asset.randomSeed & 0x00FFFFFFu),
            static_cast<float>(simulationSteps > 0 ? simulationSteps : 1),
            static_cast<float>(emitter.fixedStepIndex),
            emitter.asset.deterministicRandom ? 1.0f : 0.0f);
        uniforms.eventParams = Eigen::Vector4f(
            emitter.asset.emitSpawnEvent ? 1.0f : 0.0f,
            emitter.asset.emitDeathEvent ? 1.0f : 0.0f,
            emitter.asset.emitCollisionEvent ? 1.0f : 0.0f,
            0.0f);
        if (emitter.sdf)
        {
            uniforms.collisionSdfBoundsMin = Eigen::Vector4f(
                emitter.sdf->boundsMin.x(),
                emitter.sdf->boundsMin.y(),
                emitter.sdf->boundsMin.z(),
                0.0f);
            uniforms.collisionSdfBoundsMax = Eigen::Vector4f(
                emitter.sdf->boundsMax.x(),
                emitter.sdf->boundsMax.y(),
                emitter.sdf->boundsMax.z(),
                0.0f);
            uniforms.collisionSdfResolution = Eigen::Vector4f(
                static_cast<float>(emitter.sdf->resolution.x()),
                static_cast<float>(emitter.sdf->resolution.y()),
                static_cast<float>(emitter.sdf->resolution.z()),
                0.0f);
        }
        else
        {
            uniforms.collisionSdfBoundsMin = Eigen::Vector4f::Zero();
            uniforms.collisionSdfBoundsMax = Eigen::Vector4f::Zero();
            uniforms.collisionSdfResolution = Eigen::Vector4f::Zero();
        }
        // World 级 typed Data Channel -> 材质/精灵参数。绑定只在通道存在
        // 时覆盖作者默认值；缺失或类型不兼容保持原值，避免 Console 清空
        // 通道后材质突然变黑/全透明。颜色参数接受标量（灰阶）或 1~4 分量，
        // 标量只写 RGB，不改变原有 alpha。
        if (!emitter.asset.materialBindings.empty())
        {
            const auto readColor = [](const ParticleDataChannelValue& value,
                                      Eigen::Vector4f& out) {
                const size_t count = value.ComponentCount();
                if (value.IsScalar())
                {
                    out.head<3>().setConstant(value.values[0]);
                    return;
                }
                out.x() = value.values[0];
                out.y() = value.values[1];
                out.z() = count >= 3 ? value.values[2] : 0.0f;
                if (count >= 4)
                {
                    out.w() = value.values[3];
                }
            };
            for (const auto& [parameter, channelName] :
                 emitter.asset.materialBindings)
            {
                const auto found =
                    renderScene.particleDataChannels.find(channelName);
                if (found == renderScene.particleDataChannels.end())
                {
                    continue;
                }
                const ParticleDataChannelValue& value = found->second;
                if (parameter == "baseColor")
                {
                    // 颜色通道的数值范围与资产校验一致：RGB 在 [0,64]，
                    // alpha 在 [0,1]。写进 UBO 前夹取，非法通道值不能把
                    // 材质推到 HDR 爆亮或负值。
                    readColor(value, uniforms.materialBaseColor);
                    uniforms.materialBaseColor.head<3>() =
                        uniforms.materialBaseColor.head<3>()
                            .cwiseMax(0.0f)
                            .cwiseMin(64.0f);
                    uniforms.materialBaseColor.w() = std::clamp(
                        uniforms.materialBaseColor.w(), 0.0f, 1.0f);
                }
                else if (parameter == "color")
                {
                    readColor(value, uniforms.colorTint);
                    uniforms.colorTint.head<3>() =
                        uniforms.colorTint.head<3>()
                            .cwiseMax(0.0f)
                            .cwiseMin(64.0f);
                    uniforms.colorTint.w() =
                        std::clamp(uniforms.colorTint.w(), 0.0f, 1.0f);
                }
                else if (parameter == "emissive")
                {
                    const Eigen::Vector4f original =
                        uniforms.materialEmissiveStrength;
                    Eigen::Vector4f color = original;
                    readColor(value, color);
                    uniforms.materialEmissiveStrength =
                        Eigen::Vector4f(
                            color.x(), color.y(), color.z(), original.w());
                }
                else
                {
                    float scalar = 0.0f;
                    if (!value.TryGetFloat(scalar))
                    {
                        continue;
                    }
                    if (parameter == "emissiveStrength")
                    {
                        uniforms.materialEmissiveStrength.w() =
                            std::clamp(scalar, 0.0f, 1.0e4f);
                    }
                    else if (parameter == "roughness")
                    {
                        uniforms.materialPbr.x() =
                            std::clamp(scalar, 0.02f, 1.0f);
                    }
                    else if (parameter == "metallic")
                    {
                        uniforms.materialPbr.y() =
                            std::clamp(scalar, 0.0f, 1.0f);
                    }
                    else if (parameter == "opacity")
                    {
                        const float opacity =
                            std::clamp(scalar, 0.0f, 1.0f);
                        uniforms.materialBaseColor.w() = opacity;
                        uniforms.colorTint.w() = opacity;
                    }
                }
            }
        }
        std::memcpy(
            emitter.frameUniforms.at(imageIndex).mapped,
            &uniforms,
            sizeof(uniforms));

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

        // beam 是纯渲染模式：弧的形状完全来自 UBO，不消费粒子池、
        // 不参与局部/全局排序。跳过 simulate/sort/finalize，避免为零
        // 渲染的粒子池做无意义计算；计数与 overdraw 缓冲仍已清零。
        if (emitter.asset.renderMode == "beam")
        {
            emitter.prewarm = false;
            continue;
        }

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
                lod.renderScale);
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
