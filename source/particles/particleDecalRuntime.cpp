#include "particles/particleDecalRuntime.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

#include <Eigen/Geometry>

#include "render/frontend/renderScene.h"

namespace VL
{
namespace
{

constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;

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

} // namespace

void ParticleDecalRuntime::Reset()
{
    simulation.Reset();
    dynamicDecalIds.clear();
}

void ParticleDecalRuntime::Synchronize(
    const RenderScene& renderScene,
    const std::unordered_map<std::string, ParticleSystemAsset>&
        resolvedSystemAssets)
{
    simulation.Synchronize(
        renderScene, resolvedSystemAssets, "decal");
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

    // 粒子槽位稳定，且 decalId 不包含 burst 代际，因此 DecalRenderer 的
    // 纹理集可在同一发射器反复 burst/重生成时复用，不会按生命周期换 key。
    const uint64_t decalId = HashString(
        "particle-decal|" + state.key + "#" +
            std::to_string(particle.slot),
        0x9E3779B97F4A7C15ull);

    const Eigen::Vector3f position = emitter.localSpace
        ? (state.model * Eigen::Vector4f(
            particle.position.x(), particle.position.y(),
            particle.position.z(), 1.0f)).head<3>()
        : particle.position;
    const Eigen::Matrix3f orientation =
        EmitterOrientation(state.model) *
        RotationDegreesToBasis(settings.rotation);
    const Eigen::Vector3f halfExtent =
        settings.size * 0.5f;

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
    // 用稳定槽位派生程序化噪声相位；同一 decalId 的纹理/参数身份不变。
    snapshot.randomSeed = static_cast<float>(
        (decalId >> 40) & 0xFFFFu) / 65535.0f;
    snapshot.roughness = settings.roughness;
    snapshot.metallic = settings.metallic;
    snapshot.ambientOcclusion = settings.ambientOcclusion;
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
    uint32_t maxTotalDecals)
{
    RemoveDynamicDecals(renderScene);

    uint32_t staticDecalCount = 0;
    for (const DecalSnapshot& decal : renderScene.decals)
    {
        if (decal.visible)
        {
            ++staticDecalCount;
        }
    }
    const uint32_t dynamicCapacity =
        staticDecalCount < maxTotalDecals
            ? maxTotalDecals - staticDecalCount
            : 0u;

    simulation.Simulate(renderScene, dynamicCapacity);
    for (const ParticleCpuEmitterRuntime::EmitterState* state :
         simulation.GetOrderedStates())
    {
        for (const ParticleCpuEmitterRuntime::Particle& particle :
             state->particles)
        {
            EmitDecalSnapshot(*state, particle, renderScene);
        }
    }
}

} // namespace VL
