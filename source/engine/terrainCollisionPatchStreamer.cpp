#include "engine/terrainCollisionPatchStreamer.h"

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace VL
{
namespace
{

std::string PatchKey(
    const std::string& placementId,
    const Eigen::Vector2f& centerLocalCm)
{
    return placementId + "|" +
        std::to_string(static_cast<long long>(std::llround(centerLocalCm.x()))) +
        "|" +
        std::to_string(static_cast<long long>(std::llround(centerLocalCm.y())));
}

} // namespace

TerrainCollisionPatchStreamer::TerrainCollisionPatchStreamer(
    JPH::PhysicsSystem& world,
    Settings settings)
    : world(world)
    , settings(std::move(settings))
{
    if (this->settings.patch.patchSizeCm <= 0.0f ||
        this->settings.patch.spacingCm <= 0.0f)
        throw std::invalid_argument(
            "Terrain collision patch streamer requires positive patch size "
            "and spacing");
}

TerrainCollisionPatchStreamer::~TerrainCollisionPatchStreamer()
{
    Reset();
}

void TerrainCollisionPatchStreamer::SetProxies(
    std::vector<std::shared_ptr<const TerrainCollisionProxy>> newProxies)
{
    Reset();
    proxies = std::move(newProxies);
}

std::shared_ptr<const TerrainCollisionProxy>
TerrainCollisionPatchStreamer::FindProxy(
    const Eigen::Vector3f& position,
    Eigen::Vector2f& outLocalXz) const
{
    for (const auto& proxy : proxies)
    {
        if (!proxy)
            continue;
        const Eigen::Vector2f local =
            proxy->WorldToLocalXz(position.x(), position.z());
        if (proxy->ContainsLocalXz(local.x(), local.y()))
        {
            outLocalXz = local;
            return proxy;
        }
    }
    return nullptr;
}

bool TerrainCollisionPatchStreamer::PatchFits(
    const TerrainCollisionProxy& proxy,
    const Eigen::Vector2f& centerLocalCm) const
{
    const float half = settings.patch.patchSizeCm * 0.5f;
    return std::abs(centerLocalCm.x()) + half <=
            proxy.sizeXCm * 0.5f + 1.0f &&
        std::abs(centerLocalCm.y()) + half <=
            proxy.sizeZCm * 0.5f + 1.0f;
}

bool TerrainCollisionPatchStreamer::PollPending(
    const std::string& desiredKey)
{
    if (!pending.valid())
        return false;
    if (pending.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready)
        return false;

    std::shared_ptr<const TerrainCollisionPatch> patch;
    try
    {
        patch = pending.get();
    }
    catch (const std::exception&)
    {
        // Patch 构建失败时丢弃，下一帧会按当前单元重试或回退宏 body。
        patch.reset();
    }
    const std::string key = pendingKey;
    pending = {};
    pendingKey.clear();
    if (!patch || key != desiredKey)
        return false;
    try
    {
        CreateActive(patch, key);
        return true;
    }
    catch (const std::exception&)
    {
        // 保留旧 active patch；下一帧按当前单元重试。
        return false;
    }
}

void TerrainCollisionPatchStreamer::CreateActive(
    const std::shared_ptr<const TerrainCollisionPatch>& patch,
    const std::string& key)
{
    TerrainCollisionBody body = CreateTerrainCollisionBody(
        world, patch->proxy, settings.body);
    activeBody = std::move(body);
    activePatch = patch;
    activeKey = key;
}

void TerrainCollisionPatchStreamer::ClearActive()
{
    activeBody.Reset();
    activePatch.reset();
    activeKey.clear();
}

bool TerrainCollisionPatchStreamer::Update(
    const Eigen::Vector3f& characterPositionCm)
{
    Eigen::Vector2f local;
    const std::shared_ptr<const TerrainCollisionProxy> proxy =
        FindProxy(characterPositionCm, local);

    std::string desiredKey;
    Eigen::Vector2f centerLocalCm = Eigen::Vector2f::Zero();
    if (proxy)
    {
        const float snap = settings.patch.patchSizeCm * 0.5f;
        centerLocalCm = Eigen::Vector2f(
            std::round(local.x() / snap) * snap,
            std::round(local.y() / snap) * snap);
        if (PatchFits(*proxy, centerLocalCm))
            desiredKey = PatchKey(proxy->placementId, centerLocalCm);
    }

    if (PollPending(desiredKey))
        return true;

    if (desiredKey.empty())
    {
        if (HasActivePatch())
        {
            ClearActive();
            return true;
        }
        return false;
    }
    if (desiredKey == activeKey || desiredKey == pendingKey)
        return false;
    if (pending.valid())
        return false;

    const TerrainCollisionPatchBuildSettings patchSettings = settings.patch;
    try
    {
        pending = std::async(
            std::launch::async,
            [proxy, centerLocalCm, patchSettings]()
            {
                return BuildTerrainCollisionPatch(
                    *proxy, centerLocalCm, patchSettings);
            });
        pendingKey = desiredKey;
    }
    catch (const std::exception&)
    {
        // 线程创建失败时退化为同步构建，保持功能可用。
        const std::shared_ptr<const TerrainCollisionPatch> patch =
            BuildTerrainCollisionPatch(*proxy, centerLocalCm, patchSettings);
        if (patch)
        {
            try
            {
                CreateActive(patch, desiredKey);
                return true;
            }
            catch (const std::exception&)
            {
            }
        }
    }
    return false;
}

void TerrainCollisionPatchStreamer::Reset()
{
    ClearActive();
    if (pending.valid())
    {
        try
        {
            pending.wait();
        }
        catch (const std::exception&)
        {
        }
    }
    pending = {};
    pendingKey.clear();
}

JPH::BodyID TerrainCollisionPatchStreamer::ActivePatchBodyId() const
{
    return JPH::BodyID(activeBody.GetBodyIdValue());
}

const std::string& TerrainCollisionPatchStreamer::ActivePlacementId() const
{
    static const std::string empty;
    return activePatch ? activePatch->proxy.placementId : empty;
}

uint32_t TerrainCollisionPatchStreamer::ActivePatchSampleCount() const
{
    return activePatch ? activePatch->proxy.sampleCount : 0;
}

} // namespace VL
