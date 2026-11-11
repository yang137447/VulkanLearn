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

void RequireFinite(float value, const char* field)
{
    if (!std::isfinite(value))
    {
        throw std::invalid_argument(
            std::string("Particle field is not finite: ") + field);
    }
}

void ValidateEmitterFiniteFields(const ParticleEmitterAsset& emitter)
{
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
}

void ValidateEmitterRenderModeAndLight(const ParticleEmitterAsset& emitter)
{
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
}

void ValidateEmitterDecal(const ParticleEmitterAsset& emitter)
{
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
}

void ValidateEmitterBillboardAlignment(const ParticleEmitterAsset& emitter)
{
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
}

void ValidateEmitterRibbon(const ParticleEmitterAsset& emitter)
{
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
}

void ValidateEmitterBeam(const ParticleEmitterAsset& emitter)
{
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
}

void ValidateEmitterCollision(const ParticleEmitterAsset& emitter)
{
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
}

void ValidateEmitterDisplayAndCurves(const ParticleEmitterAsset& emitter)
{
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
}

void ValidateEmitter(const ParticleEmitterAsset& emitter)
{
    // ParseJson 已逐字段做有限性检查；这里再覆盖一次，保证程序化构造或
    // 未来新增写入路径也不能把 NaN/Inf 带进 GPU 缓冲。
    ValidateEmitterFiniteFields(emitter);
    ValidateEmitterRenderModeAndLight(emitter);
    ValidateEmitterDecal(emitter);
    ValidateEmitterBillboardAlignment(emitter);
    ValidateEmitterRibbon(emitter);
    ValidateEmitterBeam(emitter);
    ValidateEmitterSpawnAndMesh(emitter);
    ValidateEmitterSimulation(emitter);
    ValidateEmitterCollision(emitter);
    ValidateEmitterDisplayAndCurves(emitter);
    ValidateEmitterFlipbook(emitter);
}

} // namespace ParticleAssetDetail
} // namespace VL
