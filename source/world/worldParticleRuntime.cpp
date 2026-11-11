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
namespace
{
using namespace WorldParticleDetail;
} // namespace

bool World::ResetParticleSystemRuntimeOverrides(const std::string& name)
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    // 默认构造即恢复全发射器启用、倍率 1、白色 tint；visible 不属于运行时覆盖。
    systemIt->second.runtime = ParticleSystemRuntimeOverrides{};
    for (auto& slot : systemIt->second.emitterSlots)
        if (slot.active) slot.enabled = true;
    RefreshParticleRuntime(systemIt->second, particleBudget);
    return true;
}

bool World::SpawnParticleEmitter(const std::string& systemName,
    uint32_t sourceStableIndex, const std::string& requestedName,
    uint32_t& outStableIndex)
{
    const auto systemIt = particleSystems.find(systemName);
    if (systemIt == particleSystems.end()) return false;
    WorldParticleSystem& system = systemIt->second;
    if (sourceStableIndex >= system.emitterSlots.size() ||
        !system.emitterSlots[sourceStableIndex].active)
        return false;
    if (system.emitterSlots.size() >= kMaxParticleEmitters) return false;

    const uint32_t stableIndex =
        static_cast<uint32_t>(system.emitterSlots.size());
    std::string name = requestedName.empty()
        ? "runtime." + std::to_string(stableIndex)
        : requestedName;
    if (name.empty() || name.size() > 128 ||
        name.find('\0') != std::string::npos)
        return false;
    for (const auto& slot : system.emitterSlots)
        if (slot.asset.name == name) return false;

    WorldParticleSystem::EmitterSlot slot;
    slot.asset = system.emitterSlots[sourceStableIndex].asset;
    slot.asset.name = name;
    slot.active = true;
    slot.enabled = true;
    system.emitterSlots.push_back(std::move(slot));
    RefreshParticleRuntime(system, particleBudget);
    outStableIndex = stableIndex;
    return true;
}

bool World::DestroyParticleEmitter(const std::string& systemName,
    uint32_t stableIndex)
{
    const auto systemIt = particleSystems.find(systemName);
    if (systemIt == particleSystems.end()) return false;
    auto& slots = systemIt->second.emitterSlots;
    if (stableIndex >= slots.size() || !slots[stableIndex].active)
        return false;
    slots[stableIndex].active = false;
    slots[stableIndex].enabled = false;
    RefreshParticleRuntime(systemIt->second, particleBudget);
    return true;
}

bool World::RestartParticleEmitter(const std::string& systemName,
    uint32_t stableIndex)
{
    const auto systemIt = particleSystems.find(systemName);
    if (systemIt == particleSystems.end()) return false;
    auto& slots = systemIt->second.emitterSlots;
    if (stableIndex >= slots.size() || !slots[stableIndex].active)
        return false;
    if (slots[stableIndex].restartEpoch !=
        std::numeric_limits<uint32_t>::max())
        ++slots[stableIndex].restartEpoch;
    RefreshParticleRuntime(systemIt->second, particleBudget);
    return true;
}

bool World::FindParticleEmitterIndex(const std::string& systemName,
    const std::string& emitterName, uint32_t& outStableIndex) const
{
    const auto systemIt = particleSystems.find(systemName);
    if (systemIt == particleSystems.end()) return false;
    for (size_t index = 0;
         index < systemIt->second.emitterSlots.size();
         ++index)
    {
        if (systemIt->second.emitterSlots[index].asset.name != emitterName)
            continue;
        outStableIndex = static_cast<uint32_t>(index);
        return true;
    }
    return false;
}

bool World::SetParticleSystemBudget(const std::string& name,
    uint32_t maxTotalParticles, float emitterBudgetScale)
{
    if (!ValidParticleBudgetScale(emitterBudgetScale) ||
        maxTotalParticles > kMaxParticleTotalBudget)
        return false;
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.budget.maxTotalParticles = maxTotalParticles;
    systemIt->second.budget.emitterBudgetScale = emitterBudgetScale;
    RefreshParticleRuntime(systemIt->second, particleBudget);
    return true;
}

bool World::SetWorldParticleBudget(uint32_t maxTotalParticles,
    float emitterBudgetScale)
{
    if (!ValidParticleBudgetScale(emitterBudgetScale) ||
        maxTotalParticles > kMaxParticleTotalBudget)
        return false;
    particleBudget.maxTotalParticles = maxTotalParticles;
    particleBudget.emitterBudgetScale = emitterBudgetScale;
    particleBudget.effectiveCapacity = 0;
    for (auto& [name, system] : particleSystems)
    {
        (void)name;
        RefreshParticleRuntime(system, particleBudget);
    }
    return true;
}

