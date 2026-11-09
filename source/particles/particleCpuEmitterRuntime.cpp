#include "particles/particleCpuEmitterRuntime.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "particles/particleBurst.h"
#include "particles/particleEmitterKey.h"
#include "render/frontend/renderScene.h"

namespace VL
{
namespace
{

constexpr float kMaxSimulationDeltaSeconds = 0.1f;

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

// 只包含会改变粒子状态的字段；颜色/强度/半径/decal 材质参数可以在运行时
// 热改而无需清空已经在飞的粒子。
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

Eigen::Matrix4f BuildEmitterModel(
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

void ParticleCpuEmitterRuntime::Reset()
{
    emitterStates.clear();
    orderedStates.clear();
    worldGeneration = 0;
    hasWorldGeneration = false;
}

void ParticleCpuEmitterRuntime::ResetEmitterState(EmitterState& state)
{
    state.particles.clear();
    state.spawnAccumulator = 0.0f;
    state.elapsedSeconds = 0.0f;
    state.burstElapsedSeconds = 0.0f;
    state.initialBurstDone = false;
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

void ParticleCpuEmitterRuntime::Synchronize(
    const RenderScene& renderScene,
    const std::unordered_map<std::string, ParticleSystemAsset>&
        resolvedSystemAssets,
    std::string_view renderMode,
    const std::unordered_set<std::string>* excludeKeys)
{
    orderedStates.clear();
    if (!hasWorldGeneration ||
        worldGeneration != renderScene.worldGeneration)
    {
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
        const std::string resolvedKey = BuildParticleSystemAssetKey(
            snapshot.name, snapshot.assetPath);
        const auto resolvedIt =
            resolvedSystemAssets.find(resolvedKey);
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
            if (emitter.renderMode != renderMode)
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
            if (excludeKeys != nullptr &&
                excludeKeys->find(key) != excludeKeys->end())
            {
                continue;
            }
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
                state.emitter = emitter;
            }
            state.restartEpoch = restartEpoch;
            state.model = BuildEmitterModel(snapshot, emitter);
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

void ParticleCpuEmitterRuntime::SpawnParticle(
    EmitterState& state,
    uint32_t capacity,
    uint32_t& globalLiveCount)
{
    const uint32_t perEmitterCap = std::min<uint32_t>(
        state.emitter.maxParticles, capacity);
    if (state.particles.size() >= perEmitterCap ||
        globalLiveCount >= capacity)
    {
        return;
    }

    uint32_t slot = 0;
    while (slot < perEmitterCap)
    {
        bool used = false;
        for (const Particle& particle : state.particles)
        {
            if (particle.slot == slot)
            {
                used = true;
                break;
            }
        }
        if (!used)
        {
            break;
        }
        ++slot;
    }
    if (slot >= perEmitterCap)
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

    Particle particle;
    particle.lifetime = NextRange(
        state.randomState,
        emitter.lifetimeMin,
        emitter.lifetimeMax);
    particle.lifetime = std::max(particle.lifetime, 1.0e-4f);
    particle.age = 0.0f;
    particle.slot = slot;
    particle.serial = state.nextSerial++;
    if (emitter.localSpace)
    {
        particle.position = localPosition;
        particle.velocity = localVelocity;
    }
    else
    {
        const Eigen::Vector4f worldPosition =
            state.model * Eigen::Vector4f(
                localPosition.x(), localPosition.y(),
                localPosition.z(), 1.0f);
        particle.position = worldPosition.head<3>();
        particle.velocity =
            state.model.block<3, 3>(0, 0) * localVelocity;
    }
    state.particles.push_back(particle);
    ++globalLiveCount;
}

void ParticleCpuEmitterRuntime::AdvanceEmitterStep(
    EmitterState& state,
    float stepSeconds,
    uint32_t& globalLiveCount)
{
    if (!(stepSeconds > 0.0f))
    {
        return;
    }
    const ParticleEmitterAsset& emitter = state.emitter;

    const size_t previousCount = state.particles.size();
    for (Particle& particle : state.particles)
    {
        particle.age += stepSeconds;
    }
    state.particles.erase(
        std::remove_if(
            state.particles.begin(), state.particles.end(),
            [](const Particle& particle)
            {
                return particle.age >= particle.lifetime;
            }),
        state.particles.end());
    globalLiveCount -= static_cast<uint32_t>(
        previousCount - state.particles.size());
    for (Particle& particle : state.particles)
    {
        const float damping =
            std::exp(-emitter.drag * stepSeconds);
        particle.velocity =
            particle.velocity * damping +
            emitter.gravity * stepSeconds;
        particle.position += particle.velocity * stepSeconds;
    }

    state.elapsedSeconds += stepSeconds;
}

void ParticleCpuEmitterRuntime::TrimToCapacity(
    uint32_t capacity,
    uint32_t& globalLiveCount)
{
    if (globalLiveCount <= capacity)
    {
        return;
    }
    for (auto stateIt = orderedStates.rbegin();
         stateIt != orderedStates.rend() &&
             globalLiveCount > capacity;
         ++stateIt)
    {
        EmitterState& state = **stateIt;
        while (!state.particles.empty() &&
            globalLiveCount > capacity)
        {
            state.particles.pop_back();
            --globalLiveCount;
        }
    }
}

void ParticleCpuEmitterRuntime::Simulate(
    const RenderScene& renderScene,
    uint32_t capacity)
{
    orderedStates.clear();
    orderedStates.reserve(emitterStates.size());
    for (auto& entry : emitterStates)
    {
        orderedStates.push_back(&entry.second);
    }
    std::sort(
        orderedStates.begin(), orderedStates.end(),
        [](const EmitterState* lhs, const EmitterState* rhs)
        {
            if (lhs->order != rhs->order)
            {
                return lhs->order < rhs->order;
            }
            return lhs->key < rhs->key;
        });

    uint32_t globalLiveCount = 0;
    for (const EmitterState* state : orderedStates)
    {
        globalLiveCount += static_cast<uint32_t>(
            state->particles.size());
    }
    TrimToCapacity(capacity, globalLiveCount);

    float rawDelta = renderScene.simulationDeltaSeconds;
    if (!std::isfinite(rawDelta) || rawDelta < 0.0f)
    {
        rawDelta = 0.0f;
    }
    const float cappedMotionDelta =
        std::min(rawDelta, kMaxSimulationDeltaSeconds);

    // fixedTimeStep>0 时与 GPU 路径一致：累积器只推进完整步，最多
    // 8 步/帧；余量跨帧保留。没有完整步时不老化、不生成，但仍输出
    // 当前存活粒子，保证暂停/低帧率下画面不会突然消失。
    for (EmitterState* state : orderedStates)
    {
        const ParticleEmitterAsset& emitter = state->emitter;
        // 固定步长路径由 maxSteps=8 自己限制单帧追赶量；非固定步长才
        // 用 0.1 s 上限保护积分稳定性。
        const float motionDelta =
            emitter.fixedTimeStep > 0.0f
                ? rawDelta
                : cappedMotionDelta;
        const ParticleFixedStepTick tick =
            AdvanceParticleFixedStep(
                motionDelta,
                emitter.fixedTimeStep,
                8u,
                state->fixedStep);
        for (uint32_t step = 0; step < tick.steps; ++step)
        {
            AdvanceEmitterStep(
                *state, tick.stepSeconds, globalLiveCount);
        }
        const float frameSeconds =
            tick.stepSeconds * static_cast<float>(tick.steps);
        if (!(frameSeconds > 0.0f))
        {
            continue;
        }

        // burst 用绝对累计时间跨周期：同一帧跨过多个 interval 也只重触发
        // 一次，间隔不是 fixedStep 整数倍时不会累积相位误差。首次生成在
        // 第一个模拟步完成，之后 interval<=0 保持一次性 burst 语义。
        // fixedStep 只累计已经推进的完整步；非固定步长则使用 raw delta，
        // 因为大帧跳需要让 burst 周期看到真实经过时间，而年龄/位移仍由
        // motionDelta 限制。
        const float burstFrameSeconds =
            emitter.fixedTimeStep > 0.0f ? frameSeconds : rawDelta;
        const float previousBurstTime = state->burstElapsedSeconds;
        state->burstElapsedSeconds += burstFrameSeconds;
        bool burstDue = false;
        const bool initialBurst =
            emitter.burst && !state->initialBurstDone;
        if (emitter.burst)
        {
            if (initialBurst)
            {
                burstDue = true;
            }
            else if (emitter.burstInterval > 0.0f)
            {
                burstDue = ParticleBurstCycleCrossed(
                    previousBurstTime,
                    state->burstElapsedSeconds,
                    emitter.burstInterval);
            }
        }
        if (burstDue)
        {
            globalLiveCount -= static_cast<uint32_t>(
                state->particles.size());
            state->particles.clear();
            ++state->burstGeneration;
        }

        const uint32_t perEmitterCap = std::min<uint32_t>(
            emitter.maxParticles, capacity);
        const uint32_t perEmitterAvailable =
            state->particles.size() < perEmitterCap
                ? perEmitterCap -
                    static_cast<uint32_t>(state->particles.size())
                : 0u;
        const uint32_t globalAvailable =
            globalLiveCount < capacity
                ? capacity - globalLiveCount
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
                std::max(state->spawnRateScale, 0.0f);
            state->spawnAccumulator += rate * frameSeconds;
            const uint32_t requested = static_cast<uint32_t>(
                std::floor(state->spawnAccumulator));
            state->spawnAccumulator -= static_cast<float>(requested);
            spawnCount = std::min(
                std::min(requested, perEmitterAvailable),
                globalAvailable);
        }
        for (uint32_t index = 0; index < spawnCount; ++index)
        {
            SpawnParticle(*state, capacity, globalLiveCount);
        }
        // 只有真正生成成功后，一次性 initial burst 才算完成；容量为 0
        // 或全局槽位耗尽时保留 pending，释放容量后下一帧会重试。
        if (initialBurst && spawnCount > 0u)
        {
            state->initialBurstDone = true;
        }
    }
}

uint32_t ParticleCpuEmitterRuntime::GetLiveParticleCount() const
{
    uint32_t count = 0;
    for (const auto& entry : emitterStates)
    {
        count += static_cast<uint32_t>(entry.second.particles.size());
    }
    return count;
}

} // namespace VL
