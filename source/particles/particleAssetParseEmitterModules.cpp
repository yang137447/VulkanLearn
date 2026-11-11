#include "particles/particleAssetInternal.h"
#include "particles/particleAssetEmitterInternal.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "particles/particleBeam.h"
#include "particles/particleFlipbook.h"


namespace VL
{
namespace ParticleAssetDetail
{

void ParseEmitterBeam(const nlohmann::json& json, ParticleEmitterAsset& emitter)
{
    if (json.contains("beam"))
    {
        const nlohmann::json& beam = json.at("beam");
        if (!beam.is_object())
        {
            throw std::invalid_argument(
                "Particle beam must be an object");
        }
        emitter.beam.start = ReadVec3(
            beam, "start", emitter.beam.start);
        emitter.beam.end = ReadVec3(
            beam, "end", emitter.beam.end);
        emitter.beam.segments = ReadCountField(
            beam, "segments", emitter.beam.segments);
        emitter.beam.width = ReadFiniteFloat(
            beam, "width", emitter.beam.width);
        emitter.beam.count = ReadCountField(
            beam, "count", emitter.beam.count);
        emitter.beam.noiseAmplitude = ReadFiniteFloat(
            beam, "noiseAmplitude", emitter.beam.noiseAmplitude);
        emitter.beam.noiseFrequency = ReadFiniteFloat(
            beam, "noiseFrequency", emitter.beam.noiseFrequency);
        emitter.beam.noiseSpeed = ReadFiniteFloat(
            beam, "noiseSpeed", emitter.beam.noiseSpeed);
        emitter.beam.endpointJitter = ReadFiniteFloat(
            beam, "endpointJitter", emitter.beam.endpointJitter);
        emitter.beam.branchCount = ReadCountField(
            beam, "branchCount", emitter.beam.branchCount);
        emitter.beam.branchStart = ReadFiniteFloat(
            beam, "branchStart", emitter.beam.branchStart);
        emitter.beam.branchLength = ReadFiniteFloat(
            beam, "branchLength", emitter.beam.branchLength);
        emitter.beam.branchSpread = ReadFiniteFloat(
            beam, "branchSpread", emitter.beam.branchSpread);
        if (beam.contains("colorKeys"))
        {
            const nlohmann::json& keys = beam.at("colorKeys");
            if (!keys.is_array() || keys.empty() ||
                keys.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    "Particle beam.colorKeys requires 1..8 entries");
            }
            for (const nlohmann::json& keyJson : keys)
            {
                if (!keyJson.is_object())
                {
                    throw std::invalid_argument(
                        "Particle beam color key must be an object");
                }
                ParticleBeamColorKey key;
                key.time = ReadFiniteFloat(
                    keyJson, "time", key.time);
                key.color = ReadVec4(
                    keyJson, "color", key.color);
                key.tangentIn = ReadVec4Tangent(
                    keyJson, "tangentIn");
                key.tangentOut = ReadVec4Tangent(
                    keyJson, "tangentOut");
                emitter.beam.colorKeys.push_back(key);
            }
        }
        if (beam.contains("widthKeys"))
        {
            const nlohmann::json& keys = beam.at("widthKeys");
            if (!keys.is_array() || keys.empty() ||
                keys.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    "Particle beam.widthKeys requires 1..8 entries");
            }
            for (const nlohmann::json& keyJson : keys)
            {
                if (!keyJson.is_object())
                {
                    throw std::invalid_argument(
                        "Particle beam width key must be an object");
                }
                ParticleBeamWidthKey key;
                key.time = ReadFiniteFloat(
                    keyJson, "time", key.time);
                key.width = ReadFiniteFloat(
                    keyJson, "width", key.width);
                key.tangentIn = ReadFiniteFloat(
                    keyJson, "tangentIn", 0.0f);
                key.tangentOut = ReadFiniteFloat(
                    keyJson, "tangentOut", 0.0f);
                emitter.beam.widthKeys.push_back(key);
            }
        }
        if (beam.contains("arcs"))
        {
            const nlohmann::json& arcs = beam.at("arcs");
            if (!arcs.is_array() || arcs.empty() ||
                arcs.size() > kMaxParticleBeamCount)
            {
                throw std::invalid_argument(
                    "Particle beam.arcs requires 1..16 entries");
            }
            for (const nlohmann::json& arcJson : arcs)
            {
                if (!arcJson.is_object())
                {
                    throw std::invalid_argument(
                        "Particle beam arc must be an object");
                }
                ParticleBeamArc arc;
                arc.colorStart = ReadVec3(
                    arcJson, "colorStart", arc.colorStart);
                arc.colorEnd = ReadVec3(
                    arcJson, "colorEnd", arc.colorEnd);
                arc.widthScale = ReadFiniteFloat(
                    arcJson, "widthScale", arc.widthScale);
                arc.intensity = ReadFiniteFloat(
                    arcJson, "intensity", arc.intensity);
                arc.noisePhase = ReadFiniteFloat(
                    arcJson, "noisePhase", arc.noisePhase);
                emitter.beam.arcs.push_back(arc);
            }
        }
        if (beam.contains("hitScan"))
        {
            const nlohmann::json& hitScan = beam.at("hitScan");
            if (!hitScan.is_object())
            {
                throw std::invalid_argument(
                    "Particle beam.hitScan must be an object");
            }
            if (hitScan.contains("enabled"))
            {
                if (!hitScan.at("enabled").is_boolean())
                {
                    throw std::invalid_argument(
                        "Particle beam.hitScan.enabled must be boolean");
                }
                emitter.beam.hitScan.enabled =
                    hitScan.at("enabled").get<bool>();
            }
            else
            {
                emitter.beam.hitScan.enabled = true;
            }
            emitter.beam.hitScan.planeNormal = ReadVec3(
                hitScan, "planeNormal",
                emitter.beam.hitScan.planeNormal);
            emitter.beam.hitScan.planeOffset = ReadFiniteFloat(
                hitScan, "planeOffset",
                emitter.beam.hitScan.planeOffset);
            emitter.beam.hitScan.maxDistance = ReadFiniteFloat(
                hitScan, "maxDistance",
                emitter.beam.hitScan.maxDistance);
        }
    }
}

