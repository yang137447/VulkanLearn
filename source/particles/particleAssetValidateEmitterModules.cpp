#include "particles/particleAssetInternal.h"
#include "particles/particleAssetEmitterInternal.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "particles/particleBeam.h"
#include "particles/particleFlipbook.h"
#include "particles/particleMesh.h"
#include "particles/particleRibbon.h"
#include "particles/particleSdf.h"

namespace VL
{
namespace ParticleAssetDetail
{

void ValidateEmitterSpawnAndMesh(const ParticleEmitterAsset& emitter)
{
    if (!emitter.meshPath.empty())
    {
        RequireValidParticleMeshPath(emitter.meshPath);
    }
    if (emitter.spawnSource.mode != "none" &&
        emitter.spawnSource.mode != "skeletalTris" &&
        emitter.spawnSource.mode != "skeletalBones" &&
        emitter.spawnSource.mode != "parentEmitter")
    {
        throw std::invalid_argument(
            "Particle spawnSource.mode must be none/skeletalTris/"
            "skeletalBones/parentEmitter");
    }
    if (emitter.spawnSource.parentEvent != "spawn" &&
        emitter.spawnSource.parentEvent != "death" &&
        emitter.spawnSource.parentEvent != "collision")
    {
        throw std::invalid_argument(
            "Particle spawnSource.parentEvent must be spawn/death/collision");
    }
    if (emitter.spawnSource.perEvent < 1 ||
        emitter.spawnSource.perEvent > 64)
    {
        throw std::invalid_argument(
            "Particle spawnSource.perEvent must be in [1,64]");
    }
    if ((emitter.spawnSource.mode == "skeletalTris" ||
         emitter.spawnSource.mode == "skeletalBones") &&
        emitter.meshPath.empty())
    {
        throw std::invalid_argument(
            "Particle skeletal spawnSource requires mesh.path");
    }
    if (emitter.spawnSource.mode == "parentEmitter" &&
        emitter.spawnSource.parentEmitter.empty())
    {
        throw std::invalid_argument(
            "Particle spawnSource.parentEmitter must be non-empty");
    }
    RequireFinite(
        emitter.spawnSource.normalVelocityScale,
        "spawnSource.normalVelocityScale");
    if (emitter.spawnSource.normalVelocityScale < 0.0f)
    {
        throw std::invalid_argument(
            "Particle spawnSource.normalVelocityScale must be non-negative");
    }
    if ((emitter.renderMode == "skeletalMeshTris" ||
         emitter.renderMode == "skeletalMeshBones") &&
        emitter.meshPath.empty())
    {
        throw std::invalid_argument(
            "Particle skeletal renderMode requires mesh.path");
    }
    RequireFinite(emitter.skeletalPose.time, "skeletalPose.time");
    RequireFinite(emitter.skeletalPose.rate, "skeletalPose.rate");
    if (emitter.skeletalPose.time < 0.0f ||
        emitter.skeletalPose.rate < -16.0f ||
        emitter.skeletalPose.rate > 16.0f)
    {
        throw std::invalid_argument(
            "Particle skeletalPose.time must be non-negative and "
            "rate in [-16,16]");
    }
    RequireFinite(emitter.skeletalBoneRadius, "skeletalBoneRadius");
    if (emitter.skeletalBoneRadius < 0.01f ||
        emitter.skeletalBoneRadius > 100.0f)
    {
        throw std::invalid_argument(
            "Particle skeletalBoneRadius must be in [0.01, 100]");
    }
    RequireFinite(emitter.dissolve.start, "dissolve.start");
    RequireFinite(emitter.dissolve.end, "dissolve.end");
    RequireFinite(emitter.dissolve.edgeWidth, "dissolve.edgeWidth");
    RequireFinite(emitter.dissolve.edgeStrength, "dissolve.edgeStrength");
    RequireFinite(emitter.dissolve.noiseScale, "dissolve.noiseScale");
    RequireFinite(emitter.dissolve.noiseSpeed, "dissolve.noiseSpeed");
    if (emitter.dissolve.start < 0.0f || emitter.dissolve.start > 1.0f ||
        emitter.dissolve.end < 0.0f || emitter.dissolve.end > 1.0f ||
        emitter.dissolve.end < emitter.dissolve.start ||
        emitter.dissolve.edgeWidth < 0.0f ||
        emitter.dissolve.edgeWidth > 0.5f ||
        emitter.dissolve.edgeStrength < 0.0f ||
        emitter.dissolve.edgeStrength > 32.0f ||
        emitter.dissolve.noiseScale <= 0.0f ||
        emitter.dissolve.noiseScale > 64.0f ||
        emitter.dissolve.noiseSpeed < 0.0f ||
        emitter.dissolve.noiseSpeed > 16.0f)
    {
        throw std::invalid_argument(
            "Particle dissolve ranges are invalid");
    }
    for (int channel = 0; channel < 3; ++channel)
    {
        RequireFinite(emitter.dissolve.edgeColor[channel],
            "dissolve.edgeColor");
    }
    for (const auto& [parameter, channel] : emitter.materialBindings)
    {
        if (parameter != "baseColor" &&
            parameter != "emissive" &&
            parameter != "emissiveStrength" &&
            parameter != "roughness" &&
            parameter != "metallic" &&
            parameter != "opacity" &&
            parameter != "color")
        {
            throw std::invalid_argument(
                "Particle materialBindings has unsupported parameter: " +
                parameter);
        }
        if (channel.empty())
        {
            throw std::invalid_argument(
                "Particle materialBindings channel must be non-empty");
        }
    }
    for (const auto& [parameter, channel] : emitter.attributeBindings)
    {
        if (parameter != "spawnRate" &&
            parameter != "spawnPerUnit" &&
            parameter != "burstCount" &&
            parameter != "gravity" &&
            parameter != "wind" &&
            parameter != "sizeStart" &&
            parameter != "sizeEnd" &&
            parameter != "opacity")
        {
            throw std::invalid_argument(
                "Particle attributeBindings has unsupported parameter: " +
                parameter);
        }
        if (channel.empty())
        {
            throw std::invalid_argument(
                "Particle attributeBindings channel must be non-empty");
        }
    }
    RequireFinite(emitter.meshScale.x(), "mesh.scale.x");
    RequireFinite(emitter.meshScale.y(), "mesh.scale.y");
    RequireFinite(emitter.meshScale.z(), "mesh.scale.z");
    RequireFinite(emitter.meshSizeScale, "mesh.sizeScale");
    if (emitter.meshScale.squaredNorm() <= 1.0e-12f)
    {
        throw std::invalid_argument(
            "Particle mesh.scale must be non-zero");
    }
    if (emitter.meshSizeScale <= 0.0f ||
        emitter.meshSizeScale > 100.0f)
    {
        throw std::invalid_argument(
            "Particle mesh.sizeScale must be in (0, 100]");
    }
    if (!IsParticleMeshAlignment(emitter.meshAlignment))
    {
        throw std::invalid_argument(
            "Particle mesh.alignment must be 'world', 'camera' or 'velocity'");
    }
    RequireFinite(emitter.meshAmbient, "meshAmbient");
    if (emitter.meshAmbient < 0.0f || emitter.meshAmbient > 1.0f)
    {
        throw std::invalid_argument(
            "Particle meshAmbient must be in [0, 1]");
    }
    RequireFinite(emitter.billboardAmbient, "billboardAmbient");
    if (emitter.billboardAmbient < 0.0f ||
        emitter.billboardAmbient > 1.0f)
    {
        throw std::invalid_argument(
            "Particle billboardAmbient must be in [0, 1]");
    }
    RequireFinite(
        emitter.billboardLightingStrength,
        "billboardLightingStrength");
    if (emitter.billboardLightingStrength < 0.0f ||
        emitter.billboardLightingStrength > 1.0f)
    {
        throw std::invalid_argument(
            "Particle billboardLightingStrength must be in [0, 1]");
    }
    RequireFinite(emitter.billboardSpecular, "billboardSpecular");
    if (emitter.billboardSpecular < 0.0f ||
        emitter.billboardSpecular > 1.0f)
    {
        throw std::invalid_argument(
            "Particle billboardSpecular must be in [0, 1]");
    }
    // 下限 0.02：roughness=0 时 GGX 退化成 delta 分布，屏幕上会出现
    // 单像素针状高光；保留一个足够窄但有限的高光核。
    RequireFinite(emitter.billboardRoughness, "billboardRoughness");
    if (emitter.billboardRoughness < 0.02f ||
        emitter.billboardRoughness > 1.0f)
    {
        throw std::invalid_argument(
            "Particle billboardRoughness must be in [0.02, 1]");
    }
    RequireFinite(
        emitter.billboardForwardScattering,
        "billboardForwardScattering");
    if (emitter.billboardForwardScattering < 0.0f ||
        emitter.billboardForwardScattering > 2.0f)
    {
        throw std::invalid_argument(
            "Particle billboardForwardScattering must be in [0, 2]");
    }
    if (emitter.renderMode == "mesh")
    {
        if (emitter.meshPath.empty())
        {
            throw std::invalid_argument(
                "Particle mesh renderMode requires a non-empty mesh.path");
        }
    }
}

void ValidateEmitterSimulation(const ParticleEmitterAsset& emitter)
{
    if (emitter.spawnRate < 0.0f || emitter.spawnRate > 100000.0f)
    {
        throw std::invalid_argument(
            "Particle spawnRate must be in [0, 100000]");
    }
    if (emitter.burstInterval < 0.0f || emitter.burstInterval > 60.0f)
    {
        throw std::invalid_argument(
            "Particle burstInterval must be in [0, 60]");
    }
    if (emitter.burstDelay < 0.0f || emitter.burstDelay > 60.0f ||
        emitter.burstPhase < 0.0f || emitter.burstPhase > 60.0f)
    {
        throw std::invalid_argument(
            "Particle burstDelay/burstPhase must be in [0, 60]");
    }
    if (emitter.burstCount > 0 &&
        emitter.burstCount > emitter.maxParticles)
    {
        throw std::invalid_argument(
            "Particle burstCount must be zero or <= maxParticles");
    }
    if (emitter.spawnPerUnit < 0.0f ||
        emitter.spawnPerUnit > 100000.0f)
    {
        throw std::invalid_argument(
            "Particle spawnPerUnit must be in [0, 100000]");
    }
    if (emitter.lodDistance < 0.0f)
    {
        throw std::invalid_argument(
            "Particle lodDistance must be non-negative");
    }
    if (emitter.lodSpawnRateScale < 0.0f ||
        emitter.lodSpawnRateScale > 1.0f)
    {
        throw std::invalid_argument(
            "Particle lodSpawnRateScale must be in [0, 1]");
    }
    if (emitter.lodTiers.size() > kMaxParticleLodTiers)
    {
        throw std::invalid_argument(
            "Particle lod.tiers must contain at most 4 entries");
    }
    for (size_t index = 0; index < emitter.lodTiers.size(); ++index)
    {
        const ParticleLodTier& tier = emitter.lodTiers[index];
        if (!std::isfinite(tier.distance) ||
            !std::isfinite(tier.spawnRateScale) ||
            !std::isfinite(tier.renderScale) ||
            !std::isfinite(tier.fadeRange))
        {
            throw std::invalid_argument(
                "Particle lod tier values must be finite");
        }
        if (tier.distance < 0.0f)
        {
            throw std::invalid_argument(
                "Particle lod tier distance must be non-negative");
        }
        if (tier.spawnRateScale < 0.0f ||
            tier.spawnRateScale > 1.0f)
        {
            throw std::invalid_argument(
                "Particle lod tier spawnRateScale must be in [0, 1]");
        }
        if (tier.renderScale < 0.0f ||
            tier.renderScale > 4.0f)
        {
            throw std::invalid_argument(
                "Particle lod tier renderScale must be in [0, 4]");
        }
        if (tier.fadeRange < 0.0f)
        {
            throw std::invalid_argument(
                "Particle lod tier fadeRange must be non-negative");
        }
        // 选择规则是“已越过的最远档位”，乱序或重复距离会让档位含义
        // 依赖数组顺序；要求严格递增才能保证结果唯一。
        if (index > 0 &&
            tier.distance <= emitter.lodTiers[index - 1].distance)
        {
            throw std::invalid_argument(
                "Particle lod tier distances must be strictly "
                "increasing");
        }
    }
    if (emitter.lifetimeMin <= 0.0f ||
        emitter.lifetimeMax < emitter.lifetimeMin ||
        emitter.lifetimeMax > 60.0f)
    {
        throw std::invalid_argument(
            "Particle lifetime requires 0 < min <= max <= 60");
    }
    if (emitter.drag < 0.0f || emitter.drag > 20.0f)
    {
        throw std::invalid_argument(
            "Particle drag must be in [0, 20]");
    }
    if (emitter.curlNoiseStrength < 0.0f)
    {
        throw std::invalid_argument(
            "Particle curlNoise.strength must be non-negative");
    }
    if (emitter.curlNoiseFrequency < 0.0f)
    {
        throw std::invalid_argument(
            "Particle curlNoise.frequency must be non-negative");
    }
    if (emitter.vortexStrength < 0.0f)
    {
        throw std::invalid_argument(
            "Particle vortex.strength must be non-negative");
    }
    if (emitter.vortexRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle vortex.radius must be non-negative");
    }
    if (emitter.vortexStrength > 0.0f &&
        emitter.vortexAxis.squaredNorm() <= 1.0e-12f)
    {
        throw std::invalid_argument(
            "Particle vortex.axis must be non-zero when "
            "vortex.strength is positive");
    }
    if (emitter.attractorStrength < 0.0f)
    {
        throw std::invalid_argument(
            "Particle attractor.strength must be non-negative");
    }
    if (emitter.attractorRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle attractor.radius must be non-negative");
    }
    if (emitter.attractorStrength > 0.0f &&
        emitter.attractorRadius <= 0.0f)
    {
        throw std::invalid_argument(
            "Particle attractor.radius must be positive when "
            "attractor.strength is positive");
    }
    if (emitter.shapeRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle shape radius must be non-negative");
    }
    if (emitter.shapeType != "sphere" &&
        emitter.shapeType != "box" &&
        emitter.shapeType != "cone" &&
        emitter.shapeType != "cylinder" &&
        emitter.shapeType != "disc" &&
        emitter.shapeType != "ring")
    {
        throw std::invalid_argument(
            "Particle shape.type must be 'sphere', 'box', 'cone', "
            "'cylinder', 'disc', or 'ring'");
    }
    // 未使用字段仍要求非负，保证切换 shape.type 时不会把资产里的负值
    // 或退化尺寸带进运行时采样。
    for (int axis = 0; axis < 3; ++axis)
    {
        if (emitter.shapeExtent[axis] < 0.0f)
        {
            throw std::invalid_argument(
                "Particle shape extent must be non-negative");
        }
    }
    if (emitter.shapeHeight < 0.0f)
    {
        throw std::invalid_argument(
            "Particle shape height must be non-negative");
    }
    if (emitter.shapeInnerRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle shape innerRadius must be non-negative");
    }
    if (emitter.shapeType == "box" &&
        (emitter.shapeExtent.x() <= 0.0f ||
         emitter.shapeExtent.y() <= 0.0f ||
         emitter.shapeExtent.z() <= 0.0f))
    {
        throw std::invalid_argument(
            "Particle box shape extent must be positive on all axes");
    }
    if ((emitter.shapeType == "cone" ||
         emitter.shapeType == "cylinder") &&
        emitter.shapeHeight <= 0.0f)
    {
        throw std::invalid_argument(
            "Particle cone/cylinder shape height must be positive");
    }
    if (emitter.shapeType == "ring" &&
        (emitter.shapeRadius <= 0.0f ||
         emitter.shapeInnerRadius >= emitter.shapeRadius))
    {
        throw std::invalid_argument(
            "Particle ring shape requires radius > 0 and "
            "0 <= innerRadius < radius");
    }
}

void ValidateEmitterFlipbook(const ParticleEmitterAsset& emitter)
{
    if (emitter.flipbookColumns == 0 || emitter.flipbookColumns > 16 ||
        emitter.flipbookRows == 0 || emitter.flipbookRows > 16)
    {
        throw std::invalid_argument(
            "Particle flipbook columns/rows must be in [1, 16]");
    }
    if (emitter.flipbookFramesPerSecond < 0.0f ||
        emitter.flipbookFramesPerSecond > 240.0f)
    {
        throw std::invalid_argument(
            "Particle flipbook fps must be in [0, 240]");
    }
    if (emitter.flipbookPlayRate < 0.0f ||
        emitter.flipbookPlayRate > 16.0f)
    {
        throw std::invalid_argument(
            "Particle flipbook playRate must be in [0, 16]");
    }
    if (emitter.flipbookStartFrameJitter < 0.0f ||
        emitter.flipbookStartFrameJitter > 256.0f)
    {
        throw std::invalid_argument(
            "Particle flipbook startFrameJitter must be in [0, 256]");
    }
    auto validateUvCurve = [](const std::vector<ParticleFlipbookUvKey>& keys,
                              const char* field)
    {
        float previousTime = -1.0f;
        for (const ParticleFlipbookUvKey& key : keys)
        {
            RequireFinite(key.time, field);
            RequireFinite(key.value.x(), field);
            RequireFinite(key.value.y(), field);
            if (key.time < 0.0f || key.time > 1.0f ||
                key.time <= previousTime)
            {
                throw std::invalid_argument(
                    std::string("Particle flipbook ") + field +
                    " times must be in [0,1] and strictly increasing");
            }
            previousTime = key.time;
        }
    };
    validateUvCurve(emitter.flipbookUvOffsetKeys, "uvOffsetKeys");
    validateUvCurve(emitter.flipbookUvScaleKeys, "uvScaleKeys");
    float previousRateTime = -1.0f;
    for (const ParticleFlipbookRateKey& key : emitter.flipbookPlayRateKeys)
    {
        RequireFinite(key.time, "flipbook.playRateKeys.time");
        RequireFinite(key.rate, "flipbook.playRateKeys.rate");
        if (key.time < 0.0f || key.time > 1.0f ||
            key.time <= previousRateTime ||
            key.rate < 0.0f || key.rate > 16.0f)
        {
            throw std::invalid_argument(
                "Particle flipbook playRateKeys are invalid");
        }
        previousRateTime = key.time;
    }
    const uint32_t frameCount =
        emitter.flipbookColumns * emitter.flipbookRows;
    if (emitter.flipbookStartFrame >= frameCount)
    {
        throw std::invalid_argument(
            "Particle flipbook startFrame must be within the atlas");
    }
    if (emitter.flipbookEndFrame < emitter.flipbookStartFrame ||
        emitter.flipbookEndFrame >= frameCount)
    {
        throw std::invalid_argument(
            "Particle flipbook endFrame must be >= startFrame and within the atlas");
    }
    const uint32_t frameSpan =
        emitter.flipbookEndFrame - emitter.flipbookStartFrame + 1;
    if (frameSpan > kMaxParticleFlipbookFrames)
    {
        throw std::invalid_argument(
            "Particle flipbook frame span must be at most 256 frames");
    }
    if (!emitter.flipbookFrameDurations.empty())
    {
        if (emitter.flipbookFrameDurations.size() != frameSpan)
        {
            throw std::invalid_argument(
                "Particle flipbook frameDurations length must match "
                "endFrame-startFrame+1");
        }
        for (float duration : emitter.flipbookFrameDurations)
        {
            if (!std::isfinite(duration) || duration <= 0.0f)
            {
                throw std::invalid_argument(
                    "Particle flipbook frameDurations must all be finite "
                    "and positive");
            }
        }
    }
    if (!emitter.flipbookFramePivots.empty())
    {
        if (emitter.flipbookFramePivots.size() != frameSpan)
        {
            throw std::invalid_argument(
                "Particle flipbook framePivots length must match "
                "endFrame-startFrame+1");
        }
        for (const Eigen::Vector2f& pivot : emitter.flipbookFramePivots)
        {
            if (!std::isfinite(pivot.x()) || !std::isfinite(pivot.y()) ||
                pivot.x() < 0.0f || pivot.x() > 1.0f ||
                pivot.y() < 0.0f || pivot.y() > 1.0f)
            {
                throw std::invalid_argument(
                    "Particle flipbook framePivots must be finite and "
                    "within [0, 1]");
            }
        }
    }
    if (!emitter.flipbookFrameUVs.empty())
    {
        if (emitter.flipbookFrameUVs.size() != frameSpan)
        {
            throw std::invalid_argument(
                "Particle flipbook frameUVs length must match "
                "endFrame-startFrame+1");
        }
        // GPU 表是每帧一个 vec4，固定 64 项；超过时必须显式拒绝，
        // 不能把 256 帧资产的后半段静默截断成默认 rect。
        if (frameSpan > kParticleFlipbookUvVectors)
        {
            throw std::invalid_argument(
                "Particle flipbook frameUVs requires at most 64 frames");
        }
        for (const Eigen::Vector4f& rect : emitter.flipbookFrameUVs)
        {
            for (int component = 0; component < 4; ++component)
            {
                if (!std::isfinite(rect[component]) ||
                    rect[component] < 0.0f ||
                    rect[component] > 1.0f)
                {
                    throw std::invalid_argument(
                        "Particle flipbook frameUVs must be finite and "
                        "within [0, 1]");
                }
            }
            if (rect.x() >= rect.z() || rect.y() >= rect.w())
            {
                throw std::invalid_argument(
                    "Particle flipbook frameUVs require u0 < u1 "
                    "and v0 < v1");
            }
        }
    }
    if (!emitter.flipbookFrameRotations.empty())
    {
        if (emitter.flipbookFrameRotations.size() != frameSpan)
        {
            throw std::invalid_argument(
                "Particle flipbook frameRotations length must match "
                "endFrame-startFrame+1");
        }
        // GPU 表每 vec4 打包 4 帧，固定 64 帧；超过时必须拒绝，
        // 不能把后半段静默当成 0 度。
        if (frameSpan > kParticleFlipbookRotationVectors * 4)
        {
            throw std::invalid_argument(
                "Particle flipbook frameRotations requires at most "
                "64 frames");
        }
        for (float rotation : emitter.flipbookFrameRotations)
        {
            if (!std::isfinite(rotation))
            {
                throw std::invalid_argument(
                    "Particle flipbook frameRotations must all be finite");
            }
        }
    }
    if (!emitter.flipbookFramePivotKeys.empty())
    {
        if (emitter.flipbookFramePivotKeys.size() >
            kMaxParticleCurveKeys)
        {
            throw std::invalid_argument(
                "Particle flipbook.framePivotKeys allows at most 8 keys");
        }
        float previousTime = -1.0f;
        for (const ParticleFlipbookPivotKey& key :
             emitter.flipbookFramePivotKeys)
        {
            RequireFinite(key.time, "flipbook.framePivotKeys.time");
            RequireFinite(
                key.pivot.x(), "flipbook.framePivotKeys.pivot.x");
            RequireFinite(
                key.pivot.y(), "flipbook.framePivotKeys.pivot.y");
            RequireFinite(
                key.tangentIn.x(),
                "flipbook.framePivotKeys.tangentIn.x");
            RequireFinite(
                key.tangentIn.y(),
                "flipbook.framePivotKeys.tangentIn.y");
            RequireFinite(
                key.tangentOut.x(),
                "flipbook.framePivotKeys.tangentOut.x");
            RequireFinite(
                key.tangentOut.y(),
                "flipbook.framePivotKeys.tangentOut.y");
            if (key.time < 0.0f || key.time > 1.0f ||
                key.time <= previousTime)
            {
                throw std::invalid_argument(
                    "Particle flipbook.framePivotKeys times must be in "
                    "[0,1] and strictly increasing");
            }
            if (key.pivot.x() < -8.0f || key.pivot.x() > 8.0f ||
                key.pivot.y() < -8.0f || key.pivot.y() > 8.0f)
            {
                throw std::invalid_argument(
                    "Particle flipbook.framePivotKeys pivot must be in "
                    "[-8,8]");
            }
            previousTime = key.time;
        }
    }
    if (!emitter.flipbookFrameRotationKeys.empty())
    {
        if (emitter.flipbookFrameRotationKeys.size() >
            kMaxParticleCurveKeys)
        {
            throw std::invalid_argument(
                "Particle flipbook.frameRotationKeys allows at most 8 "
                "keys");
        }
        float previousTime = -1.0f;
        for (const ParticleFlipbookRotationKey& key :
             emitter.flipbookFrameRotationKeys)
        {
            RequireFinite(
                key.time, "flipbook.frameRotationKeys.time");
            RequireFinite(
                key.degrees, "flipbook.frameRotationKeys.degrees");
            RequireFinite(
                key.tangentIn, "flipbook.frameRotationKeys.tangentIn");
            RequireFinite(
                key.tangentOut, "flipbook.frameRotationKeys.tangentOut");
            if (key.time < 0.0f || key.time > 1.0f ||
                key.time <= previousTime)
            {
                throw std::invalid_argument(
                    "Particle flipbook.frameRotationKeys times must be "
                    "in [0,1] and strictly increasing");
            }
            previousTime = key.time;
        }
    }
}

} // namespace ParticleAssetDetail
} // namespace VL