ParticleSystemBudget World::GetEffectiveParticleBudget(
    const std::string& name) const
{
    const auto target = particleSystems.find(name);
    if (target == particleSystems.end()) return ParticleSystemBudget{};

    const uint32_t worldMax = particleBudget.maxTotalParticles;
    uint32_t cap = target->second.budget.maxTotalParticles;
    if (cap == 0 && worldMax != 0)
    {
        std::vector<std::string> names;
        uint32_t explicitTotal = 0;
        for (const auto& entry : particleSystems)
        {
            if (entry.second.budget.maxTotalParticles != 0)
                explicitTotal += entry.second.budget.maxTotalParticles;
            else
                names.push_back(entry.first);
        }
        std::sort(names.begin(), names.end());
        const uint32_t remaining = explicitTotal >= worldMax
            ? 0
            : worldMax - explicitTotal;
        if (!names.empty())
        {
            const uint32_t base = remaining /
                static_cast<uint32_t>(names.size());
            const uint32_t extra = remaining %
                static_cast<uint32_t>(names.size());
            for (size_t index = 0; index < names.size(); ++index)
                if (names[index] == name)
                {
                    cap = base + (index < extra ? 1u : 0u);
                    break;
                }
        }
    }

    ParticleSystemBudget result;
    result.maxTotalParticles = cap;
    result.emitterBudgetScale = std::clamp(
        target->second.budget.emitterBudgetScale *
            particleBudget.emitterBudgetScale,
        0.0f,
        16.0f);
    uint32_t consumed = 0;
    for (const auto& slot : target->second.emitterSlots)
    {
        if (!slot.active) continue;
        // beam 直接绘制、light 走独立 emit compute，不参与普通模拟池容量；
        // decal 与普通 GPU emitter 一样消耗预算。
        if (slot.asset.renderMode == "beam" ||
            slot.asset.renderMode == "light") continue;
        const uint32_t scaled =
            ScaledParticleCapacity(slot.asset.maxParticles,
                result.emitterBudgetScale);
        const uint32_t remaining = cap == 0
            ? std::numeric_limits<uint32_t>::max()
            : (consumed >= cap ? 0 : cap - consumed);
        const uint32_t budgeted = std::min(scaled, remaining);
        consumed += budgeted;
        result.effectiveCapacity += budgeted;
    }
    return result;
}

bool World::SetParticleSystemUserParameter(const std::string& name,
    const std::string& key, float value)
{
    if (key.empty() || key.size() > kMaxParticleParameterKeyLength ||
        key.find('\0') != std::string::npos ||
        !std::isfinite(value) ||
        std::abs(value) > kMaxParticleParameterAbsoluteValue)
        return false;
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    auto& parameters = systemIt->second.userParameters;
    if (!parameters.count(key) &&
        parameters.size() >= kMaxParticleUserParameters)
        return false;
    parameters[key] = value;
    return true;
}

bool World::ClearParticleSystemUserParameter(const std::string& name,
    const std::string& key)
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.userParameters.erase(key);
    return true;
}

bool World::SetParticleDataChannel(const std::string& name, float value)
{
    return SetParticleDataChannel(
        name, ParticleDataChannelValue::MakeFloat(value));
}

bool World::SetParticleDataChannel(const std::string& name,
    const ParticleDataChannelValue& value)
{
    std::string error;
    if (!ValidateParticleDataChannelName(name, error) ||
        !ValidateParticleDataChannelValue(value, error))
        return false;
    const auto existing = particleDataChannels.find(name);
    if (existing == particleDataChannels.end() &&
        particleDataChannels.size() >= kMaxParticleDataChannels)
        return false;
    particleDataChannels[name] = value;
    return true;
}

bool World::ClearParticleDataChannel(const std::string& name)
{
    std::string error;
    if (!ValidateParticleDataChannelName(name, error)) return false;
    return particleDataChannels.erase(name) != 0;
}

bool World::GetParticleDataChannel(const std::string& name,
    float& outValue) const
{
    std::string error;
    if (!ValidateParticleDataChannelName(name, error)) return false;
    const auto found = particleDataChannels.find(name);
    if (found == particleDataChannels.end()) return false;
    return found->second.TryGetFloat(outValue);
}

bool World::GetParticleDataChannelValue(const std::string& name,
    ParticleDataChannelValue& outValue) const
{
    std::string error;
    if (!ValidateParticleDataChannelName(name, error)) return false;
    const auto found = particleDataChannels.find(name);
    if (found == particleDataChannels.end()) return false;
    outValue = found->second;
    return true;
}

