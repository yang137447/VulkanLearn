#include "particles/particleDecalRuntime.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

#include <Eigen/Geometry>

#include "particles/particleEmitterKey.h"
#include "particles/particleLod.h"
#include "render/frontend/renderScene.h"

namespace VL
{
namespace
{

constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;

Eigen::Matrix3f RotationDegreesToBasis(const Eigen::Vector3f& rotation)
{
    // 与 CommonFunction::RotationToMatrix 的 Z/Y/X 角度约定保持一致：
    // 先 roll(Z)、再 pitch(X)、最后 yaw(Y)。
    const Eigen::AngleAxisf yaw(
        rotation.y() * kDegreesToRadians,
        Eigen::Vector3f::UnitY());
    const Eigen::AngleAxisf pitch(
        rotation.x() * kDegreesToRadians,
        Eigen::Vector3f::UnitX());
    const Eigen::AngleAxisf roll(
        rotation.z() * kDegreesToRadians,
        Eigen::Vector3f::UnitZ());
    Eigen::Quaternionf quaternion = yaw * pitch * roll;
    quaternion.normalize();
    return quaternion.toRotationMatrix();
}

Eigen::Matrix3f EmitterOrientation(const Eigen::Matrix4f& model)
{
    // 只取父级方向，不把粒子系统/发射器的缩放带进 decal 体积；decal.size
    // 已经是作者在世界单位下的完整尺寸。列归一化可去掉附着物体的缩放。
    Eigen::Matrix3f basis = model.block<3, 3>(0, 0);
    for (int column = 0; column < 3; ++column)
    {
        const float length = basis.col(column).norm();
        if (!std::isfinite(length) || length <= 1.0e-6f)
        {
            return Eigen::Matrix3f::Identity();
        }
        basis.col(column) /= length;
    }
    Eigen::Quaternionf quaternion(basis);
    if (!std::isfinite(quaternion.w()) ||
        quaternion.norm() <= 1.0e-6f)
    {
        return Eigen::Matrix3f::Identity();
    }
    quaternion.normalize();
    return quaternion.toRotationMatrix();
}

Eigen::Matrix4f BuildEmitterModel(
    const ParticleSystemSnapshot& snapshot,
    const ParticleEmitterAsset& emitter)
{
    Eigen::Matrix4f translation = Eigen::Matrix4f::Identity();
    translation.block<3, 1>(0, 3) = emitter.localPosition;
    return snapshot.model * translation;
}

} // namespace

void ParticleDecalRuntime::Reset()
{
    simulation.Reset();
    dynamicDecalIds.clear();
    orderedEmitters.clear();
    gpuEmitters.clear();
    liveDecalCount = 0;
}

void ParticleDecalRuntime::Synchronize(
    const RenderScene& renderScene,
    const std::unordered_map<std::string, ParticleSystemAsset>&
        resolvedSystemAssets,
    const std::unordered_map<
        std::string, std::vector<ParticleDecalGpuInstance>>&
        gpuInstances)
{
    orderedEmitters.clear();
    gpuEmitters.clear();
    std::unordered_set<std::string> gpuKeys;

    // 这里的过滤顺序必须与 ParticleCpuEmitterRuntime::Synchronize 保持
    // 一致：同一个 decal emitter 只能出现在 CPU 或 GPU 一条路径上，
    // 预算截断也仍按 World snapshot / emitter 顺序确定性推进。
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
                telemetryByName.emplace(telemetry.name, &telemetry);
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
            if (emitter.renderMode != "decal")
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
            const bool gpu =
                gpuInstances.find(key) != gpuInstances.end();
            if (gpu)
            {
                gpuKeys.insert(key);
                GpuEmitterState state;
                state.key = key;
                state.emitter = emitter;
                state.model = BuildEmitterModel(snapshot, emitter);
                state.colorTint = snapshot.runtime.colorTint;
                gpuEmitters[key] = std::move(state);
            }
            orderedEmitters.push_back(OrderedEmitter{key, gpu});
        }
    }

    simulation.Synchronize(
        renderScene, resolvedSystemAssets, "decal", &gpuKeys);
}

void ParticleDecalRuntime::RemoveDynamicDecals(RenderScene& renderScene)
{
    if (dynamicDecalIds.empty())
    {
        return;
    }
    std::vector<DecalSnapshot>& decals = renderScene.decals;
    decals.erase(
        std::remove_if(
            decals.begin(), decals.end(),
            [this](const DecalSnapshot& decal)
            {
                return dynamicDecalIds.find(decal.decalId) !=
                    dynamicDecalIds.end();
            }),
        decals.end());
    dynamicDecalIds.clear();
}

