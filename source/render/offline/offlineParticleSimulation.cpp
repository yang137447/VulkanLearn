#include "render/offline/offlineParticleSimulation.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "particles/particleCollision.h"
#include "particles/particleEmitterBounds.h"
#include "particles/particleLifetime.h"
#include "particles/particleLod.h"

namespace VL
{
namespace
{

constexpr float kTwoPi = 6.28318530718f;

float Mix(float first, float second, float weight)
{
    return first + (second - first) * weight;
}

Eigen::Vector3f Mix(
    const Eigen::Vector3f& first,
    const Eigen::Vector3f& second,
    const Eigen::Vector3f& weight)
{
    return Eigen::Vector3f(
        Mix(first.x(), second.x(), weight.x()),
        Mix(first.y(), second.y(), weight.y()),
        Mix(first.z(), second.z(), weight.z()));
}

// 与 particleSimulate.comp 的 PCG hash 完全一致；同一 index/时间/预热
// 标志在 CPU 与 GPU 上得到同一随机流。
uint32_t PcgHash(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float NextRandom(uint32_t& state)
{
    state = state * 747796405u + 2891336453u;
    uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word = (word >> 22u) ^ word;
    return static_cast<float>(word & 0x00ffffffu) / 16777215.0f;
}

float Fract(float value)
{
    return value - std::floor(value);
}

uint32_t CellBits(int value)
{
    // GLSL 的 uint(int) 按 2^32 取模；C++ 静态转换保持同一行为。
    return static_cast<uint32_t>(static_cast<int32_t>(value));
}

float HashNoiseLattice(const Eigen::Vector3i& cell)
{
    uint32_t hash = CellBits(cell.x()) * 0x8da6b343u;
    hash ^= CellBits(cell.y()) * 0xd8163841u;
    hash ^= CellBits(cell.z()) * 0xcb1ab31fu;
    return static_cast<float>(PcgHash(hash) & 0x00ffffffu) / 16777215.0f;
}

float ValueNoise3D(const Eigen::Vector3f& position)
{
    const Eigen::Vector3f cell(std::floor(position.x()),
        std::floor(position.y()), std::floor(position.z()));
    Eigen::Vector3f fraction(
        Fract(position.x()), Fract(position.y()), Fract(position.z()));
    fraction = Eigen::Vector3f(
        fraction.x() * fraction.x() * (3.0f - 2.0f * fraction.x()),
        fraction.y() * fraction.y() * (3.0f - 2.0f * fraction.y()),
        fraction.z() * fraction.z() * (3.0f - 2.0f * fraction.z()));
    const Eigen::Vector3i base(
        static_cast<int>(cell.x()),
        static_cast<int>(cell.y()),
        static_cast<int>(cell.z()));

    const float n000 = HashNoiseLattice(base + Eigen::Vector3i(0, 0, 0));
    const float n100 = HashNoiseLattice(base + Eigen::Vector3i(1, 0, 0));
    const float n010 = HashNoiseLattice(base + Eigen::Vector3i(0, 1, 0));
    const float n110 = HashNoiseLattice(base + Eigen::Vector3i(1, 1, 0));
    const float n001 = HashNoiseLattice(base + Eigen::Vector3i(0, 0, 1));
    const float n101 = HashNoiseLattice(base + Eigen::Vector3i(1, 0, 1));
    const float n011 = HashNoiseLattice(base + Eigen::Vector3i(0, 1, 1));
    const float n111 = HashNoiseLattice(base + Eigen::Vector3i(1, 1, 1));

    return Mix(
        Mix(Mix(n000, n100, fraction.x()),
            Mix(n010, n110, fraction.x()), fraction.y()),
        Mix(Mix(n001, n101, fraction.x()),
            Mix(n011, n111, fraction.x()), fraction.y()),
        fraction.z());
}

Eigen::Vector3f CurlNoisePotential(const Eigen::Vector3f& position)
{
    const Eigen::Vector3f offsetX(0.0f, 0.0f, 0.0f);
    const Eigen::Vector3f offsetY(37.1f, 11.7f, 71.3f);
    const Eigen::Vector3f offsetZ(83.9f, 47.5f, 19.1f);
    return Eigen::Vector3f(
        ValueNoise3D(position + offsetX),
        ValueNoise3D(position + offsetY),
        ValueNoise3D(position + offsetZ));
}

Eigen::Vector3f CurlNoiseForce(
    const ParticleEmitterAsset& asset,
    const Eigen::Vector3f& position,
    float time)
{
    const float strength = asset.curlNoiseStrength;
    const float frequency = asset.curlNoiseFrequency;
    if (!(strength > 0.0f) || !(frequency > 0.0f))
    {
        return Eigen::Vector3f::Zero();
    }
    if (!position.allFinite() || !std::isfinite(time * asset.curlNoiseSpeed))
    {
        return Eigen::Vector3f::Zero();
    }

    const Eigen::Vector3f samplePosition =
        position * frequency +
        Eigen::Vector3f(0.0f, 0.0f, time * asset.curlNoiseSpeed);
    const float epsilon = 0.01f;
    const Eigen::Vector3f unitX(epsilon, 0.0f, 0.0f);
    const Eigen::Vector3f unitY(0.0f, epsilon, 0.0f);
    const Eigen::Vector3f unitZ(0.0f, 0.0f, epsilon);
    const Eigen::Vector3f x0 = CurlNoisePotential(samplePosition - unitX);
    const Eigen::Vector3f x1 = CurlNoisePotential(samplePosition + unitX);
    const Eigen::Vector3f y0 = CurlNoisePotential(samplePosition - unitY);
    const Eigen::Vector3f y1 = CurlNoisePotential(samplePosition + unitY);
    const Eigen::Vector3f z0 = CurlNoisePotential(samplePosition - unitZ);
    const Eigen::Vector3f z1 = CurlNoisePotential(samplePosition + unitZ);
    const Eigen::Vector3f curl(
        (y1.z() - y0.z()) - (z1.y() - z0.y()),
        (z1.x() - z0.x()) - (x1.z() - x0.z()),
        (x1.y() - x0.y()) - (y1.x() - y0.x()));
    return curl * (strength / (2.0f * epsilon));
}

Eigen::Vector3f VortexForce(
    const ParticleEmitterAsset& asset,
    const Eigen::Vector3f& position,
    const Eigen::Matrix4f& model)
{
    const float strength = asset.vortexStrength;
    const float radius = asset.vortexRadius;
    if (!(strength > 0.0f) || !(radius > 0.0f))
    {
        return Eigen::Vector3f::Zero();
    }
    const float axisLength = asset.vortexAxis.norm();
    if (!(axisLength > 1.0e-6f))
    {
        return Eigen::Vector3f::Zero();
    }
    const Eigen::Vector3f axis = asset.vortexAxis / axisLength;
    // localSpace 发射器在局部空间模拟，涡旋中心是局部原点；世界空间
    // 发射器使用模型平移，与 shader 的 localToWorld[3] 一致。
    Eigen::Vector3f center = Eigen::Vector3f::Zero();
    if (!asset.localSpace)
    {
        center = model.block<3, 1>(0, 3);
    }
    const Eigen::Vector3f radial = position - center;
    const float distance = radial.norm();
    if (!(distance > 1.0e-6f))
    {
        return Eigen::Vector3f::Zero();
    }
    const float falloff = std::clamp(1.0f - distance / radius, 0.0f, 1.0f);
    if (!(falloff > 0.0f))
    {
        return Eigen::Vector3f::Zero();
    }
    const Eigen::Vector3f tangent = axis.cross(radial);
    const float tangentLength = tangent.norm();
    if (!(tangentLength > 1.0e-6f))
    {
        return Eigen::Vector3f::Zero();
    }
    return tangent / tangentLength * (strength * falloff);
}

Eigen::Vector3f AttractorForce(
    const ParticleEmitterAsset& asset,
    const Eigen::Vector3f& position)
{
    const float strength = asset.attractorStrength;
    const float radius = asset.attractorRadius;
    if (!(strength > 0.0f) || !(radius > 0.0f))
    {
        return Eigen::Vector3f::Zero();
    }
    const Eigen::Vector3f offset = asset.attractorPosition - position;
    const float distance = offset.norm();
    if (!(distance > 1.0e-6f))
    {
        return Eigen::Vector3f::Zero();
    }
    const float ratio = distance / radius;
    const float falloff = 1.0f / (1.0f + ratio * ratio);
    return offset / distance * (strength * falloff);
}

int ShapeCode(const std::string& shapeType)
{
    if (shapeType == "box") return 1;
    if (shapeType == "cone") return 2;
    if (shapeType == "cylinder") return 3;
    if (shapeType == "disc") return 4;
    if (shapeType == "ring") return 5;
    return 0;
}

Eigen::Vector3f SampleSpawnPosition(
    const ParticleEmitterAsset& asset,
    uint32_t& randomState)
{
    const int shapeType = ShapeCode(asset.shapeType);
    const float radius = std::max(asset.shapeRadius, 0.0f);
    const float height = std::max(asset.shapeHeight, 0.0f);
    const float innerRadius =
        std::clamp(asset.shapeInnerRadius, 0.0f, radius);

    if (shapeType == 0)
    {
        // 球体体积采样：方向均匀，半径按 cbrt(u) 分布。
        const float cosTheta = 1.0f - 2.0f * NextRandom(randomState);
        const float sinTheta =
            std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        const float phi = kTwoPi * NextRandom(randomState);
        const Eigen::Vector3f direction(
            sinTheta * std::cos(phi),
            cosTheta,
            sinTheta * std::sin(phi));
        return direction *
            (radius * std::pow(NextRandom(randomState), 1.0f / 3.0f));
    }
    if (shapeType == 1)
    {
        const Eigen::Vector3f extent(
            std::max(asset.shapeExtent.x(), 0.0f),
            std::max(asset.shapeExtent.y(), 0.0f),
            std::max(asset.shapeExtent.z(), 0.0f));
        return Eigen::Vector3f(
            (NextRandom(randomState) * 2.0f - 1.0f) * extent.x(),
            (NextRandom(randomState) * 2.0f - 1.0f) * extent.y(),
            (NextRandom(randomState) * 2.0f - 1.0f) * extent.z());
    }

    const float phi = kTwoPi * NextRandom(randomState);
    const Eigen::Vector2f radialDirection(std::cos(phi), std::sin(phi));
    if (shapeType == 2)
    {
        const float t = NextRandom(randomState);
        const float layerRadius = radius * (1.0f - t);
        return Eigen::Vector3f(
            radialDirection.x() * layerRadius *
                std::sqrt(NextRandom(randomState)),
            height * t,
            radialDirection.y() * layerRadius *
                std::sqrt(NextRandom(randomState)));
    }
    if (shapeType == 3)
    {
        const float layerRadius = radius * std::sqrt(NextRandom(randomState));
        return Eigen::Vector3f(
            radialDirection.x() * layerRadius,
            height * (NextRandom(randomState) - 0.5f),
            radialDirection.y() * layerRadius);
    }
    if (shapeType == 4)
    {
        const float discRadius = radius * std::sqrt(NextRandom(randomState));
        return Eigen::Vector3f(
            radialDirection.x() * discRadius,
            0.0f,
            radialDirection.y() * discRadius);
    }
    // ring：对 r² 线性插值后开方，保证面积均匀。
    const float innerSquared = innerRadius * innerRadius;
    const float outerSquared = radius * radius;
    const float ringRadius = std::sqrt(Mix(
        innerSquared, outerSquared, NextRandom(randomState)));
    return Eigen::Vector3f(
        radialDirection.x() * ringRadius,
        0.0f,
        radialDirection.y() * ringRadius);
}

void SpawnParticle(
    OfflineParticleEmitterState& emitter,
    uint32_t index,
    uint32_t& randomState,
    bool prewarm)
{
    const ParticleEmitterAsset& asset = emitter.asset;
    const Eigen::Vector3f localPosition =
        SampleSpawnPosition(asset, randomState);
    const Eigen::Vector3f localVelocity = Mix(
        asset.velocityMin,
        asset.velocityMax,
        Eigen::Vector3f(NextRandom(randomState),
            NextRandom(randomState), NextRandom(randomState)));

    const float lifetime = Mix(
        asset.lifetimeMin, asset.lifetimeMax, NextRandom(randomState));
    const bool burst = asset.burst;
    float age = (prewarm && !burst)
        ? NextRandom(randomState) * lifetime * 0.95f
        : 0.0f;

    Eigen::Vector3f position = localPosition;
    Eigen::Vector3f velocity = localVelocity;
    if (!asset.localSpace)
    {
        position = (emitter.model * Eigen::Vector4f(
            localPosition.x(), localPosition.y(), localPosition.z(), 1.0f))
            .head<3>();
        velocity = emitter.model.block<3, 3>(0, 0) * localVelocity;
    }

    if (prewarm && age > 0.0f)
    {
        // 解析解：dv/dt = g - k·v。与 shader 的 prewarm 积分同式；
        // 运行时曲线不参与预热，避免把作者时间倍率用在“已存在”历史上。
        const Eigen::Vector3f gravity = asset.gravity;
        const float drag = std::max(asset.drag, 0.0f);
        if (drag <= 1.0e-5f)
        {
            position += velocity * age + gravity * (0.5f * age * age);
            velocity += gravity * age;
        }
        else
        {
            const Eigen::Vector3f terminal = gravity / drag;
            const float decay = std::exp(-drag * age);
            position += terminal * age +
                (velocity - terminal) * ((1.0f - decay) / drag);
            velocity = terminal + (velocity - terminal) * decay;
        }
    }

    // localSpace 发射器的碰撞体按局部空间作者化，与粒子坐标同空间；
    // 因此只有碰撞类型存在时才求交，不再按 localSpace 跳过。
    if (ParticleCollisionTypeValue(asset.collisionType) != 0)
    {
        ParticleCollisionState collision{position, velocity, true};
        if (!ResolveParticleCollision(collision, asset))
        {
            age = lifetime;
        }
        position = collision.position;
        velocity = collision.velocity;
    }

    OfflineParticle& particle = emitter.particles[index];
    particle.positionAge =
        Eigen::Vector4f(position.x(), position.y(), position.z(), age);
    particle.velocityLife =
        Eigen::Vector4f(velocity.x(), velocity.y(), velocity.z(), lifetime);
    // 新粒子的三个历史点都等于生成位置；首帧 ribbon 退化为零长度。
    particle.history0 = particle.positionAge;
    particle.history1 = particle.positionAge;
    particle.history2 = particle.positionAge;
}

void IntegrateParticle(
    OfflineParticleEmitterState& emitter,
    OfflineParticle& particle,
    float deltaSeconds)
{
    const ParticleEmitterAsset& asset = emitter.asset;
    const float lifetime = particle.velocityLife.w();
    const float lifeT = std::clamp(
        particle.positionAge.w() / std::max(lifetime, 1.0e-6f), 0.0f, 1.0f);
    const float scaledDelta = deltaSeconds *
        EvaluateParticleLifetimeScale(asset.lifetimeScaleKeys, lifeT);
    if (!(scaledDelta > 0.0f))
    {
        return;
    }
    particle.positionAge.w() += scaledDelta;
    if (particle.positionAge.w() >= lifetime)
    {
        particle.positionAge.w() = lifetime;
        return;
    }

    // 历史右移在积分之前：history0 记录本帧起点。
    particle.history2 = particle.history1;
    particle.history1 = particle.history0;
    particle.history0 = particle.positionAge;

    Eigen::Vector3f position = particle.positionAge.head<3>();
    Eigen::Vector3f velocity = particle.velocityLife.head<3>();
    float age = particle.positionAge.w();
    const int steps =
        ParticleCollisionStepCount(velocity, asset, scaledDelta);
    const float stepSeconds = scaledDelta / static_cast<float>(steps);
    const float damping = std::exp(-std::max(asset.drag, 0.0f) * stepSeconds);
    for (int step = 0; step < steps; ++step)
    {
        Eigen::Vector3f force = asset.gravity + asset.wind;
        force += CurlNoiseForce(asset, position, emitter.elapsedSeconds);
        force += VortexForce(asset, position, emitter.model);
        force += AttractorForce(asset, position);
        velocity += force * stepSeconds;
        velocity *= damping;
        position += velocity * stepSeconds;

        ParticleCollisionState collision{position, velocity, true};
        if (!ResolveParticleCollision(collision, asset))
        {
            age = lifetime;
            position = collision.position;
            velocity = collision.velocity;
            break;
        }
        position = collision.position;
        velocity = collision.velocity;
    }

    particle.positionAge =
        Eigen::Vector4f(position.x(), position.y(), position.z(), age);
    particle.velocityLife.head<3>() = velocity;
}

struct ParticleSystemNameOrder
{
    bool operator()(
        const ParticleSystemSnapshot* left,
        const ParticleSystemSnapshot* right) const
    {
        if (left->name != right->name) return left->name < right->name;
        return left->particleSystemId < right->particleSystemId;
    }
};

} // namespace

std::vector<OfflineParticleSystemInput> BuildOfflineParticleInputs(
    const WorldSnapshot& snapshot,
    uint32_t seed)
{
    std::vector<const ParticleSystemSnapshot*> ordered;
    ordered.reserve(snapshot.particleSystems.size());
    for (const ParticleSystemSnapshot& system : snapshot.particleSystems)
    {
        ordered.push_back(&system);
    }
    std::stable_sort(
        ordered.begin(), ordered.end(), ParticleSystemNameOrder{});

    std::vector<OfflineParticleSystemInput> inputs;
    inputs.reserve(ordered.size());
    for (const ParticleSystemSnapshot* system : ordered)
    {
        OfflineParticleSystemInput input;
        input.name = system->name;
        input.assetPath = system->assetPath;
        input.asset = system->asset;
        input.model = system->model;
        input.visible = system->visible;
        input.runtime = system->runtime;
        input.seed = seed ^ StableOfflineParticleSeed(system->name);
        inputs.push_back(std::move(input));
    }
    return inputs;
}

OfflineParticleSystemState CreateOfflineParticleSystemState(
    const OfflineParticleSystemInput& input)
{
    OfflineParticleSystemState system;
    system.name = input.name;
    system.assetPath = input.assetPath;
    system.seed = input.seed;
    system.visible = input.visible;
    system.emitters.reserve(input.asset.emitters.size());
    for (size_t index = 0; index < input.asset.emitters.size(); ++index)
    {
        OfflineParticleEmitterState emitter;
        emitter.asset = input.asset.emitters[index];
        emitter.model = input.model;
        emitter.visible = input.visible;
        emitter.enabled = index < kMaxParticleEmitters &&
            (input.runtime.enabledEmitterMask & (1u << index)) != 0;
        emitter.spawnRateScale = input.runtime.spawnRateScale;
        emitter.colorTint = input.runtime.colorTint;
        ComputeParticleEmitterBounds(
            emitter.asset,
            emitter.model,
            emitter.worldCenter,
            emitter.worldRadius);
        // light 发射器没有 GPU 粒子池，也没有离线可见几何；不要为它
        // 分配/模拟粒子。保留 slot 以维持 enabledEmitterMask 与编辑器
        // 发射器索引的稳定性。
        if (emitter.asset.renderMode != "light")
        {
            emitter.particles.resize(emitter.asset.maxParticles);
        }
        system.emitters.push_back(std::move(emitter));
    }
    return system;
}

void StepOfflineParticleSystem(
    OfflineParticleSystemState& system,
    float deltaSeconds,
    const Eigen::Vector3f& cameraPosition)
{
    float dt = deltaSeconds;
    if (!std::isfinite(dt)) dt = 0.0f;
    dt = std::clamp(dt, 0.0f, 0.1f);

    for (OfflineParticleEmitterState& emitter : system.emitters)
    {
        // 隐藏/禁用与 GPU 的冻结路径一致：不推进 elapsed、不消费 prewarm。
        if (!emitter.visible || !emitter.enabled)
        {
            continue;
        }
        // CPU 粒子灯只服务实时 PointLight，离线合成器没有消费路径；
        // 跳过整个模拟，避免为不会显示也不会发光的粒子付出开销。
        if (emitter.asset.renderMode == "light")
        {
            continue;
        }
        const ParticleEmitterAsset& asset = emitter.asset;
        const bool prewarm = emitter.prewarm;
        emitter.elapsedSeconds += dt;

        const float lodDistance = std::isfinite(cameraPosition.x())
            ? (emitter.worldCenter - cameraPosition).norm()
            : 0.0f;
        const float effectiveSpawnRate =
            EffectiveParticleSpawnRate(asset, lodDistance) *
            emitter.spawnRateScale;
        const uint32_t count = static_cast<uint32_t>(
            std::min<size_t>(asset.maxParticles, emitter.particles.size()));
        if (count == 0)
        {
            emitter.prewarm = false;
            continue;
        }

        for (uint32_t index = 0; index < count; ++index)
        {
            const uint32_t randomState = PcgHash(
                index * 0x9e3779b9u ^
                static_cast<uint32_t>(emitter.elapsedSeconds * 1000.0f) ^
                (prewarm ? 0x51ed270bu : 0u) ^ system.seed);
            uint32_t state = randomState;

            if (prewarm)
            {
                if (!asset.burst)
                {
                    const float averageLifetime =
                        0.5f * (asset.lifetimeMin + asset.lifetimeMax);
                    const uint32_t target = static_cast<uint32_t>(std::clamp(
                        asset.spawnRate * emitter.spawnRateScale *
                            averageLifetime,
                        0.0f,
                        static_cast<float>(count)));
                    if (index >= target)
                    {
                        emitter.particles[index] = OfflineParticle{};
                        continue;
                    }
                }
                SpawnParticle(emitter, index, state, true);
                continue;
            }

            if (asset.burst && asset.burstInterval > 0.0f)
            {
                const float time = emitter.elapsedSeconds;
                const float previousTime = std::max(time - dt, 0.0f);
                if (std::floor(time / asset.burstInterval) >
                    std::floor(previousTime / asset.burstInterval))
                {
                    SpawnParticle(emitter, index, state, false);
                    continue;
                }
            }

            OfflineParticle& particle = emitter.particles[index];
            if (!particle.Alive())
            {
                if (asset.burst)
                {
                    continue;
                }
                const float spawnChance = std::clamp(
                    effectiveSpawnRate * dt / std::max(float(count), 1.0f),
                    0.0f,
                    1.0f);
                if (NextRandom(state) < spawnChance)
                {
                    SpawnParticle(emitter, index, state, false);
                }
                continue;
            }
            IntegrateParticle(emitter, particle, dt);
        }
        emitter.prewarm = false;
    }
}

OfflineParticleSystemState SimulateOfflineParticles(
    const OfflineParticleSystemInput& input,
    const OfflineParticlePreviewPolicy& policy,
    const Eigen::Vector3f& cameraPosition)
{
    OfflineParticleSystemState system =
        CreateOfflineParticleSystemState(input);
    const uint32_t steps = ComputeOfflineParticleStepCount(policy);
    for (uint32_t step = 0; step < steps; ++step)
    {
        StepOfflineParticleSystem(
            system, policy.fixedDeltaSeconds, cameraPosition);
    }
    return system;
}

} // namespace VL