void ParseEmitterCollision(const nlohmann::json& json, ParticleEmitterAsset& emitter, const std::filesystem::path& resourceRoot, const ParticleCollisionResourceCache* collisionCache)
{
    if (json.contains("collision"))
    {
        const nlohmann::json& collision = json.at("collision");
        if (!collision.is_object())
        {
            throw std::invalid_argument(
                "Particle collision must be an object");
        }
        emitter.collisionType = collision.value(
            "type", emitter.collisionType);
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
        if (collision.contains("plane"))
        {
            const nlohmann::json& plane = collision.at("plane");
            if (!plane.is_object())
            {
                throw std::invalid_argument(
                    "Particle collision plane must be an object");
            }
            emitter.collisionPlaneNormal =
                NormalizeParticleCollisionPlaneNormal(
                    ReadVec3(
                        plane,
                        "normal",
                        emitter.collisionPlaneNormal),
                    "collision.plane.normal");
            emitter.collisionPlaneOffset = ReadFiniteFloat(
                plane,
                "offset",
                emitter.collisionPlaneOffset);
        }
        if (collision.contains("box"))
        {
            const nlohmann::json& box = collision.at("box");
            if (!box.is_object())
            {
                throw std::invalid_argument(
                    "Particle collision box must be an object");
            }
            emitter.collisionBoxMin = ReadVec3(
                box,
                "min",
                emitter.collisionBoxMin);
            emitter.collisionBoxMax = ReadVec3(
                box,
                "max",
                emitter.collisionBoxMax);
        }
        if (collision.contains("capsule"))
        {
            const nlohmann::json& capsule = collision.at("capsule");
            if (!capsule.is_object())
            {
                throw std::invalid_argument(
                    "Particle collision capsule must be an object");
            }
            emitter.collisionCapsuleA = ReadVec3(
                capsule,
                "a",
                emitter.collisionCapsuleA);
            emitter.collisionCapsuleB = ReadVec3(
                capsule,
                "b",
                emitter.collisionCapsuleB);
            emitter.collisionCapsuleRadius = ReadFiniteFloat(
                capsule,
                "radius",
                emitter.collisionCapsuleRadius);
        }
        if (collision.contains("sdf"))
        {
            const nlohmann::json& sdf = collision.at("sdf");
            if (!sdf.is_object())
            {
                throw std::invalid_argument(
                    "Particle collision sdf must be an object");
            }
            if (sdf.contains("path"))
            {
                if (!sdf.at("path").is_string())
                {
                    throw std::invalid_argument(
                        "Particle collision.sdf.path must be a string");
                }
                emitter.collisionSdfPath =
                    sdf.at("path").get<std::string>();
            }
        }
        if (collision.contains("mesh"))
        {
            const nlohmann::json& mesh = collision.at("mesh");
            if (!mesh.is_object())
            {
                throw std::invalid_argument(
                    "Particle collision mesh must be an object");
            }
            if (mesh.contains("path"))
            {
                if (!mesh.at("path").is_string())
                {
                    throw std::invalid_argument(
                        "Particle collision.mesh.path must be a string");
                }
                emitter.collisionMeshPath =
                    mesh.at("path").get<std::string>();
            }
        }
        emitter.collisionBounce = ReadFiniteFloat(
            collision,
            "bounce",
            emitter.collisionBounce);
        emitter.collisionFriction = ReadFiniteFloat(
            collision,
            "friction",
            emitter.collisionFriction);
        emitter.collisionRadius = ReadFiniteFloat(
            collision,
            "radius",
            emitter.collisionRadius);
        emitter.killOnCollision = collision.value(
            "killOnCollision",
            emitter.killOnCollision);
        emitter.collisionCcd = collision.value(
            "ccd",
            emitter.collisionCcd);
        if (emitter.collisionType == "sdf")
        {
            if (emitter.collisionSdfPath.empty())
            {
                throw std::invalid_argument(
                    "Particle collision type 'sdf' requires "
                    "collision.sdf.path");
            }
            if (collisionCache != nullptr)
            {
                const auto cached = collisionCache->find(
                    ParticleCollisionResourceCacheKey(
                        emitter.collisionType,
                        emitter.collisionSdfPath));
                if (cached != collisionCache->end())
                {
                    emitter.collisionSdf = cached->second;
                }
            }
            if (!emitter.collisionSdf)
            {
                emitter.collisionSdf =
                    LoadParticleSdfAsset(
                        emitter.collisionSdfPath, resourceRoot);
            }
        }
        else if (emitter.collisionType == "mesh")
        {
            if (emitter.collisionMeshPath.empty())
            {
                throw std::invalid_argument(
                    "Particle collision type 'mesh' requires "
                    "collision.mesh.path");
            }
            // 网格在提交前烘成 SDF；运行时 GPU 只看到 SDF 类型 4，
            // 不会退回 plane/box 或静默关闭碰撞。
            if (collisionCache != nullptr)
            {
                const auto cached = collisionCache->find(
                    ParticleCollisionResourceCacheKey(
                        emitter.collisionType,
                        emitter.collisionMeshPath));
                if (cached != collisionCache->end())
                {
                    emitter.collisionSdf = cached->second;
                }
            }
            if (!emitter.collisionSdf)
            {
                emitter.collisionSdf =
                    LoadParticleCollisionMeshAsset(
                        emitter.collisionMeshPath,
                        resourceRoot,
                        emitter.collisionRadius);
            }
        }
    }
}

