#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <set>
#include <string>
#include <utility>

#include "commonFunction.h"
#include "sceneNode.h"
#include "world/meshSceneObjectIdentity.h"
#include "script/scriptScene.h"
#include "world/scriptTemplateExtension.h"
#include "animation/skeletalPlayback.h"
#include "render/resource/renderableObjectKey.h"
#include "world/worldParticleInternal.h"

namespace VL
{
namespace WorldParticleDetail
{

bool ValidParticleScaleAxis(float value)
{
    constexpr float kMinAbsoluteScale = 1.0e-6f;
    return std::isfinite(value) && std::abs(value) >= kMinAbsoluteScale;
}


bool ValidParticleBudgetScale(float value)
{
    return std::isfinite(value) && value >= 0.0f && value <= 16.0f;
}

uint32_t ScaledParticleCapacity(uint32_t capacity, float scale)
{
    const double scaled = static_cast<double>(capacity) *
        static_cast<double>(scale);
    if (scaled <= 0.0) return 0;
    if (scaled >= static_cast<double>(
            std::numeric_limits<uint32_t>::max()))
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(scaled);
}

uint32_t EffectiveParticleMaxTotal(
    const WorldParticleSystem& system,
    const ParticleSystemBudget& worldBudget)
{
    return system.budget.maxTotalParticles != 0
        ? system.budget.maxTotalParticles
        : worldBudget.maxTotalParticles;
}

float EffectiveParticleBudgetScale(
    const WorldParticleSystem& system,
    const ParticleSystemBudget& worldBudget)
{
    return std::clamp(
        system.budget.emitterBudgetScale *
            worldBudget.emitterBudgetScale,
        0.0f,
        16.0f);
}

// 槽位身份与运行时资产是两份视图：槽位保留 tombstone，asset.emitters 只放
// active 项。渲染器仍按 emitter.name 建池，因此 destroy 能退休资源，而
// stableIndex 不会因删除中间项而漂移。enabledEmitterMask 始终按紧凑位置编码。
void RefreshParticleRuntime(
    WorldParticleSystem& system,
    const ParticleSystemBudget& worldBudget)
{
    system.asset.emitters.clear();
    uint32_t mask = 0;
    for (size_t stableIndex = 0;
         stableIndex < system.emitterSlots.size();
         ++stableIndex)
    {
        const auto& slot = system.emitterSlots[stableIndex];
        if (!slot.active) continue;
        const uint32_t position =
            static_cast<uint32_t>(system.asset.emitters.size());
        system.asset.emitters.push_back(slot.asset);
        if (slot.enabled && position < kMaxParticleEmitters)
            mask |= (1u << position);
    }
    if (!system.emitterSlots.empty())
        system.runtime.enabledEmitterMask = mask;
    system.nextEmitterSlot =
        static_cast<uint32_t>(system.emitterSlots.size());

    const ParticleSystemTelemetry previous = system.telemetry;
    system.telemetry = ParticleSystemTelemetry{};
    system.telemetry.available = previous.available;
    system.telemetry.aliveParticles = previous.aliveParticles;
    system.telemetry.lastFrameBudget = previous.lastFrameBudget;

    const uint32_t maxTotal = EffectiveParticleMaxTotal(system, worldBudget);
    const float scale = EffectiveParticleBudgetScale(system, worldBudget);
    uint32_t consumed = 0;
    for (size_t stableIndex = 0;
         stableIndex < system.emitterSlots.size();
         ++stableIndex)
    {
        const auto& slot = system.emitterSlots[stableIndex];
        ParticleEmitterTelemetry emitter;
        emitter.stableIndex = static_cast<uint32_t>(stableIndex);
        emitter.name = slot.asset.name;
        emitter.active = slot.active;
        emitter.enabled = slot.active && slot.enabled;
        emitter.restartEpoch = slot.restartEpoch;
        emitter.maxParticles = slot.active ? slot.asset.maxParticles : 0;
        if (stableIndex < previous.emitters.size() &&
            previous.emitters[stableIndex].stableIndex == stableIndex)
        {
            emitter.aliveParticles =
                previous.emitters[stableIndex].aliveParticles;
            emitter.aliveAvailable =
                previous.emitters[stableIndex].aliveAvailable;
        }
        system.telemetry.emitters.push_back(std::move(emitter));
        if (!slot.active) continue;
        ++system.telemetry.activeEmitterCount;
        if (slot.enabled) ++system.telemetry.enabledEmitterCount;
        // beam 直接绘制，light 由独立 emit compute 输出场景点光；两者
        // 不计入 authored/budgetedCapacity。decal 复用普通 GPU 模拟池，
        // 必须与 billboard/ribbon 一样参与总预算。
        if (slot.asset.renderMode == "beam" ||
            slot.asset.renderMode == "light")
        {
            system.telemetry.emitters.back().budgetedMaxParticles =
                slot.asset.renderMode == "beam"
                    ? slot.asset.maxParticles
                    : 0u;
            continue;
        }
        system.telemetry.authoredCapacity += slot.asset.maxParticles;
        const uint32_t scaled =
            ScaledParticleCapacity(slot.asset.maxParticles, scale);
        const uint32_t remaining = maxTotal == 0
            ? std::numeric_limits<uint32_t>::max()
            : (consumed >= maxTotal ? 0 : maxTotal - consumed);
        const uint32_t budgeted = std::min(scaled, remaining);
        consumed += budgeted;
        system.telemetry.emitters.back().budgetedMaxParticles = budgeted;
        system.telemetry.budgetedCapacity += budgeted;
    }
    system.telemetry.emitterCount =
        static_cast<uint32_t>(system.emitterSlots.size());
    system.telemetry.visibleEmitterCount = system.visible
        ? system.telemetry.enabledEmitterCount
        : 0u;
}

void InitializeParticleEmitterSlots(WorldParticleSystem& system)
{
    system.emitterSlots.clear();
    for (const auto& emitter : system.asset.emitters)
    {
        WorldParticleSystem::EmitterSlot slot;
        slot.asset = emitter;
        system.emitterSlots.push_back(std::move(slot));
    }
    system.nextEmitterSlot =
        static_cast<uint32_t>(system.emitterSlots.size());
}


} // namespace WorldParticleDetail
namespace
{
using namespace WorldParticleDetail;
} // namespace

void World::AddParticleSystem(
    std::string name,
    WorldParticleSystem system)
{
    InitializeParticleEmitterSlots(system);
    RefreshParticleRuntime(system, particleBudget);
    particleSystems[std::move(name)] = std::move(system);
}

bool World::RemoveParticleSystem(const std::string& name)
{
    return particleSystems.erase(name) > 0;
}

const WorldParticleSystem* World::FindParticleSystem(
    const std::string& name) const
{
    const auto systemIt = particleSystems.find(name);
    return systemIt != particleSystems.end() ? &systemIt->second : nullptr;
}

size_t World::UpdateParticleSystemAsset(
    const std::string& assetPath,
    ParticleSystemAsset asset)
{
    size_t replaced = 0;
    for (auto& [name, particleSystem] : particleSystems)
    {
        if (particleSystem.assetPath != assetPath) continue;
        particleSystem.asset = asset;
        InitializeParticleEmitterSlots(particleSystem);
        // 资产替换会重建发射器槽位，但实例级预算/附着/用户参数仍属于
        // 当前 World 实例，不应被编辑器预览资产刷新静默清掉。
        RefreshParticleRuntime(particleSystem, particleBudget);
        ++replaced;
    }
    return replaced;
}

bool World::SetParticleSystemVisible(const std::string& name, bool visible)
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.visible = visible;
    RefreshParticleRuntime(systemIt->second, particleBudget);
    return true;
}

