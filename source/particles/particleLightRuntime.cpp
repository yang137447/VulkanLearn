#include "particles/particleLightRuntime.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "render/frontend/renderScene.h"

namespace VL
{
namespace
{

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

} // namespace

void ParticleLightRuntime::Reset()
{
    simulation.Reset();
    dynamicLightIds.clear();
}

void ParticleLightRuntime::Synchronize(
    const RenderScene& renderScene,
    const std::unordered_map<std::string, ParticleSystemAsset>&
        resolvedSystemAssets)
{
    // 保留 dynamicLightIds：旧灯光仍在 RenderScene::lights 里，必须让
    // 下一次 Update 先移除它们；共享模拟按新代际重建状态。
    simulation.Synchronize(
        renderScene, resolvedSystemAssets, "light");
}

void ParticleLightRuntime::EmitLightSnapshot(
    const ParticleCpuEmitterRuntime::EmitterState& state,
    const ParticleCpuEmitterRuntime::Particle& particle,
    RenderScene& renderScene)
{
    const ParticleEmitterAsset& emitter = state.emitter;
    const float lifeT = std::clamp(
        particle.lifetime > 0.0f
            ? particle.age / particle.lifetime
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
    // 保持原有 lightId 语义：burst 重触发代际 + 发射序号共同组成动态身份。
    snapshot.lightId = HashString(
        state.key + "#" +
            std::to_string(state.burstGeneration) + ":" +
            std::to_string(particle.serial),
        0x9E3779B97F4A7C15ull);
    snapshot.type = LightSnapshotType::Point;
    snapshot.color = color;
    snapshot.intensity =
        emitter.light.intensity * (1.0f - lifeT) * alpha;
    snapshot.radius = emitter.light.radius;
    snapshot.position = emitter.localSpace
        ? (state.model * Eigen::Vector4f(
            particle.position.x(), particle.position.y(),
            particle.position.z(), 1.0f)).head<3>()
        : particle.position;
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

    simulation.Simulate(renderScene, kMaxLiveLights);
    for (const ParticleCpuEmitterRuntime::EmitterState* state :
         simulation.GetOrderedStates())
    {
        for (const ParticleCpuEmitterRuntime::Particle& particle :
             state->particles)
        {
            EmitLightSnapshot(*state, particle, renderScene);
        }
    }
}

uint32_t ParticleLightRuntime::GetLiveLightCount() const
{
    return simulation.GetLiveParticleCount();
}

} // namespace VL
