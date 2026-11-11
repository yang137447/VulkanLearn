#include "particles/particleAssetInternal.h"

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
namespace
{
void RequireFinite(float value, const char* field)
{
    if (!std::isfinite(value))
    {
        throw std::invalid_argument(
            std::string("Particle field is not finite: ") + field);
    }
}

} // namespace

void ValidateEmitter(const ParticleEmitterAsset& emitter)
{
    // ParseJson 已逐字段做有限性检查；这里再覆盖一次，保证程序化构造或
    // 未来新增写入路径也不能把 NaN/Inf 带进 GPU 缓冲。
    RequireFinite(emitter.localPosition.x(), "position.x");
    RequireFinite(emitter.localPosition.y(), "position.y");
    RequireFinite(emitter.localPosition.z(), "position.z");
    RequireFinite(emitter.fixedBoundsCenter.x(), "fixedBounds.center.x");
    RequireFinite(emitter.fixedBoundsCenter.y(), "fixedBounds.center.y");
    RequireFinite(emitter.fixedBoundsCenter.z(), "fixedBounds.center.z");
    RequireFinite(emitter.fixedBoundsExtent.x(), "fixedBounds.extent.x");
    RequireFinite(emitter.fixedBoundsExtent.y(), "fixedBounds.extent.y");
    RequireFinite(emitter.fixedBoundsExtent.z(), "fixedBounds.extent.z");
    RequireFinite(emitter.spawnRate, "spawnRate");
    RequireFinite(emitter.ribbonWidth, "ribbonWidth");
    RequireFinite(emitter.ribbonUVTiling, "ribbonUVTiling");
    RequireFinite(emitter.ribbonFade, "ribbonFade");
    RequireFinite(emitter.beam.start.x(), "beam.start.x");
    RequireFinite(emitter.beam.start.y(), "beam.start.y");
    RequireFinite(emitter.beam.start.z(), "beam.start.z");
    RequireFinite(emitter.beam.end.x(), "beam.end.x");
    RequireFinite(emitter.beam.end.y(), "beam.end.y");
    RequireFinite(emitter.beam.end.z(), "beam.end.z");
    RequireFinite(emitter.beam.width, "beam.width");
    RequireFinite(emitter.beam.noiseAmplitude, "beam.noiseAmplitude");
    RequireFinite(emitter.beam.noiseFrequency, "beam.noiseFrequency");
    RequireFinite(emitter.beam.noiseSpeed, "beam.noiseSpeed");
    RequireFinite(emitter.beam.endpointJitter, "beam.endpointJitter");
    for (const ParticleBeamColorKey& key : emitter.beam.colorKeys)
    {
        RequireFinite(key.time, "beam.colorKeys.time");
        for (int channel = 0; channel < 4; ++channel)
        {
            RequireFinite(key.color[channel], "beam.colorKeys.color");
        }
    }
    for (const ParticleBeamWidthKey& key : emitter.beam.widthKeys)
    {
        RequireFinite(key.time, "beam.widthKeys.time");
        RequireFinite(key.width, "beam.widthKeys.width");
    }
    for (const ParticleBeamArc& arc : emitter.beam.arcs)
    {
        RequireFinite(arc.colorStart.x(), "beam.arcs.colorStart.x");
        RequireFinite(arc.colorStart.y(), "beam.arcs.colorStart.y");
        RequireFinite(arc.colorStart.z(), "beam.arcs.colorStart.z");
        RequireFinite(arc.colorEnd.x(), "beam.arcs.colorEnd.x");
        RequireFinite(arc.colorEnd.y(), "beam.arcs.colorEnd.y");
        RequireFinite(arc.colorEnd.z(), "beam.arcs.colorEnd.z");
        RequireFinite(arc.widthScale, "beam.arcs.widthScale");
        RequireFinite(arc.intensity, "beam.arcs.intensity");
        RequireFinite(arc.noisePhase, "beam.arcs.noisePhase");
    }
    RequireFinite(emitter.lifetimeMin, "lifetime.min");
    RequireFinite(emitter.lifetimeMax, "lifetime.max");
    RequireFinite(emitter.velocityMin.x(), "velocity.min.x");
    RequireFinite(emitter.velocityMin.y(), "velocity.min.y");
    RequireFinite(emitter.velocityMin.z(), "velocity.min.z");
    RequireFinite(emitter.velocityMax.x(), "velocity.max.x");
    RequireFinite(emitter.velocityMax.y(), "velocity.max.y");
    RequireFinite(emitter.velocityMax.z(), "velocity.max.z");
    RequireFinite(emitter.gravity.x(), "gravity.x");
    RequireFinite(emitter.gravity.y(), "gravity.y");
    RequireFinite(emitter.gravity.z(), "gravity.z");
    RequireFinite(emitter.drag, "drag");
    RequireFinite(emitter.curlNoiseStrength, "curlNoise.strength");
    RequireFinite(emitter.curlNoiseFrequency, "curlNoise.frequency");
    RequireFinite(emitter.curlNoiseSpeed, "curlNoise.speed");
    RequireFinite(emitter.vortexStrength, "vortex.strength");
    RequireFinite(emitter.vortexRadius, "vortex.radius");
    RequireFinite(emitter.vortexAxis.x(), "vortex.axis.x");
    RequireFinite(emitter.vortexAxis.y(), "vortex.axis.y");
    RequireFinite(emitter.vortexAxis.z(), "vortex.axis.z");
    RequireFinite(emitter.attractorPosition.x(),
        "attractor.position.x");
    RequireFinite(emitter.attractorPosition.y(),
        "attractor.position.y");
    RequireFinite(emitter.attractorPosition.z(),
        "attractor.position.z");
    RequireFinite(emitter.attractorStrength, "attractor.strength");
    RequireFinite(emitter.attractorRadius, "attractor.radius");
    RequireFinite(emitter.wind.x(), "wind.x");
    RequireFinite(emitter.wind.y(), "wind.y");
    RequireFinite(emitter.wind.z(), "wind.z");
    RequireFinite(emitter.shapeRadius, "shape.radius");
    RequireFinite(emitter.shapeExtent.x(), "shape.extent.x");
    RequireFinite(emitter.shapeExtent.y(), "shape.extent.y");
    RequireFinite(emitter.shapeExtent.z(), "shape.extent.z");
    RequireFinite(emitter.shapeHeight, "shape.height");
    RequireFinite(emitter.shapeInnerRadius, "shape.innerRadius");
    RequireFinite(emitter.collisionPlaneNormal.x(),
        "collision.plane.normal.x");
    RequireFinite(emitter.collisionPlaneNormal.y(),
        "collision.plane.normal.y");
    RequireFinite(emitter.collisionPlaneNormal.z(),
        "collision.plane.normal.z");
    RequireFinite(emitter.collisionPlaneOffset,
        "collision.plane.offset");
    RequireFinite(emitter.collisionBoxMin.x(),
        "collision.box.min.x");
    RequireFinite(emitter.collisionBoxMin.y(),
        "collision.box.min.y");
    RequireFinite(emitter.collisionBoxMin.z(),
        "collision.box.min.z");
    RequireFinite(emitter.collisionBoxMax.x(),
        "collision.box.max.x");
    RequireFinite(emitter.collisionBoxMax.y(),
        "collision.box.max.y");
    RequireFinite(emitter.collisionBoxMax.z(),
        "collision.box.max.z");
    RequireFinite(emitter.collisionCapsuleA.x(),
        "collision.capsule.a.x");
    RequireFinite(emitter.collisionCapsuleA.y(),
        "collision.capsule.a.y");
    RequireFinite(emitter.collisionCapsuleA.z(),
        "collision.capsule.a.z");
    RequireFinite(emitter.collisionCapsuleB.x(),
        "collision.capsule.b.x");
    RequireFinite(emitter.collisionCapsuleB.y(),
        "collision.capsule.b.y");
    RequireFinite(emitter.collisionCapsuleB.z(),
        "collision.capsule.b.z");
    RequireFinite(emitter.collisionCapsuleRadius,
        "collision.capsule.radius");
    RequireFinite(emitter.collisionBounce, "collision.bounce");
    RequireFinite(emitter.collisionFriction, "collision.friction");
    RequireFinite(emitter.collisionRadius, "collision.radius");
    RequireFinite(emitter.sizeStart, "size.start");
    RequireFinite(emitter.sizeEnd, "size.end");
    RequireFinite(emitter.rotation, "rotation");
    RequireFinite(emitter.rotationSpeed, "rotationSpeed");
    RequireFinite(emitter.softness, "softness");
    RequireFinite(emitter.softParticleDistance, "softParticleDistance");
    RequireFinite(emitter.softParticleTapRadius, "softParticleTapRadius");
    RequireFinite(emitter.softParticleThickness, "softParticleThickness");
    RequireFinite(emitter.opacity, "opacity");
    RequireFinite(emitter.distortionStrength, "distortionStrength");
    RequireFinite(emitter.maxDrawDistance, "maxDrawDistance");
    RequireFinite(emitter.fadeDistance, "fadeDistance");
    RequireFinite(emitter.minScreenSize, "minScreenSize");
    RequireFinite(emitter.maxScreenSize, "maxScreenSize");
    RequireFinite(emitter.burstInterval, "burstInterval");
    RequireFinite(emitter.spawnPerUnit, "spawnPerUnit");
    RequireFinite(emitter.lodDistance, "lodDistance");
    RequireFinite(emitter.lodSpawnRateScale, "lodSpawnRateScale");
    RequireFinite(emitter.flipbookFramesPerSecond, "flipbook.fps");
    RequireFinite(emitter.flipbookPlayRate, "flipbook.playRate");
    RequireFinite(emitter.eooEmissiveStrength, "eooEmissiveStrength");
    for (int channel = 0; channel < 4; ++channel)
    {
        RequireFinite(emitter.colorStart[channel], "colorStart");
        RequireFinite(emitter.colorEnd[channel], "colorEnd");
    }

    if (emitter.maxParticles == 0 || emitter.maxParticles > 65536)
    {
        throw std::invalid_argument(
            "Particle maxParticles must be in [1, 65536]");
    }
    if (emitter.renderMode != "billboard" &&
        emitter.renderMode != "ribbon" &&
        emitter.renderMode != "mesh" &&
        emitter.renderMode != "skeletalMeshTris" &&
        emitter.renderMode != "skeletalMeshBones" &&
        emitter.renderMode != "beam" &&
        emitter.renderMode != "distortion" &&
        emitter.renderMode != "light" &&
        emitter.renderMode != "decal")
    {
        throw std::invalid_argument(
            "Particle renderMode must be 'billboard', 'ribbon', 'mesh', "
            "'skeletalMeshTris', 'skeletalMeshBones', "
            "'beam', 'distortion', 'light' or 'decal'");
    }
    if (emitter.hasLight)
    {
        RequireFinite(emitter.light.intensity, "light.intensity");
        RequireFinite(emitter.light.radius, "light.radius");
        if (emitter.light.intensity <= 0.0f)
        {
            throw std::invalid_argument(
                "Particle light.intensity must be positive");
        }
        if (emitter.light.radius <= 0.0f)
        {
            throw std::invalid_argument(
                "Particle light.radius must be positive");
        }
        RequireFinite(emitter.light.shadowBias, "light.shadowBias");
        float previousBrightnessTime = -1.0f;
        for (const ParticleLightBrightnessKey& key :
             emitter.light.brightnessKeys)
        {
            RequireFinite(key.time, "light.brightnessKeys.time");
            RequireFinite(key.brightness, "light.brightnessKeys.brightness");
            for (int channel = 0; channel < 3; ++channel)
            {
                RequireFinite(key.color[channel],
                    "light.brightnessKeys.color");
                if (key.color[channel] < 0.0f ||
                    key.color[channel] > 64.0f)
                {
                    throw std::invalid_argument(
                        "Particle light.brightnessKeys color must be in "
                        "[0,64]");
                }
            }
            if (key.time < 0.0f || key.time > 1.0f ||
                key.time <= previousBrightnessTime)
            {
                throw std::invalid_argument(
                    "Particle light.brightnessKeys times must be in [0,1] "
                    "and strictly increasing");
            }
            if (key.brightness < 0.0f || key.brightness > 64.0f)
            {
                throw std::invalid_argument(
                    "Particle light.brightnessKeys brightness must be in "
                    "[0,64]");
            }
            previousBrightnessTime = key.time;
        }
        if (emitter.light.iesProfile.size() > 64 ||
            emitter.light.iesTexture.size() > 64)
        {
            throw std::invalid_argument(
                "Particle light IES profile allows at most 64 samples");
        }
        if (emitter.light.shadowBias < 0.0f ||
            emitter.light.shadowBias > 100.0f)
        {
            throw std::invalid_argument(
                "Particle light.shadowBias must be in [0,100]");
        }
    }
    if (emitter.renderMode == "light")
    {
        if (!emitter.hasLight)
        {
            throw std::invalid_argument(
                "Particle light renderMode requires a light object");
        }
        if (emitter.maxParticles > 64)
        {
            throw std::invalid_argument(
                "Particle light maxParticles must be <= 64");
        }
    }
    if (emitter.hasDecal)
    {
        RequireFinite(emitter.decal.size.x(), "decal.size.x");
        RequireFinite(emitter.decal.size.y(), "decal.size.y");
        RequireFinite(emitter.decal.size.z(), "decal.size.z");
        for (int axis = 0; axis < 3; ++axis)
        {
            if (emitter.decal.size[axis] <= 0.0f ||
                emitter.decal.size[axis] > 100000.0f)
            {
                throw std::invalid_argument(
                    "Particle decal.size components must be in "
                    "(0, 100000]");
            }
        }
        RequireFinite(emitter.decal.rotation.x(), "decal.rotation.x");
        RequireFinite(emitter.decal.rotation.y(), "decal.rotation.y");
        RequireFinite(emitter.decal.rotation.z(), "decal.rotation.z");
        for (int channel = 0; channel < 3; ++channel)
        {
            RequireFinite(
                emitter.decal.color[channel], "decal.color");
            if (emitter.decal.color[channel] < 0.0f ||
                emitter.decal.color[channel] > 64.0f)
            {
                throw std::invalid_argument(
                    "Particle decal.color components must be in [0, 64]");
            }
        }
        const struct
        {
            float value;
            const char* name;
        } unitRangeFields[] = {
            {emitter.decal.opacity, "decal.opacity"},
            {emitter.decal.edgeSoftness, "decal.edgeSoftness"},
            {emitter.decal.noiseStrength, "decal.noiseStrength"},
            {emitter.decal.roughness, "decal.roughness"},
            {emitter.decal.metallic, "decal.metallic"},
            {emitter.decal.ambientOcclusion, "decal.ambientOcclusion"},
            {emitter.decal.normalStrength, "decal.normalStrength"}};
        for (const auto& field : unitRangeFields)
        {
            RequireFinite(field.value, field.name);
            if (field.value < 0.0f || field.value > 1.0f)
            {
                throw std::invalid_argument(
                    std::string("Particle ") + field.name +
                    " must be in [0, 1]");
            }
        }
        for (int channel = 0; channel < 3; ++channel)
        {
            RequireFinite(
                emitter.decal.emissive[channel], "decal.emissive");
            if (emitter.decal.emissive[channel] < 0.0f ||
                emitter.decal.emissive[channel] > 64.0f)
            {
                throw std::invalid_argument(
                    "Particle decal.emissive components must be in [0, 64]");
            }
        }
        RequireFinite(
            emitter.decal.emissiveStrength, "decal.emissiveStrength");
        if (emitter.decal.emissiveStrength < 0.0f ||
            emitter.decal.emissiveStrength > 1000.0f)
        {
            throw std::invalid_argument(
                "Particle decal.emissiveStrength must be in [0, 1000]");
        }
        if (emitter.decal.sortOrder > 255)
        {
            throw std::invalid_argument(
                "Particle decal.sortOrder must be in [0, 255]");
        }
    }
    if (emitter.renderMode == "decal" && !emitter.hasDecal)
    {
        throw std::invalid_argument(
            "Particle decal renderMode requires a decal object");
    }
    if (emitter.billboardAlignment != "camera" &&
        emitter.billboardAlignment != "velocity" &&
        emitter.billboardAlignment != "fixed")
    {
        throw std::invalid_argument(
            "Particle billboardAlignment must be 'camera', 'velocity' "
            "or 'fixed'");
    }
    if (emitter.billboardAlignment == "fixed")
    {
        RequireFinite(emitter.billboardNormal.x(), "billboardNormal.x");
        RequireFinite(emitter.billboardNormal.y(), "billboardNormal.y");
        RequireFinite(emitter.billboardNormal.z(), "billboardNormal.z");
        RequireFinite(emitter.billboardUp.x(), "billboardUp.x");
        RequireFinite(emitter.billboardUp.y(), "billboardUp.y");
        RequireFinite(emitter.billboardUp.z(), "billboardUp.z");
        const float normalLengthSquared =
            emitter.billboardNormal.squaredNorm();
        const float upLengthSquared =
            emitter.billboardUp.squaredNorm();
        if (!(normalLengthSquared > 1.0e-6f) ||
            !(upLengthSquared > 1.0e-6f))
        {
            throw std::invalid_argument(
                "Particle fixed billboardNormal/billboardUp must be "
                "non-zero");
        }
        const float parallel = std::abs(
            emitter.billboardNormal.normalized().dot(
                emitter.billboardUp.normalized()));
        if (!(parallel < 0.999f))
        {
            throw std::invalid_argument(
                "Particle fixed billboardNormal and billboardUp must not "
                "be parallel");
        }
    }
    if (emitter.ribbonWidth <= 0.0f)
    {
        throw std::invalid_argument(
            "Particle ribbonWidth must be positive");
    }
    if (emitter.ribbonSegments < 1 ||
        emitter.ribbonSegments > kMaxParticleRibbonSegments)
    {
        throw std::invalid_argument(
            "Particle ribbonSegments must be in [1, 32]");
    }
    if (emitter.ribbonFade < 0.0f || emitter.ribbonFade > 1.0f)
    {
        throw std::invalid_argument(
            "Particle ribbonFade must be in [0, 1]");
    }
    if (emitter.ribbonUVTiling <= 0.0f ||
        emitter.ribbonUVTiling > 64.0f)
    {
        throw std::invalid_argument(
            "Particle ribbonUVTiling must be in (0, 64]");
    }
    if (emitter.ribbonPivot < -1.0f ||
        emitter.ribbonPivot > 1.0f)
    {
        throw std::invalid_argument(
            "Particle ribbonPivot must be in [-1, 1]");
    }
    if (!std::isfinite(emitter.ribbonTwist) ||
        emitter.ribbonTwist < -3600.0f ||
        emitter.ribbonTwist > 3600.0f)
    {
        throw std::invalid_argument(
            "Particle ribbonTwist must be finite and in [-3600, 3600]");
    }
    float previousRibbonWidthTime = -1.0f;
    for (const ParticleRibbonWidthKey& key : emitter.ribbonWidthKeys)
    {
        RequireFinite(key.time, "ribbonWidthKeys.time");
        RequireFinite(key.width, "ribbonWidthKeys.width");
        RequireFinite(key.tangentIn, "ribbonWidthKeys.tangentIn");
        RequireFinite(key.tangentOut, "ribbonWidthKeys.tangentOut");
        if (key.time < 0.0f || key.time > 1.0f ||
            key.time <= previousRibbonWidthTime)
        {
            throw std::invalid_argument(
                "Particle ribbonWidthKeys times must be in [0,1] "
                "and strictly increasing");
        }
        if (key.width < 0.0f || key.width > 8.0f)
        {
            throw std::invalid_argument(
                "Particle ribbonWidthKeys width must be in [0,8]");
        }
        previousRibbonWidthTime = key.time;
    }
    float previousRibbonColorTime = -1.0f;
    for (const ParticleRibbonColorKey& key : emitter.ribbonColorKeys)
    {
        if (key.time < 0.0f || key.time > 1.0f ||
            key.time <= previousRibbonColorTime)
        {
            throw std::invalid_argument(
                "Particle ribbonColorKeys times must be in [0,1] "
                "and strictly increasing");
        }
        for (int channel = 0; channel < 4; ++channel)
        {
            RequireFinite(key.color[channel], "ribbonColorKeys.color");
            RequireFinite(
                key.tangentIn[channel], "ribbonColorKeys.tangentIn");
            RequireFinite(
                key.tangentOut[channel], "ribbonColorKeys.tangentOut");
            if (key.color[channel] < 0.0f || key.color[channel] > 64.0f)
            {
                throw std::invalid_argument(
                    "Particle ribbonColorKeys color must be in [0,64]");
            }
        }
        previousRibbonColorTime = key.time;
    }
    if (emitter.beam.segments < kMinParticleBeamSegments ||
        emitter.beam.segments > kMaxParticleBeamSegments)
    {
        throw std::invalid_argument(
            "Particle beam.segments must be in [2, 64]");
    }
    if (emitter.beam.count < kMinParticleBeamCount ||
        emitter.beam.count > kMaxParticleBeamCount)
    {
        throw std::invalid_argument(
            "Particle beam.count must be in [1, 16]");
    }
    if (emitter.beam.width <= 0.0f)
    {
        throw std::invalid_argument(
            "Particle beam.width must be positive");
    }
    if (emitter.beam.noiseAmplitude < 0.0f)
    {
        throw std::invalid_argument(
            "Particle beam.noiseAmplitude must be non-negative");
    }
    if (emitter.beam.noiseFrequency <= 0.0f)
    {
        throw std::invalid_argument(
            "Particle beam.noiseFrequency must be positive");
    }
    if (emitter.beam.noiseSpeed < 0.0f)
    {
        throw std::invalid_argument(
            "Particle beam.noiseSpeed must be non-negative");
    }
    if (emitter.beam.endpointJitter < 0.0f)
    {
        throw std::invalid_argument(
            "Particle beam.endpointJitter must be non-negative");
    }
    if (emitter.beam.branchCount > kMaxParticleBeamBranches)
    {
        throw std::invalid_argument(
            "Particle beam.branchCount must be in [0, 4]");
    }
    if (emitter.beam.branchStart < 0.0f ||
        emitter.beam.branchStart > 1.0f)
    {
        throw std::invalid_argument(
            "Particle beam.branchStart must be in [0, 1]");
    }
    if (emitter.beam.branchLength <= 0.0f ||
        emitter.beam.branchLength > 1.0f)
    {
        throw std::invalid_argument(
            "Particle beam.branchLength must be in (0, 1]");
    }
    if (emitter.beam.branchSpread < 0.0f)
    {
        throw std::invalid_argument(
            "Particle beam.branchSpread must be non-negative");
    }
    if (emitter.beam.colorKeys.size() > kMaxParticleCurveKeys ||
        emitter.beam.widthKeys.size() > kMaxParticleCurveKeys)
    {
        throw std::invalid_argument(
            "Particle beam curve key count exceeds GPU capacity");
    }
    float previousBeamColorTime = -1.0f;
    for (const ParticleBeamColorKey& key : emitter.beam.colorKeys)
    {
        RequireFinite(key.time, "beam.colorKeys.time");
        for (int channel = 0; channel < 4; ++channel)
        {
            RequireFinite(
                key.tangentIn[channel], "beam.colorKeys.tangentIn");
            RequireFinite(
                key.tangentOut[channel], "beam.colorKeys.tangentOut");
        }
        if (key.time < 0.0f || key.time > 1.0f ||
            key.time <= previousBeamColorTime)
        {
            throw std::invalid_argument(
                "Particle beam color key times must be strictly "
                "increasing in [0,1]");
        }
        for (int channel = 0; channel < 4; ++channel)
        {
            if (key.color[channel] < 0.0f ||
                key.color[channel] > 64.0f)
            {
                throw std::invalid_argument(
                    "Particle beam color key channels must be in [0,64]");
            }
        }
        previousBeamColorTime = key.time;
    }
    float previousBeamWidthTime = -1.0f;
    for (const ParticleBeamWidthKey& key : emitter.beam.widthKeys)
    {
        RequireFinite(key.time, "beam.widthKeys.time");
        RequireFinite(key.width, "beam.widthKeys.width");
        RequireFinite(key.tangentIn, "beam.widthKeys.tangentIn");
        RequireFinite(key.tangentOut, "beam.widthKeys.tangentOut");
        if (key.time < 0.0f || key.time > 1.0f ||
            key.time <= previousBeamWidthTime)
        {
            throw std::invalid_argument(
                "Particle beam width key times must be strictly "
                "increasing in [0,1]");
        }
        if (key.width < 0.0f || key.width > 1.0f)
        {
            throw std::invalid_argument(
                "Particle beam width key value must be in [0,1]");
        }
        previousBeamWidthTime = key.time;
    }
    if (emitter.beam.arcs.size() > kMaxParticleBeamCount)
    {
        throw std::invalid_argument(
            "Particle beam.arcs exceeds the 16-arc capacity");
    }
    if (!emitter.beam.arcs.empty() &&
        emitter.beam.arcs.size() != emitter.beam.count)
    {
        throw std::invalid_argument(
            "Particle beam.arcs size must equal beam.count");
    }
    for (const ParticleBeamArc& arc : emitter.beam.arcs)
    {
        if (arc.colorStart.minCoeff() < 0.0f ||
            arc.colorStart.maxCoeff() > 64.0f ||
            arc.colorEnd.minCoeff() < 0.0f ||
            arc.colorEnd.maxCoeff() > 64.0f)
        {
            throw std::invalid_argument(
                "Particle beam arc color channels must be in [0,64]");
        }
        if (arc.widthScale < 0.0f || arc.widthScale > 1.0f)
        {
            throw std::invalid_argument(
                "Particle beam arc widthScale must be in [0,1]");
        }
        if (arc.intensity < 0.0f || arc.intensity > 64.0f)
        {
            throw std::invalid_argument(
                "Particle beam arc intensity must be in [0,64]");
        }
        if (arc.noisePhase < -1000.0f || arc.noisePhase > 1000.0f)
        {
            throw std::invalid_argument(
                "Particle beam arc noisePhase must be in [-1000,1000]");
        }
    }
    if (emitter.beam.hitScan.enabled)
    {
        RequireFinite(
            emitter.beam.hitScan.planeNormal.x(),
            "beam.hitScan.planeNormal.x");
        RequireFinite(
            emitter.beam.hitScan.planeNormal.y(),
            "beam.hitScan.planeNormal.y");
        RequireFinite(
            emitter.beam.hitScan.planeNormal.z(),
            "beam.hitScan.planeNormal.z");
        if (emitter.beam.hitScan.planeNormal.squaredNorm() <= 1.0e-12f)
        {
            throw std::invalid_argument(
                "Particle beam.hitScan.planeNormal must be non-zero");
        }
        RequireFinite(
            emitter.beam.hitScan.planeOffset,
            "beam.hitScan.planeOffset");
        RequireFinite(
            emitter.beam.hitScan.maxDistance,
            "beam.hitScan.maxDistance");
        if (emitter.beam.hitScan.maxDistance <= 0.0f ||
            emitter.beam.hitScan.maxDistance > 1000000.0f)
        {
            throw std::invalid_argument(
                "Particle beam.hitScan.maxDistance must be in (0, 1e6]");
        }
    }
    if ((emitter.beam.end - emitter.beam.start).squaredNorm() <=
        1.0e-12f)
    {
        throw std::invalid_argument(
            "Particle beam start and end must differ");
    }
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
    if (emitter.collisionType != "none" &&
        emitter.collisionType != "plane" &&
        emitter.collisionType != "box" &&
        emitter.collisionType != "capsule" &&
        emitter.collisionType != "sdf" &&
        emitter.collisionType != "mesh")
    {
        throw std::invalid_argument(
            "Particle collision type must be 'none', 'plane', 'box', "
            "'capsule', 'sdf', or 'mesh'");
    }
    // localSpace 与碰撞现在允许组合：localSpace=true 时碰撞体（平面/
    // 盒体/胶囊/SDF/三角网格）按发射器局部空间解释，与局部粒子坐标
    // 同一空间。
    if (emitter.collisionType == "sdf" &&
        (!emitter.collisionSdf || emitter.collisionSdf->Empty()))
    {
        throw std::invalid_argument(
            "Particle collision type 'sdf' requires loaded SDF data");
    }
    if (emitter.collisionType == "mesh")
    {
        if (emitter.collisionMeshPath.empty())
        {
            throw std::invalid_argument(
                "Particle collision type 'mesh' requires "
                "collision.mesh.path");
        }
        RequireValidParticleMeshPath(
            emitter.collisionMeshPath, "collision.mesh.path");
        if (!emitter.collisionSdf || emitter.collisionSdf->Empty())
        {
            throw std::invalid_argument(
                "Particle collision type 'mesh' requires loaded SDF data");
        }
        if (emitter.collisionSdf->sourcePadding >= 0.0f &&
            emitter.collisionRadius >
                emitter.collisionSdf->sourcePadding + 1.0e-5f)
        {
            throw std::invalid_argument(
                "Particle collision mesh padding must be >= "
                "collision.radius");
        }
    }
    else if (!emitter.collisionMeshPath.empty())
    {
        // 非激活类型的路径仍要满足资源根相对契约，避免切换类型时
        // 才暴露拼写错误。
        RequireValidParticleMeshPath(
            emitter.collisionMeshPath, "collision.mesh.path");
    }
    if (!emitter.materialInstancePath.empty() &&
        !IsValidParticleMeshRelativePath(emitter.materialInstancePath))
    {
        throw std::invalid_argument(
            "Particle materialInstancePath must be a "
            "resource-root-relative .json path: " +
            emitter.materialInstancePath);
    }
    if (emitter.fixedTimeStep < 0.0f ||
        emitter.fixedTimeStep > 0.25f)
    {
        throw std::invalid_argument(
            "Particle fixedTimeStep must be in [0, 0.25] seconds");
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (emitter.fixedBoundsExtent[axis] < 0.0f)
        {
            throw std::invalid_argument(
                "Particle fixedBounds extent must be non-negative");
        }
    }
    if (emitter.useFixedBounds &&
        emitter.fixedBoundsExtent.squaredNorm() <= 0.0f)
    {
        throw std::invalid_argument(
            "Particle fixedBounds extent must not be all zero");
    }
    if (!emitter.collisionPlaneNormal.allFinite() ||
        emitter.collisionPlaneNormal.norm() <= 1.0e-6f)
    {
        throw std::invalid_argument(
            "Particle collision plane normal must be finite and non-zero");
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (emitter.collisionBoxMin[axis] > emitter.collisionBoxMax[axis])
        {
            throw std::invalid_argument(
                "Particle collision box min must not exceed max");
        }
    }
    if (emitter.collisionBounce < 0.0f ||
        emitter.collisionBounce > 1.0f ||
        emitter.collisionFriction < 0.0f ||
        emitter.collisionFriction > 1.0f)
    {
        throw std::invalid_argument(
            "Particle collision bounce/friction must be in [0, 1]");
    }
    if (emitter.collisionRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle collision radius must be non-negative");
    }
    if (emitter.collisionCapsuleRadius < 0.0f)
    {
        throw std::invalid_argument(
            "Particle collision capsule radius must be non-negative");
    }
    if (emitter.sizeStart < 0.0f || emitter.sizeEnd < 0.0f)
    {
        throw std::invalid_argument(
            "Particle size must be non-negative");
    }
    if (emitter.rotationSpeed < -3600.0f ||
        emitter.rotationSpeed > 3600.0f)
    {
        throw std::invalid_argument(
            "Particle rotationSpeed must be in [-3600, 3600]");
    }
    if (emitter.softness < 0.0f || emitter.softness > 1.0f)
    {
        throw std::invalid_argument(
            "Particle softness must be in [0, 1]");
    }
    if (emitter.softParticleDistance < 0.0f ||
        emitter.softParticleDistance > 10000.0f)
    {
        throw std::invalid_argument(
            "Particle softParticleDistance must be in [0, 10000]");
    }
    if (emitter.softParticleTapRadius < 0.0f ||
        emitter.softParticleTapRadius > 32.0f)
    {
        throw std::invalid_argument(
            "Particle softParticleTapRadius must be in [0, 32]");
    }
    if (emitter.softParticleThickness < 0.0f ||
        emitter.softParticleThickness > 10000.0f)
    {
        throw std::invalid_argument(
            "Particle softParticleThickness must be in [0, 10000]");
    }
    if (emitter.billboardDensity < 0.0f ||
        emitter.billboardDensity > 0.25f)
    {
        throw std::invalid_argument(
            "Particle billboardDensity must be in [0, 0.25]");
    }
    // 当前 bitonic sort 每个 pass 一次 dispatch；4096 已需要 78 个 pass。
    // 排序池必须覆盖整个 particle pool，因此不能在排序开启时静默截断。
    if (emitter.sortParticles &&
        emitter.renderMode != "decal" &&
        emitter.maxParticles > 4096)
    {
        throw std::invalid_argument(
            "Particle sortParticles requires maxParticles <= 4096");
    }
    if (emitter.sortOrder < -1000 || emitter.sortOrder > 1000)
    {
        throw std::invalid_argument(
            "Particle sortOrder must be in [-1000, 1000]");
    }
    // alpha billboard 的绘制顺序直接影响结果；作者忘了开启排序时直接拒绝，
    // 而不是静默按池索引顺序画出穿插。additive 与顺序无关，保持自由。
    // beam 的实例顺序就是作者弧编号，不存在逐粒子深度排序；alpha beam
    // 不应被要求打开一个渲染端永远不会使用的排序开关。distortion 的累积
    // 统一走 One/SrcAlpha 加法，与 emitter.additive 无关，也不需要排序。
    if (!emitter.additive &&
        !emitter.sortParticles &&
        emitter.renderMode != "beam" &&
        emitter.renderMode != "distortion" &&
        emitter.renderMode != "light" &&
        emitter.renderMode != "decal")
    {
        throw std::invalid_argument(
            "Particle alpha blend requires sortParticles=true");
    }
    if (emitter.eooEmissiveStrength < 0.0f ||
        emitter.eooEmissiveStrength > 16.0f)
    {
        throw std::invalid_argument(
            "Particle eooEmissiveStrength must be in [0, 16]");
    }
    if (emitter.opacity < 0.0f || emitter.opacity > 1.0f)
    {
        throw std::invalid_argument(
            "Particle opacity must be in [0, 1]");
    }
    if (emitter.distortionStrength < 0.0f ||
        emitter.distortionStrength > 0.25f)
    {
        throw std::invalid_argument(
            "Particle distortionStrength must be in [0, 0.25]");
    }
    if (emitter.maxDrawDistance < 0.0f ||
        emitter.fadeDistance < 0.0f ||
        emitter.minScreenSize < 0.0f ||
        emitter.maxScreenSize < 0.0f)
    {
        throw std::invalid_argument(
            "Particle visibility fields must be non-negative");
    }
    if (emitter.maxDrawDistance == 0.0f)
    {
        if (emitter.fadeDistance != 0.0f)
        {
            throw std::invalid_argument(
                "Particle fadeDistance requires maxDrawDistance > 0");
        }
    }
    else if (emitter.fadeDistance > emitter.maxDrawDistance)
    {
        throw std::invalid_argument(
            "Particle fadeDistance must not exceed maxDrawDistance");
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (emitter.velocityMin[axis] > emitter.velocityMax[axis])
        {
            throw std::invalid_argument(
                "Particle velocity min must not exceed max");
        }
    }
    if (emitter.colorKeys.size() > kMaxParticleCurveKeys ||
        emitter.sizeKeys.size() > kMaxParticleCurveKeys ||
        emitter.lifetimeScaleKeys.size() > kMaxParticleCurveKeys)
    {
        throw std::invalid_argument(
            "Particle curve key count exceeds GPU capacity");
    }
    const int interpolationCode =
        ParticleCurveInterpolationCode(emitter.curveInterpolation);
    if (interpolationCode < 0 || interpolationCode > 2)
    {
        throw std::invalid_argument(
            "Particle curveInterpolation is not supported");
    }
    float previousColorTime = -1.0f;
    for (const ParticleColorKey& key : emitter.colorKeys)
    {
        RequireFinite(key.time, "colorKeys.time");
        for (int channel = 0; channel < 4; ++channel)
        {
            RequireFinite(key.color[channel], "colorKeys.color");
            RequireFinite(
                key.tangentIn[channel], "colorKeys.tangentIn");
            RequireFinite(
                key.tangentOut[channel], "colorKeys.tangentOut");
        }
        if (key.time < 0.0f || key.time > 1.0f ||
            key.time <= previousColorTime)
        {
            throw std::invalid_argument(
                "Particle color key times must be strictly increasing in [0,1]");
        }
        previousColorTime = key.time;
    }
    float previousSizeTime = -1.0f;
    for (const ParticleSizeKey& key : emitter.sizeKeys)
    {
        RequireFinite(key.time, "sizeKeys.time");
        RequireFinite(key.size, "sizeKeys.size");
        RequireFinite(key.tangentIn, "sizeKeys.tangentIn");
        RequireFinite(key.tangentOut, "sizeKeys.tangentOut");
        if (key.time < 0.0f || key.time > 1.0f ||
            key.time <= previousSizeTime)
        {
            throw std::invalid_argument(
                "Particle size key times must be strictly increasing in [0,1]");
        }
        if (key.size < 0.0f)
        {
            throw std::invalid_argument(
                "Particle size key value must be non-negative");
        }
        previousSizeTime = key.time;
    }
    float previousLifetimeScaleTime = -1.0f;
    for (const ParticleLifetimeScaleKey& key :
         emitter.lifetimeScaleKeys)
    {
        RequireFinite(key.time, "lifetimeScaleKeys.time");
        RequireFinite(key.scale, "lifetimeScaleKeys.scale");
        if (key.time < 0.0f || key.time > 1.0f ||
            key.time < previousLifetimeScaleTime)
        {
            throw std::invalid_argument(
                "Particle lifetime scale key times must be in [0,1] "
                "and non-decreasing");
        }
        if (key.scale < 0.0f || key.scale > 4.0f)
        {
            throw std::invalid_argument(
                "Particle lifetime scale key value must be in [0,4]");
        }
        previousLifetimeScaleTime = key.time;
    }
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
