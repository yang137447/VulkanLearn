#include "particles/particleRendererInternal.h"

namespace VL
{

void ParticleRenderer::FillEmitterSimulationUniformsPrimary(
    EmitterResources& emitter,
    FrameUniforms& uniforms,
    const RenderScene& renderScene,
    const ParticleBoundSpawnAttributes& boundSpawnAttributes,
    const ParticleEmitterCullingResult& culling,
    const std::string& key,
    float spawnPerUnitRate,
    float simulationStepSeconds,
    float viewportHeightPixels,
    float curveTangentScale,
    float& lodRenderScale)
{
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
        lodRenderScale = lod.renderScale;
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
}

void ParticleRenderer::FillEmitterSimulationUniformsRendering(
    EmitterResources& emitter,
    FrameUniforms& uniforms,
    const ParticleBoundSpawnAttributes& boundSpawnAttributes,
    bool meshLightingEnabled,
    bool billboardLightingEnabled,
    const Eigen::Vector3f& lightDirection,
    const Eigen::Vector3f& lightColor,
    float curveTangentScale)
{
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
}

void ParticleRenderer::FillEmitterSimulationUniformsAuxiliary(
    EmitterResources& emitter,
    FrameUniforms& uniforms,
    const RenderScene& renderScene,
    uint32_t simulationSteps)
{
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
}

} // namespace VL