bool World::SetParticleSystemTransform(const std::string& name,
    const Eigen::Vector3f& position, const Eigen::Vector3f& rotation,
    const Eigen::Vector3f& scale)
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;

    if (!position.allFinite() || !rotation.allFinite() ||
        !ValidParticleScaleAxis(scale.x()) ||
        !ValidParticleScaleAxis(scale.y()) ||
        !ValidParticleScaleAxis(scale.z()))
    {
        return false;
    }

    Eigen::Matrix4f scaleMatrix = Eigen::Matrix4f::Identity();
    scaleMatrix(0, 0) = scale.x();
    scaleMatrix(1, 1) = scale.y();
    scaleMatrix(2, 2) = scale.z();
    Eigen::Matrix4f translation = Eigen::Matrix4f::Identity();
    translation.block<3, 1>(0, 3) = position;
    // 与 WorldBuilder::BuildParticleSystemModel 保持同一 bake 顺序。
    systemIt->second.model = translation *
        CommonFunction::RotationToMatrix(rotation) *
        scaleMatrix;
    systemIt->second.scenePosition = position;
    systemIt->second.sceneRotation = rotation;
    systemIt->second.sceneScale = scale;
    // 显式移动 actor 视为脱离附着，避免下一帧又被目标矩阵拉回去。
    systemIt->second.attachment = ParticleSystemAttachment{};
    return true;
}

bool World::SetParticleSystemSpawnRateScale(const std::string& name, float scale)
{
    std::string error;
    if (!ValidateParticleSpawnRateScale(scale, error))
    {
        return false;
    }
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.runtime.spawnRateScale = scale;
    return true;
}

bool World::SetParticleSystemColorTint(const std::string& name,
    const Eigen::Vector4f& colorTint)
{
    std::string error;
    if (!ValidateParticleColorTint(colorTint, error))
    {
        return false;
    }
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.runtime.colorTint = colorTint;
    return true;
}

bool World::SetParticleSystemEmitterEnabled(const std::string& name,
    uint32_t emitterIndex, bool enabled)
{
    std::string error;
    if (!ValidateParticleEmitterIndex(emitterIndex, error))
    {
        return false;
    }
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;

    if (!systemIt->second.emitterSlots.empty())
    {
        if (emitterIndex >= systemIt->second.emitterSlots.size())
            return false;
        auto& slot = systemIt->second.emitterSlots[emitterIndex];
        if (!slot.active) return false;
        slot.enabled = enabled;
        RefreshParticleRuntime(systemIt->second, particleBudget);
        return true;
    }

    // 只改对应低位；高位保留隐式/未来发射器的默认启用状态。
    const uint32_t emitterBit = 1u << emitterIndex;
    if (enabled)
    {
        systemIt->second.runtime.enabledEmitterMask |= emitterBit;
    }
    else
    {
        systemIt->second.runtime.enabledEmitterMask &= ~emitterBit;
    }
    return true;
}

} // namespace VL