void ParseEmitterFlipbook(const nlohmann::json& json, ParticleEmitterAsset& emitter)
{
    emitter.texturePath =
        json.value("texture", emitter.texturePath);
    emitter.eooPacked = json.value(
        "eooPacked", emitter.eooPacked);
    emitter.eooEmissiveStrength = ReadFiniteFloat(
        json, "eooEmissiveStrength", emitter.eooEmissiveStrength);
    if (json.contains("flipbook"))
    {
        const nlohmann::json& flipbook = json.at("flipbook");
        if (!flipbook.is_object())
        {
            throw std::invalid_argument(
                "Particle flipbook must be an object");
        }
        emitter.flipbookColumns = ReadCountField(
            flipbook, "columns", emitter.flipbookColumns);
        emitter.flipbookRows = ReadCountField(
            flipbook, "rows", emitter.flipbookRows);
        emitter.flipbookFramesPerSecond = ReadFiniteFloat(
            flipbook, "fps", emitter.flipbookFramesPerSecond);
        emitter.flipbookStartFrame = ReadFrameIndex(
            flipbook, "startFrame", emitter.flipbookStartFrame);
        // 只有作者显式写出 endFrame 时才把它当作真实帧号；
        // 省略时解析为 atlas 最后一帧，而不是把 0 永远当哨兵。
        const uint32_t frameCount =
            emitter.flipbookColumns * emitter.flipbookRows;
        emitter.flipbookEndFrame = flipbook.contains("endFrame")
            ? ReadFrameIndex(flipbook, "endFrame", 0)
            : (frameCount > 0 ? frameCount - 1 : 0);
        emitter.flipbookLoop = flipbook.value(
            "loop", emitter.flipbookLoop);
        emitter.flipbookRandomStartFrame = flipbook.value(
            "randomStartFrame",
            emitter.flipbookRandomStartFrame);
        emitter.flipbookPerParticle = flipbook.value(
            "perParticle",
            emitter.flipbookPerParticle);
        emitter.flipbookBlend = flipbook.value(
            "blend",
            emitter.flipbookBlend);
        emitter.flipbookPlayRate = ReadFiniteFloat(
            flipbook,
            "playRate",
            emitter.flipbookPlayRate);
        emitter.flipbookStartFrameJitter = ReadFiniteFloat(
            flipbook,
            "startFrameJitter",
            emitter.flipbookStartFrameJitter);
        auto readUvCurve = [&](const char* field,
                               std::vector<ParticleFlipbookUvKey>& keys)
        {
            if (!flipbook.contains(field))
            {
                return;
            }
            const nlohmann::json& curve = flipbook.at(field);
            if (!curve.is_array() ||
                curve.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    std::string("Particle flipbook.") + field +
                    " allows at most 8 keys");
            }
            keys.clear();
            float previousTime = -1.0f;
            for (const nlohmann::json& entry : curve)
            {
                if (!entry.is_object())
                {
                    throw std::invalid_argument(
                        std::string("Particle flipbook.") + field +
                        " entry must be an object");
                }
                ParticleFlipbookUvKey key;
                key.time = ReadFiniteFloat(entry, "time", 0.0f);
                const nlohmann::json& value = entry.at("value");
                if (!value.is_array() || value.size() != 2)
                {
                    throw std::invalid_argument(
                        std::string("Particle flipbook.") + field +
                        " value requires [x,y]");
                }
                key.value = Eigen::Vector2f(
                    ReadFiniteNumber(value.at(0), "value.x"),
                    ReadFiniteNumber(value.at(1), "value.y"));
                if (key.time < 0.0f || key.time > 1.0f ||
                    key.time <= previousTime)
                {
                    throw std::invalid_argument(
                        std::string("Particle flipbook.") + field +
                        " times must be in [0,1] and strictly increasing");
                }
                previousTime = key.time;
                keys.push_back(key);
            }
        };
        readUvCurve("uvOffsetKeys", emitter.flipbookUvOffsetKeys);
        readUvCurve("uvScaleKeys", emitter.flipbookUvScaleKeys);
        if (flipbook.contains("playRateKeys"))
        {
            const nlohmann::json& curve = flipbook.at("playRateKeys");
            if (!curve.is_array() ||
                curve.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    "Particle flipbook.playRateKeys allows at most 8 keys");
            }
            emitter.flipbookPlayRateKeys.clear();
            float previousTime = -1.0f;
            for (const nlohmann::json& entry : curve)
            {
                if (!entry.is_object())
                {
                    throw std::invalid_argument(
                        "Particle flipbook.playRateKeys entry must be an object");
                }
                ParticleFlipbookRateKey key;
                key.time = ReadFiniteFloat(entry, "time", 0.0f);
                key.rate = ReadFiniteFloat(entry, "rate", 1.0f);
                if (key.time < 0.0f || key.time > 1.0f ||
                    key.time <= previousTime)
                {
                    throw std::invalid_argument(
                        "Particle flipbook.playRateKeys times must be in "
                        "[0,1] and strictly increasing");
                }
                if (key.rate < 0.0f || key.rate > 16.0f)
                {
                    throw std::invalid_argument(
                        "Particle flipbook.playRateKeys rate must be in [0,16]");
                }
                previousTime = key.time;
                emitter.flipbookPlayRateKeys.push_back(key);
            }
        }

        if (flipbook.contains("framePivotKeys"))
        {
            const nlohmann::json& keys =
                flipbook.at("framePivotKeys");
            if (!keys.is_array() || keys.empty() ||
                keys.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    "Particle flipbook.framePivotKeys requires 1..8 "
                    "entries");
            }
            float previousTime = -1.0f;
            for (const nlohmann::json& entry : keys)
            {
                if (!entry.is_object())
                {
                    throw std::invalid_argument(
                        "Particle flipbook.framePivotKeys entry must be "
                        "an object");
                }
                ParticleFlipbookPivotKey key;
                key.time = ReadFiniteFloat(entry, "time", 0.0f);
                key.pivot = ReadVec2(
                    entry, "pivot", key.pivot);
                key.tangentIn = ReadVec2Tangent(
                    entry, "tangentIn");
                key.tangentOut = ReadVec2Tangent(
                    entry, "tangentOut");
                if (key.time < 0.0f || key.time > 1.0f ||
                    key.time <= previousTime)
                {
                    throw std::invalid_argument(
                        "Particle flipbook.framePivotKeys times must be "
                        "in [0,1] and strictly increasing");
                }
                for (int axis = 0; axis < 2; ++axis)
                {
                    if (key.pivot[axis] < -8.0f ||
                        key.pivot[axis] > 8.0f)
                    {
                        throw std::invalid_argument(
                            "Particle flipbook.framePivotKeys pivot must "
                            "be in [-8,8]");
                    }
                }
                previousTime = key.time;
                emitter.flipbookFramePivotKeys.push_back(key);
            }
        }

        if (flipbook.contains("frameRotationKeys"))
        {
            const nlohmann::json& keys =
                flipbook.at("frameRotationKeys");
            if (!keys.is_array() || keys.empty() ||
                keys.size() > kMaxParticleCurveKeys)
            {
                throw std::invalid_argument(
                    "Particle flipbook.frameRotationKeys requires 1..8 "
                    "entries");
            }
            float previousTime = -1.0f;
            for (const nlohmann::json& entry : keys)
            {
                if (!entry.is_object())
                {
                    throw std::invalid_argument(
                        "Particle flipbook.frameRotationKeys entry must "
                        "be an object");
                }
                ParticleFlipbookRotationKey key;
                key.time = ReadFiniteFloat(entry, "time", 0.0f);
                key.degrees = ReadFiniteFloat(
                    entry, "degrees", 0.0f);
                key.tangentIn = ReadFiniteFloat(
                    entry, "tangentIn", 0.0f);
                key.tangentOut = ReadFiniteFloat(
                    entry, "tangentOut", 0.0f);
                if (key.time < 0.0f || key.time > 1.0f ||
                    key.time <= previousTime)
                {
                    throw std::invalid_argument(
                        "Particle flipbook.frameRotationKeys times must "
                        "be in [0,1] and strictly increasing");
                }
                previousTime = key.time;
                emitter.flipbookFrameRotationKeys.push_back(key);
            }
        }

        if (flipbook.contains("frameDurations"))
        {
            const nlohmann::json& durations =
                flipbook.at("frameDurations");
            if (!durations.is_array() ||
                durations.size() > kMaxParticleFlipbookFrames)
            {
                throw std::invalid_argument(
                    "Particle flipbook frameDurations requires an array "
                    "of at most 256 entries");
            }
            emitter.flipbookFrameDurations.reserve(durations.size());
            for (const nlohmann::json& entry : durations)
            {
                const float duration = ReadFiniteNumber(
                    entry, "flipbook.frameDurations");
                if (duration <= 0.0f)
                {
                    throw std::invalid_argument(
                        "Particle flipbook frameDuration must be finite "
                        "and positive");
                }
                emitter.flipbookFrameDurations.push_back(duration);
            }
        }

        if (flipbook.contains("framePivots"))
        {
            const nlohmann::json& pivots =
                flipbook.at("framePivots");
            if (!pivots.is_array() ||
                pivots.size() > kMaxParticleFlipbookFrames)
            {
                throw std::invalid_argument(
                    "Particle flipbook framePivots requires an array "
                    "of at most 256 entries");
            }
            emitter.flipbookFramePivots.reserve(pivots.size());
            for (const nlohmann::json& entry : pivots)
            {
                if (!entry.is_array() || entry.size() != 2)
                {
                    throw std::invalid_argument(
                        "Particle flipbook framePivot requires [u, v]");
                }
                Eigen::Vector2f pivot(
                    ReadFiniteNumber(
                        entry.at(0), "flipbook.framePivots.u"),
                    ReadFiniteNumber(
                        entry.at(1), "flipbook.framePivots.v"));
                if (!std::isfinite(pivot.x()) ||
                    !std::isfinite(pivot.y()) ||
                    pivot.x() < 0.0f || pivot.x() > 1.0f ||
                    pivot.y() < 0.0f || pivot.y() > 1.0f)
                {
                    throw std::invalid_argument(
                        "Particle flipbook framePivot components must be "
                        "finite and in [0, 1]");
                }
                emitter.flipbookFramePivots.push_back(pivot);
            }
        }

        if (flipbook.contains("frameUVs"))
        {
            const nlohmann::json& uvs = flipbook.at("frameUVs");
            if (!uvs.is_array() ||
                uvs.size() > kMaxParticleFlipbookFrames)
            {
                throw std::invalid_argument(
                    "Particle flipbook frameUVs requires an array "
                    "of at most 256 entries");
            }
            emitter.flipbookFrameUVs.reserve(uvs.size());
            for (const nlohmann::json& entry : uvs)
            {
                if (!entry.is_array() || entry.size() != 4)
                {
                    throw std::invalid_argument(
                        "Particle flipbook frameUV requires "
                        "[u0, v0, u1, v1]");
                }
                Eigen::Vector4f rect(
                    ReadFiniteNumber(
                        entry.at(0), "flipbook.frameUVs.u0"),
                    ReadFiniteNumber(
                        entry.at(1), "flipbook.frameUVs.v0"),
                    ReadFiniteNumber(
                        entry.at(2), "flipbook.frameUVs.u1"),
                    ReadFiniteNumber(
                        entry.at(3), "flipbook.frameUVs.v1"));
                for (int component = 0; component < 4; ++component)
                {
                    if (!std::isfinite(rect[component]) ||
                        rect[component] < 0.0f ||
                        rect[component] > 1.0f)
                    {
                        throw std::invalid_argument(
                            "Particle flipbook frameUV components must be "
                            "finite and in [0, 1]");
                    }
                }
                if (rect.x() >= rect.z() || rect.y() >= rect.w())
                {
                    throw std::invalid_argument(
                        "Particle flipbook frameUV requires u0 < u1 "
                        "and v0 < v1");
                }
                emitter.flipbookFrameUVs.push_back(rect);
            }
        }

        if (flipbook.contains("frameRotations"))
        {
            const nlohmann::json& rotations =
                flipbook.at("frameRotations");
            if (!rotations.is_array() ||
                rotations.size() > kMaxParticleFlipbookFrames)
            {
                throw std::invalid_argument(
                    "Particle flipbook frameRotations requires an array "
                    "of at most 256 entries");
            }
            emitter.flipbookFrameRotations.reserve(rotations.size());
            for (const nlohmann::json& entry : rotations)
            {
                emitter.flipbookFrameRotations.push_back(
                    ReadFiniteNumber(
                        entry, "flipbook.frameRotations"));
            }
        }
    }
    else
    {
        const uint32_t frameCount =
            emitter.flipbookColumns * emitter.flipbookRows;
        emitter.flipbookEndFrame =
            frameCount > 0 ? frameCount - 1 : 0;
    }
}

} // namespace ParticleAssetDetail
} // namespace VL