void ParticleDecalRuntime::EmitDecalSnapshot(
    const ParticleCpuEmitterRuntime::EmitterState& state,
    const ParticleCpuEmitterRuntime::Particle& particle,
    RenderScene& renderScene)
{
    const ParticleEmitterAsset& emitter = state.emitter;
    const ParticleDecalAsset& settings = emitter.decal;
    const float lifeT = std::clamp(
        particle.lifetime > 0.0f
            ? particle.age / particle.lifetime
            : 1.0f,
        0.0f, 1.0f);

    const uint64_t decalId = ParticleDecalStableId(
        state.key, particle.slot);

    const Eigen::Vector3f position = emitter.localSpace
        ? (state.model * Eigen::Vector4f(
            particle.position.x(), particle.position.y(),
            particle.position.z(), 1.0f)).head<3>()
        : particle.position;
    const Eigen::Matrix3f orientation =
        EmitterOrientation(state.model) *
        RotationDegreesToBasis(settings.rotation);
    const Eigen::Vector3f emitterCenter =
        (state.model * Eigen::Vector4f(
            0.0f, 0.0f, 0.0f, 1.0f)).head<3>();
    const ParticleLodState lod = EffectiveParticleLod(
        emitter,
        (emitterCenter - renderScene.camera.position).norm());
    const Eigen::Vector3f halfExtent =
        settings.size * 0.5f * lod.renderScale;

    DecalSnapshot snapshot;
    snapshot.decalId = decalId;
    snapshot.name =
        emitter.name + "#" + std::to_string(particle.slot);
    snapshot.decalToWorld = Eigen::Matrix4f::Identity();
    snapshot.decalToWorld.block<3, 3>(0, 0) =
        orientation * halfExtent.asDiagonal();
    snapshot.decalToWorld.block<3, 1>(0, 3) = position;

    const Eigen::Vector3f lifeColor =
        (1.0f - lifeT) * emitter.colorStart.head<3>() +
        lifeT * emitter.colorEnd.head<3>();
    const Eigen::Vector3f color =
        settings.color.cwiseProduct(lifeColor).cwiseProduct(
            state.colorTint.head<3>());
    snapshot.color =
        Eigen::Vector4f(color.x(), color.y(), color.z(), 1.0f);

    const float lifeAlpha = std::clamp(
        ((1.0f - lifeT) * emitter.colorStart.w() +
            lifeT * emitter.colorEnd.w()) *
            state.colorTint.w(),
        0.0f, 1.0f);
    snapshot.opacity = std::clamp(
        settings.opacity * lifeAlpha, 0.0f, 1.0f);
    snapshot.edgeSoftness = settings.edgeSoftness;
    snapshot.noiseStrength = settings.noiseStrength;
    snapshot.randomSeed = static_cast<float>(
        (decalId >> 40) & 0xFFFFu) / 65535.0f;
    snapshot.roughness = settings.roughness;
    snapshot.metallic = settings.metallic;
    snapshot.ambientOcclusion = settings.ambientOcclusion;
    snapshot.emissive =
        settings.emissive.cwiseProduct(state.colorTint.head<3>());
    snapshot.emissiveStrength = settings.emissiveStrength;
    snapshot.responseMask = settings.responseMask;
    snapshot.normalStrength = settings.normalStrength;
    snapshot.albedoMap = settings.albedoMap;
    snapshot.normalMap = settings.normalMap;
    snapshot.ormMap = settings.ormMap;
    snapshot.sortOrder = settings.sortOrder;
    snapshot.visible = true;
    snapshot.persistentTextureSet = true;

    renderScene.decals.push_back(std::move(snapshot));
    dynamicDecalIds.insert(decalId);
}