bool ExecuteParticleDataChannelCommand(World& world, bool clear,
    const std::string& name, float value, std::string& outError)
{
    return ExecuteParticleDataChannelCommand(
        world, clear, name,
        ParticleDataChannelValue::MakeFloat(value), outError);
}

bool ExecuteParticleDataChannelCommand(World& world, bool clear,
    const std::string& name, const ParticleDataChannelValue& value,
    std::string& outError)
{
    if (!ValidateParticleDataChannelName(name, outError)) return false;
    if (clear)
    {
        if (!world.ClearParticleDataChannel(name))
        {
            outError =
                "Particle data channel clear found no entry named: " + name;
            return false;
        }
        outError.clear();
        return true;
    }
    if (!ValidateParticleDataChannelValue(value, outError)) return false;
    if (!world.SetParticleDataChannel(name, value))
    {
        outError =
            "Particle data channel set was rejected (64-channel limit): " +
            name;
        return false;
    }
    outError.clear();
    return true;
}

bool World::AttachParticleSystem(const std::string& name,
    const std::string& targetKind, const std::string& targetName,
    const std::string& componentName,
    const Eigen::Vector3f& localPosition,
    const Eigen::Vector3f& localRotation,
    const Eigen::Vector3f& localScale)
{
    if (targetKind != "mesh" && targetKind != "component") return false;
    if (targetName.empty() || targetName.size() > 128 ||
        targetName.find('\0') != std::string::npos ||
        componentName.size() > 64 ||
        componentName.find('\0') != std::string::npos)
        return false;
    if (!localPosition.allFinite() || !localRotation.allFinite() ||
        !ValidParticleScaleAxis(localScale.x()) ||
        !ValidParticleScaleAxis(localScale.y()) ||
        !ValidParticleScaleAxis(localScale.z()))
        return false;
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    ParticleSystemAttachment attachment;
    attachment.targetKind = targetKind;
    attachment.targetName = targetName;
    attachment.componentName = componentName;
    attachment.localPosition = localPosition;
    attachment.localRotation = localRotation;
    attachment.localScale = localScale;
    attachment.resolved = false;
    systemIt->second.attachment = std::move(attachment);
    return true;
}

bool World::DetachParticleSystem(const std::string& name)
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.attachment = ParticleSystemAttachment{};
    return true;
}

bool World::SetParticleSystemRendererTelemetry(const std::string& name,
    uint32_t aliveParticles,
    const std::vector<uint32_t>& emitterAliveParticles,
    uint32_t lastFrameBudget)
{
    if (emitterAliveParticles.size() > kMaxParticleEmitters) return false;
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    ParticleSystemTelemetry& telemetry = systemIt->second.telemetry;
    telemetry.available = true;
    telemetry.aliveParticles = aliveParticles;
    telemetry.lastFrameBudget = lastFrameBudget;
    for (auto& emitter : telemetry.emitters)
    {
        emitter.aliveParticles = 0;
        emitter.aliveAvailable = false;
    }
    for (size_t index = 0;
         index < emitterAliveParticles.size() &&
         index < telemetry.emitters.size();
         ++index)
    {
        telemetry.emitters[index].aliveParticles =
            emitterAliveParticles[index];
        telemetry.emitters[index].aliveAvailable = true;
    }
    return true;
}

const ParticleSystemTelemetry* World::GetParticleSystemTelemetry(
    const std::string& name) const
{
    const auto systemIt = particleSystems.find(name);
    return systemIt == particleSystems.end()
        ? nullptr
        : &systemIt->second.telemetry;
}

nlohmann::json World::DescribeParticleSystemTelemetry(
    const std::string& name) const
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return nlohmann::json();
    const WorldParticleSystem& system = systemIt->second;
    const ParticleSystemBudget budget = GetEffectiveParticleBudget(name);
    bool attachmentResolved = false;
    (void)ResolveParticleSystemModel(system, attachmentResolved);
    nlohmann::json result{
        {"name", name},
        {"emitterCount", system.telemetry.emitterCount},
        {"activeEmitterCount", system.telemetry.activeEmitterCount},
        {"enabledEmitterCount", system.telemetry.enabledEmitterCount},
        {"visibleEmitterCount", system.telemetry.visibleEmitterCount},
        {"authoredCapacity", system.telemetry.authoredCapacity},
        {"budgetedCapacity", system.telemetry.budgetedCapacity},
        {"aliveAvailable", system.telemetry.available},
        {"aliveParticles", system.telemetry.aliveParticles},
        {"lastFrameBudget", system.telemetry.lastFrameBudget},
        {"budget", {
            {"maxTotalParticles", budget.maxTotalParticles},
            {"emitterBudgetScale", budget.emitterBudgetScale},
            {"effectiveCapacity", budget.effectiveCapacity}}},
        {"attachment", {
            {"kind", system.attachment.targetKind},
            {"target", system.attachment.targetName},
            {"component", system.attachment.componentName},
            {"resolved", attachmentResolved}}}};
    result["emitters"] = nlohmann::json::array();
    for (const auto& emitter : system.telemetry.emitters)
        result["emitters"].push_back({
            {"index", emitter.stableIndex},
            {"name", emitter.name},
            {"active", emitter.active},
            {"enabled", emitter.enabled},
            {"restartEpoch", emitter.restartEpoch},
            {"maxParticles", emitter.maxParticles},
            {"budgetedMaxParticles", emitter.budgetedMaxParticles},
            {"aliveAvailable", emitter.aliveAvailable},
            {"aliveParticles", emitter.aliveParticles}});
    return result;
}

