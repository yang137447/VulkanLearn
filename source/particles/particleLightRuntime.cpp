#include "particles/particleLightRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>

#include "particles/particleEmitterKey.h"
#include "render/frontend/renderScene.h"

namespace VL
{
namespace
{

constexpr float kMaxSimulationDeltaSeconds = 0.1f;
constexpr float kLargeBurstTimer = 1.0e30f;

uint64_t HashString(std::string_view value, uint64_t seed)
{
    constexpr uint64_t fnvPrime = 1099511628211ull;
    uint64_t hash = seed == 0 ? 14695981039346656037ull : seed;
    for (unsigned char character : value)
    {
        hash ^= character;
        hash *= fnvPrime;
    }
    return hash;
}

uint32_t NextU32(uint64_t& state)
{
    // SplitMix64：不依赖标准库分布实现，跨平台结果稳定。
    state += 0x9E3779B97F4A7C15ull;
    uint64_t value = state;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    value ^= value >> 31;
    return static_cast<uint32_t>(value >> 32);
}

float Next01(uint64_t& state)
{
    return static_cast<float>(NextU32(state)) * (1.0f / 4294967296.0f);
}

float NextRange(uint64_t& state, float minimum, float maximum)
{
    return minimum + (maximum - minimum) * Next01(state);
}

void MixSignature(
    uint64_t& hash,
    const void* data,
    size_t size)
{
    const unsigned char* bytes =
        static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

void MixUint32(uint64_t& hash, uint32_t value)
{
    MixSignature(hash, &value, sizeof(value));
}

void MixFloat(uint64_t& hash, float value)
{
    MixSignature(hash, &value, sizeof(value));
}

void MixVec3(uint64_t& hash, const Eigen::Vector3f& value)
{
    MixFloat(hash, value.x());
    MixFloat(hash, value.y());
    MixFloat(hash, value.z());
}

// 只包含会改变粒子状态的字段；颜色/强度/半径可以在运行时热改而无需
// 清空已经在飞的灯光。
uint64_t BuildStateSignature(const ParticleEmitterAsset& emitter)
{
    uint64_t hash = 14695981039346656037ull;
    MixUint32(hash, emitter.maxParticles);
    MixUint32(hash, emitter.burst ? 1u : 0u);
    MixUint32(hash, emitter.localSpace ? 1u : 0u);
    MixUint32(hash, emitter.randomSeed);
    MixFloat(hash, emitter.burstInterval);
    MixFloat(hash, emitter.spawnRate);
    MixFloat(hash, emitter.lifetimeMin);
    MixFloat(hash, emitter.lifetimeMax);
    MixFloat(hash, emitter.drag);
    MixFloat(hash, emitter.fixedTimeStep);
    MixVec3(hash, emitter.gravity);
    MixVec3(hash, emitter.velocityMin);
    MixVec3(hash, emitter.velocityMax);
    MixVec3(hash, emitter.shapeExtent);
    MixFloat(hash, emitter.shapeRadius);
    MixFloat(hash, emitter.shapeHeight);
    MixFloat(hash, emitter.shapeInnerRadius);
    for (unsigned char character : emitter.shapeType)
    {
        MixUint32(hash, character);
    }
    return hash;
}

Eigen::Matrix4f BuildLightEmitterModel(
    const ParticleSystemSnapshot& snapshot,
    const ParticleEmitterAsset& emitter)
{
    Eigen::Matrix4f translation = Eigen::Matrix4f::Identity();
    translation.block<3, 1>(0, 3) = emitter.localPosition;
    return snapshot.model * translation;
}

int ShapeCode(const std::string& shape)
{
    if (shape == "box") return 1;
    if (shape == "cone") return 2;
    if (shape == "cylinder") return 3;
    if (shape == "disc") return 4;
    if (shape == "ring") return 5;
    return 0;
}

Eigen::Vector3f SampleShapePosition(
    const ParticleEmitterAsset& emitter,
    uint64_t& randomState)
{
    constexpr float twoPi = 6.28318530718f;
    const float radius = std::max(emitter.shapeRadius, 0.0f);
    const float height = std::max(emitter.shapeHeight, 0.0f);
    const float innerRadius = std::clamp(
        emitter.shapeInnerRadius, 0.0f, radius);
    const int shapeCode = ShapeCode(emitter.shapeType);
    if (shapeCode == 0)
    {
        const float cosTheta = 1.0f - 2.0f * Next01(randomState);
        const float sinTheta = std::sqrt(std::max(
            0.0f, 1.0f - cosTheta * cosTheta));
        const float phi = twoPi * Next01(randomState);
        const Eigen::Vector3f direction(
            sinTheta * std::cos(phi),
            cosTheta,
            sinTheta * std::sin(phi));
        return direction *
            (radius * std::pow(Next01(randomState), 1.0f / 3.0f));
    }
    if (shapeCode == 1)
    {
        const Eigen::Vector3f extent =
            emitter.shapeExtent.cwiseMax(0.0f);
        return Eigen::Vector3f(
            (Next01(randomState) * 2.0f - 1.0f) * extent.x(),
            (Next01(randomState) * 2.0f - 1.0f) * extent.y(),
            (Next01(randomState) * 2.0f - 1.0f) * extent.z());
    }

    const float phi = twoPi * Next01(randomState);
    const Eigen::Vector2f radialDirection(
        std::cos(phi), std::sin(phi));
    if (shapeCode == 2)
    {
        const float layer = Next01(randomState);
        const float layerRadius = radius * (1.0f - layer);
        return Eigen::Vector3f(
            radialDirection.x() * layerRadius *
                std::sqrt(Next01(randomState)),
            height * layer,
            radialDirection.y() * layerRadius *
                std::sqrt(Next01(randomState)));
    }
    if (shapeCode == 3)
    {
        const float layerRadius =
            radius * std::sqrt(Next01(randomState));
        return Eigen::Vector3f(
            radialDirection.x() * layerRadius,
            height * (Next01(randomState) - 0.5f),
            radialDirection.y() * layerRadius);
    }
    if (shapeCode == 4)
    {
        const float discRadius =
            radius * std::sqrt(Next01(randomState));
        return Eigen::Vector3f(
            radialDirection.x() * discRadius,
            0.0f,
            radialDirection.y() * discRadius);
    }
    const float innerSquared = innerRadius * innerRadius;
    const float outerSquared = radius * radius;
    const float ringRadius = std::sqrt(
        innerSquared +
        (outerSquared - innerSquared) * Next01(randomState));
    return Eigen::Vector3f(
        radialDirection.x() * ringRadius,
        0.0f,
        radialDirection.y() * ringRadius);
}

} // namespace

void ParticleLightRuntime::Reset()
{
    emitterStates.clear();
    dynamicLightIds.clear();
    worldGeneration = 0;
    hasWorldGeneration = false;
}

void ParticleLightRuntime::ResetEmitterState(EmitterState& state)
{
    state.lights.clear();
    state.spawnAccumulator = 0.0f;
    state.burstTimer = 0.0f;
    state.fixedStep = ParticleFixedStepState{};
    state.burstGeneration = 0;
    state.nextSerial = 0;
    state.randomState = HashString(
        state.key,
        static_cast<uint64_t>(state.emitter.randomSeed) + 0x9E3779B9ull);
    if (state.randomState == 0)
    {
        state.randomState = 0x9E3779B97F4A7C15ull;
    }
}

void ParticleLightRuntime::Synchronize(
    const RenderScene& renderScene,
    const std::unordered_map<std::string, ParticleSystemAsset>&
        resolvedSystemAssets)
{
    if (!hasWorldGeneration ||
        worldGeneration != renderScene.worldGeneration)
    {
        // 保留 dynamicLightIds：旧灯光仍在 RenderScene::lights 里，必须让
        // 下一次 Update 先移除它们；状态本身按新代际重建。
        emitterStates.clear();
        worldGeneration = renderScene.worldGeneration;
        hasWorldGeneration = true;
    }

    uint32_t order = 0;
    std::unordered_set<std::string> desired;
    for (const ParticleSystemSnapshot& snapshot :
         renderScene.particleSystems)
    {
        if (!snapshot.visible)
        {
            continue;
        }
        std::unordered_map<std::string,
            const ParticleEmitterTelemetry*> telemetryByName;
        telemetryByName.reserve(snapshot.telemetry.emitters.size());
        for (const ParticleEmitterTelemetry& telemetry :
             snapshot.telemetry.emitters)
        {
            if (!telemetry.name.empty())
            {
                telemetryByName.emplace(
                    telemetry.name, &telemetry);
            }
        }
        const auto resolvedIt =
            resolvedSystemAssets.find(snapshot.name);
        const ParticleSystemAsset& resolvedAsset =
            resolvedIt != resolvedSystemAssets.end()
                ? resolvedIt->second
                : snapshot.asset;
        for (size_t emitterIndex = 0;
             emitterIndex < resolvedAsset.emitters.size();
             ++emitterIndex)
        {
            const ParticleEmitterAsset& emitter =
                resolvedAsset.emitters[emitterIndex];
            if (emitter.renderMode != "light")
            {
                continue;
            }
            if (emitterIndex >= kMaxParticleEmitters ||
                ((snapshot.runtime.enabledEmitterMask >>
                    emitterIndex) & 1u) == 0)
            {
                continue;
            }
            const auto telemetryIt =
                telemetryByName.find(emitter.name);
            if (telemetryIt != telemetryByName.end())
            {
                const ParticleEmitterTelemetry& telemetry =
                    *telemetryIt->second;
                if (!telemetry.active || !telemetry.enabled)
                {
                    continue;
                }
            }

            const std::string key = BuildParticleEmitterKey(
                snapshot.name, snapshot.assetPath, emitter.name);
            desired.insert(key);
            const uint64_t signature =
                BuildStateSignature(emitter);
            auto stateIt = emitterStates.find(key);
            if (stateIt == emitterStates.end())
            {
                EmitterState state;
                state.key = key;
                state.emitter = emitter;
                state.signature = signature;
                ResetEmitterState(state);
                stateIt = emitterStates.emplace(
                    key, std::move(state)).first;
            }
            EmitterState& state = stateIt->second;
            const uint32_t restartEpoch = telemetryIt !=
                    telemetryByName.end()
                ? telemetryIt->second->restartEpoch
                : 0u;
            if (state.signature != signature ||
                state.restartEpoch != restartEpoch)
            {
                state.emitter = emitter;
                state.signature = signature;
                ResetEmitterState(state);
            }
            else
            {
                // 颜色/强度/半径允许逐帧热改，不清空已经在飞的灯光。
                state.emitter = emitter;
            }
            state.restartEpoch = restartEpoch;
            state.model = BuildLightEmitterModel(snapshot, emitter);
            state.colorTint = snapshot.runtime.colorTint;
            state.spawnRateScale =
                snapshot.runtime.spawnRateScale;
            state.order = order++;
        }
    }

    for (auto stateIt = emitterStates.begin();
         stateIt != emitterStates.end();)
    {
        if (desired.find(stateIt->first) == desired.end())
        {
            stateIt = emitterStates.erase(stateIt);
        }
        else
        {
            ++stateIt;
        }
    }
}

void ParticleLightRuntime::SpawnLight(
    EmitterState& state,
    uint32_t& globalLiveCount)
{
    const uint32_t perEmitterCap = std::min<uint32_t>(
        state.emitter.maxParticles, kMaxLiveLights);
    if (state.lights.size() >= perEmitterCap ||
        globalLiveCount >= kMaxLiveLights)
    {
        return;
    }

    const ParticleEmitterAsset& emitter = state.emitter;
    const Eigen::Vector3f localPosition =
        SampleShapePosition(emitter, state.randomState);
    const Eigen::Vector3f localVelocity(
        NextRange(state.randomState, emitter.velocityMin.x(),
            emitter.velocityMax.x()),
        NextRange(state.randomState, emitter.velocityMin.y(),
            emitter.velocityMax.y()),
        NextRange(state.randomState, emitter.velocityMin.z(),
            emitter.velocityMax.z()));

    LiveLight light;
    light.lifetime = NextRange(
        state.randomState,
        emitter.lifetimeMin,
        emitter.lifetimeMax);
    light.lifetime = std::max(light.lifetime, 1.0e-4f);
    light.age = 0.0f;
    if (emitter.localSpace)
    {
        light.position = localPosition;
        light.velocity = localVelocity;
    }
    else
    {
        const Eigen::Vector4f worldPosition =
            state.model * Eigen::Vector4f(
                localPosition.x(), localPosition.y(),
                localPosition.z(), 1.0f);
        light.position = worldPosition.head<3>();
        light.velocity =
            state.model.block<3, 3>(0, 0) * localVelocity;
    }
    light.lightId = HashString(
        state.key + "#" + std::to_string(state.burstGeneration) +
            ":" + std::to_string(state.nextSerial++),
        0x9E3779B97F4A7C15ull);
    state.lights.push_back(light);
    ++globalLiveCount;
}

void ParticleLightRuntime::SimulateEmitterStep(
    EmitterState& state,
    float stepSeconds,
    uint32_t& globalLiveCount)
{
    if (!(stepSeconds > 0.0f))
    {
        return;
    }
    const ParticleEmitterAsset& emitter = state.emitter;

    const size_t previousCount = state.lights.size();
    for (LiveLight& light : state.lights)
    {
        light.age += stepSeconds;
    }
    state.lights.erase(
        std::remove_if(
            state.lights.begin(), state.lights.end(),
            [](const LiveLight& light)
            {
                return light.age >= light.lifetime;
            }),
        state.lights.end());
    globalLiveCount -= static_cast<uint32_t>(
        previousCount - state.lights.size());
    for (LiveLight& light : state.lights)
    {
        const float damping =
            std::exp(-emitter.drag * stepSeconds);
        light.velocity =
            light.velocity * damping + emitter.gravity * stepSeconds;
        light.position += light.velocity * stepSeconds;
    }

    bool burstDue = false;
    if (emitter.burst)
    {
        state.burstTimer -= stepSeconds;
        if (state.burstTimer <= 0.0f)
        {
            // 与 GPU burst 合同一致：周期到点整池重触发，而不是等旧
            // 灯光自然死亡后逐个补位。
            globalLiveCount -= static_cast<uint32_t>(
                state.lights.size());
            state.lights.clear();
            state.burstTimer = emitter.burstInterval > 0.0f
                ? emitter.burstInterval
                : kLargeBurstTimer;
            ++state.burstGeneration;
            burstDue = true;
        }
    }

    const uint32_t perEmitterCap = std::min<uint32_t>(
        emitter.maxParticles, kMaxLiveLights);
    const uint32_t perEmitterAvailable =
        state.lights.size() < perEmitterCap
            ? perEmitterCap -
                static_cast<uint32_t>(state.lights.size())
            : 0u;
    const uint32_t globalAvailable =
        globalLiveCount < kMaxLiveLights
            ? kMaxLiveLights - globalLiveCount
            : 0u;
    uint32_t spawnCount = 0;
    if (emitter.burst)
    {
        if (burstDue)
        {
            spawnCount = std::min(
                perEmitterAvailable, globalAvailable);
        }
    }
    else
    {
        const float rate = std::max(emitter.spawnRate, 0.0f) *
            std::max(state.spawnRateScale, 0.0f);
        state.spawnAccumulator += rate * stepSeconds;
        if (state.spawnAccumulator >= 1.0f)
        {
            const uint32_t requested =
                static_cast<uint32_t>(std::floor(
                    state.spawnAccumulator));
            state.spawnAccumulator -= static_cast<float>(
                requested);
            spawnCount = std::min(
                std::min(requested, perEmitterAvailable),
                globalAvailable);
        }
    }
    for (uint32_t index = 0; index < spawnCount; ++index)
    {
        SpawnLight(state, globalLiveCount);
    }
}

void ParticleLightRuntime::EmitLightSnapshot(
    const EmitterState& state,
    const LiveLight& light,
    RenderScene& renderScene)
{
    const ParticleEmitterAsset& emitter = state.emitter;
    const float lifeT = std::clamp(
        light.lifetime > 0.0f
            ? light.age / light.lifetime
            : 1.0f,
        0.0f, 1.0f);
    Eigen::Vector3f color =
        (1.0f - lifeT) * emitter.colorStart.head<3>() +
        lifeT * emitter.colorEnd.head<3>();
    color = color.cwiseProduct(state.colorTint.head<3>());
    const float alpha = std::clamp(
        ((1.0f - lifeT) * emitter.colorStart.w() +
            lifeT * emitter.colorEnd.w()) *
            state.colorTint.w(),
        0.0f, 1.0f);

    LightSnapshot snapshot;
    snapshot.lightId = light.lightId;
    snapshot.type = LightSnapshotType::Point;
    snapshot.color = color;
    snapshot.intensity =
        emitter.light.intensity * (1.0f - lifeT) * alpha;
    snapshot.radius = emitter.light.radius;
    snapshot.position = emitter.localSpace
        ? (state.model * Eigen::Vector4f(
            light.position.x(), light.position.y(),
            light.position.z(), 1.0f)).head<3>()
        : light.position;
    renderScene.lights.push_back(snapshot);
    dynamicLightIds.insert(snapshot.lightId);
}

void ParticleLightRuntime::Update(RenderScene& renderScene)
{
    if (!dynamicLightIds.empty())
    {
        std::vector<LightSnapshot>& lights = renderScene.lights;
        lights.erase(
            std::remove_if(
                lights.begin(), lights.end(),
                [this](const LightSnapshot& light)
                {
                    return dynamicLightIds.find(light.lightId) !=
                        dynamicLightIds.end();
                }),
            lights.end());
        dynamicLightIds.clear();
    }

    if (emitterStates.empty())
    {
        return;
    }

    float delta = renderScene.simulationDeltaSeconds;
    if (!std::isfinite(delta) || delta < 0.0f)
    {
        delta = 0.0f;
    }
    delta = std::min(delta, kMaxSimulationDeltaSeconds);

    std::vector<EmitterState*> ordered;
    ordered.reserve(emitterStates.size());
    for (auto& entry : emitterStates)
    {
        ordered.push_back(&entry.second);
    }
    std::sort(
        ordered.begin(), ordered.end(),
        [](const EmitterState* lhs, const EmitterState* rhs)
        {
            if (lhs->order != rhs->order)
            {
                return lhs->order < rhs->order;
            }
            return lhs->key < rhs->key;
        });

    uint32_t globalLiveCount = 0;
    for (const EmitterState* state : ordered)
    {
        globalLiveCount += static_cast<uint32_t>(
            state->lights.size());
    }
    // fixedTimeStep>0 时与 GPU 路径一致：累积器只推进完整步，最多
    // 8 步/帧；余量跨帧保留。没有完整步时不老化、不生成，但仍输出
    // 当前存活灯，保证暂停/低帧率下画面不会突然消失。
    for (EmitterState* state : ordered)
    {
        const ParticleEmitterAsset& emitter = state->emitter;
        const ParticleFixedStepTick tick =
            AdvanceParticleFixedStep(
                delta,
                emitter.fixedTimeStep,
                8u,
                state->fixedStep);
        for (uint32_t step = 0; step < tick.steps; ++step)
        {
            SimulateEmitterStep(
                *state, tick.stepSeconds, globalLiveCount);
        }
    }
    for (EmitterState* state : ordered)
    {
        for (const LiveLight& light : state->lights)
        {
            EmitLightSnapshot(*state, light, renderScene);
        }
    }
}

uint32_t ParticleLightRuntime::GetLiveLightCount() const
{
    uint32_t count = 0;
    for (const auto& entry : emitterStates)
    {
        count += static_cast<uint32_t>(entry.second.lights.size());
    }
    return count;
}

} // namespace VL