void ParticleDecalRuntime::EmitDecalSnapshotFromGpu(
    const GpuEmitterState& state,
    const ParticleDecalGpuInstance& instance,
    RenderScene& renderScene)
{
    const ParticleEmitterAsset& emitter = state.emitter;
    const ParticleDecalAsset& settings = emitter.decal;
    const uint32_t slot = static_cast<uint32_t>(
        std::max(instance.rotationStable.w(), 0.0f) + 0.5f);
    const uint64_t decalId =
        ParticleDecalStableId(state.key, slot);

    const Eigen::Vector3f localPosition =
        instance.positionLifeT.head<3>();
    const Eigen::Vector3f position = emitter.localSpace
        ? (state.model * Eigen::Vector4f(
            localPosition.x(), localPosition.y(),
            localPosition.z(), 1.0f)).head<3>()
        : localPosition;
    const Eigen::Vector3f rotation =
        instance.rotationStable.head<3>();
    const Eigen::Matrix3f orientation =
        EmitterOrientation(state.model) *
        RotationDegreesToBasis(rotation);
    // gather 参数已经乘过 LOD renderScale，尺寸直接按 GPU 记录消费，
    // 不再在这里重复乘一次。
    const Eigen::Vector3f halfExtent =
        instance.sizeRotation.head<3>() * 0.5f;

    DecalSnapshot snapshot;
    snapshot.decalId = decalId;
    snapshot.name = emitter.name + "#" + std::to_string(slot);
    snapshot.decalToWorld = Eigen::Matrix4f::Identity();
    snapshot.decalToWorld.block<3, 3>(0, 0) =
        orientation * halfExtent.asDiagonal();
    snapshot.decalToWorld.block<3, 1>(0, 3) = position;

    snapshot.color = Eigen::Vector4f(
        instance.colorOpacity.x(),
        instance.colorOpacity.y(),
        instance.colorOpacity.z(),
        1.0f);
    snapshot.opacity = std::clamp(
        instance.colorOpacity.w(), 0.0f, 1.0f);
    snapshot.edgeSoftness = settings.edgeSoftness;
    snapshot.noiseStrength = settings.noiseStrength;
    snapshot.randomSeed = static_cast<float>(
        (decalId >> 40) & 0xFFFFu) / 65535.0f;
    snapshot.roughness = settings.roughness;
    snapshot.metallic = settings.metallic;
    snapshot.ambientOcclusion = settings.ambientOcclusion;
    // colorOpacity 已包含实例 colorTint 的 RGB；emissive 仍按同一 tint
    // 缩放，保持与 CPU 路径的材质语义一致。
    snapshot.emissive =
        settings.emissive.cwiseProduct(state.colorTint.head<3>());
    snapshot.emissiveStrength = settings.emissiveStrength;
    snapshot.responseMask = settings.responseMask;
    snapshot.normalStrength = settings.normalStrength;
    snapshot.albedoMap = settings.albedoMap;
    snapshot.normalMap = settings.normalMap;
    snapshot.ormMap = settings.ormMap;
    snapshot.sortOrder = settings.sortOrder;
    snapshot.visible = true;
    snapshot.persistentTextureSet = true;

    renderScene.decals.push_back(std::move(snapshot));
    dynamicDecalIds.insert(decalId);
}

void ParticleDecalRuntime::Update(
    RenderScene& renderScene,
    uint32_t maxTotalDecals,
    const std::unordered_map<
        std::string, std::vector<ParticleDecalGpuInstance>>&
        gpuInstances)
{
    RemoveDynamicDecals(renderScene);
    liveDecalCount = 0;

    uint32_t staticDecalCount = 0;
    for (const DecalSnapshot& decal : renderScene.decals)
    {
        if (decal.visible)
        {
            ++staticDecalCount;
        }
    }
    uint32_t dynamicCapacity =
        staticDecalCount < maxTotalDecals
            ? maxTotalDecals - staticDecalCount
            : 0u;

    // 只有非 GPU 接管（或无 GPU 池）的 decal emitter 仍在 CPU runtime 中；
    // Simulate 的容量同样使用全局剩余槽位，保持与旧路径一致。
    simulation.Simulate(renderScene, dynamicCapacity);

    std::unordered_map<std::string,
        const ParticleCpuEmitterRuntime::EmitterState*> cpuStates;
    cpuStates.reserve(simulation.GetOrderedStates().size());
    for (const ParticleCpuEmitterRuntime::EmitterState* state :
         simulation.GetOrderedStates())
    {
        cpuStates.emplace(state->key, state);
    }

    for (const OrderedEmitter& ordered : orderedEmitters)
    {
        if (dynamicCapacity == 0)
        {
            break;
        }
        if (ordered.gpu)
        {
            const auto instanceIt =
                gpuInstances.find(ordered.key);
            const auto stateIt = gpuEmitters.find(ordered.key);
            if (instanceIt == gpuInstances.end() ||
                stateIt == gpuEmitters.end())
            {
                continue;
            }
            for (const ParticleDecalGpuInstance& instance :
                 instanceIt->second)
            {
                if (dynamicCapacity == 0)
                {
                    break;
                }
                EmitDecalSnapshotFromGpu(
                    stateIt->second, instance, renderScene);
                --dynamicCapacity;
                ++liveDecalCount;
            }
            continue;
        }

        const auto stateIt = cpuStates.find(ordered.key);
        if (stateIt == cpuStates.end())
        {
            continue;
        }
        for (const ParticleCpuEmitterRuntime::Particle& particle :
             stateIt->second->particles)
        {
            if (dynamicCapacity == 0)
            {
                break;
            }
            EmitDecalSnapshot(
                *stateIt->second, particle, renderScene);
            --dynamicCapacity;
            ++liveDecalCount;
        }
    }
}

} // namespace VL