bool World::QueueParticleRuntimeEvent(const WorldParticleRuntimeEvent& event)
{
    if (event.systemName.empty() || event.systemName.size() > 128 ||
        event.systemName.find('\0') != std::string::npos ||
        event.emitterName.size() > 128 ||
        event.emitterName.find('\0') != std::string::npos ||
        (event.kind != "spawn" && event.kind != "death" &&
            event.kind != "collision") ||
        !std::isfinite(event.time) || !event.position.allFinite() ||
        !event.normal.allFinite())
        return false;
    if (particleSystems.find(event.systemName) == particleSystems.end())
        return false;
    if (particleRuntimeEvents.size() >= kMaxParticleRuntimeEvents)
        return false;
    particleRuntimeEvents.push_back(event);
    return true;
}

std::vector<WorldParticleRuntimeEvent> World::TakeParticleRuntimeEvents()
{
    auto result = std::move(particleRuntimeEvents);
    particleRuntimeEvents.clear();
    return result;
}

bool World::SetParticleEventCaptureInterest(
    const std::string& owner, bool enabled)
{
    if (owner.empty() || owner.size() > 128 ||
        owner.find('\0') != std::string::npos)
        return false;
    if (enabled)
        particleEventCaptureOwners.insert(owner);
    else
        particleEventCaptureOwners.erase(owner);
    return true;
}

Eigen::Matrix4f World::ResolveParticleSystemModel(
    const WorldParticleSystem& system, bool& resolved) const
{
    resolved = true;
    if (!system.attachment.IsEnabled()) return system.model;

    SceneNode local;
    local.SetTransform(
        system.attachment.localPosition,
        system.attachment.localRotation,
        system.attachment.localScale);
    Eigen::Matrix4f target = Eigen::Matrix4f::Identity();
    bool found = false;
    if (system.attachment.targetKind == "mesh")
    {
        std::vector<std::string> matchingKeys;
        for (const auto& [key, object] : meshObjects)
            if (MeshSceneObjectIdentity(object, key) ==
                    system.attachment.targetName ||
                key == system.attachment.targetName)
                matchingKeys.push_back(key);
        if (!matchingKeys.empty())
        {
            std::sort(matchingKeys.begin(), matchingKeys.end());
            target = meshObjects.at(matchingKeys.front()).model;
            found = true;
        }
    }
    else if (system.attachment.targetKind == "component")
    {
        std::string identity = system.attachment.targetName;
        if (identity.rfind("__blueprint/", 0) != 0)
            identity = "__blueprint/" + identity;
        if (!system.attachment.componentName.empty())
            identity += "/" + system.attachment.componentName;
        const auto matrix = scriptComponentMatrices.find(identity);
        if (matrix != scriptComponentMatrices.end())
        {
            target = matrix->second;
            found = true;
        }
        else if (!system.attachment.componentName.empty())
        {
            // 组件矩阵表尚未同步时，回退到冻结的运行时状态，避免出生帧
            // 附着目标短暂掉回作者原点。
            for (const auto& actor : scriptBlueprintStates)
            {
                if (actor.value("id", std::string()) !=
                    system.attachment.targetName)
                    continue;
                const auto components =
                    actor.value("components", nlohmann::json::array());
                for (size_t index = 0; index < components.size(); ++index)
                {
                    if (components[index].at("type") != "mesh" ||
                        ScriptComponentId(components[index]) !=
                            system.attachment.componentName)
                        continue;
                    target = ScriptComponentMatrix(actor, index);
                    found = true;
                    break;
                }
                if (found) break;
            }
        }
    }
    if (!found)
    {
        resolved = false;
        return system.model;
    }
    return target * local.GetModelMatrix();
}

} // namespace VL
