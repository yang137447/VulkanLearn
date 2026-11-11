#include "skeletalPlaybackInternal.h"

#include <algorithm>
#include <cmath>

namespace VL::Animation
{
void SkeletalPlayback::ClearSyncSourceClock()
{
    syncSourceValid = false;
    syncSourceSelected = 0;
    syncSourceTime = 0;
    syncSourceRangeStart = 0;
    syncSourceRangeEnd = 0;
    syncSourceRate = 1;
    syncSourceLooping = true;
}

void SkeletalPlayback::AdvanceSyncSourceClock(float deltaSeconds)
{
    if (!syncSourceValid) return;
    const float length = syncSourceRangeEnd - syncSourceRangeStart;
    if (!std::isfinite(length) || length <= 0) return;
    double next = double(syncSourceTime) + double(deltaSeconds) * syncSourceRate;
    if (syncSourceLooping)
    {
        next = double(syncSourceRangeStart) +
            std::fmod(next - double(syncSourceRangeStart), double(length));
        if (next < syncSourceRangeStart) next += length;
    }
    else next = std::min(next, double(syncSourceRangeEnd));
    syncSourceTime = static_cast<float>(next);
}

float SkeletalPlayback::SyncPhase() const
{
    const float duration = ClipDuration();
    if (duration <= 0 || !std::isfinite(time)) return 0;
    float phase = time / duration;
    if (looping) phase -= std::floor(phase);
    return std::clamp(phase, 0.0f, 1.0f);
}

void SkeletalPlayback::RefreshSyncMarker()
{
    syncMarkerName.clear();
    syncMarkerAlpha = 0;
    const auto& sequence = asset->AnimationAsset().clips.at(selected).sequence;
    if (sequence.syncMarkers.empty() || !std::isfinite(time)) return;
    const SyncMarker* previous = nullptr;
    const SyncMarker* next = nullptr;
    for (const SyncMarker& marker : sequence.syncMarkers)
    {
        if (marker.time <= time) previous = &marker;
        else
        {
            next = &marker;
            break;
        }
    }
    if (previous == nullptr) return;
    syncMarkerName = previous->name;
    if (next != nullptr && next->time > previous->time)
        syncMarkerAlpha = std::clamp(
            (time - previous->time) / (next->time - previous->time), 0.0f, 1.0f);
}

const PlayerBinding* SkeletalPlayback::SyncBinding() const
{
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty()) return nullptr;
    const SequenceData& sequence = animation.clips.at(selected).sequence;
    const PlayerBinding* fallback = nullptr;
    for (const PlayerBinding& player : sequence.players)
    {
        if (player.syncGroup.empty() || player.syncMethod == SyncMethod::DoNotSync) continue;
        if (fallback == nullptr) fallback = &player;
        if (!activeSyncGroup.empty() && player.syncGroup == activeSyncGroup) return &player;
    }
    if (fallback != nullptr) return fallback;
    return sequence.players.empty() ? nullptr : &sequence.players.front();
}

const PlayerBinding* SkeletalPlayback::SyncSourceBinding() const
{
    if (!syncSourceValid) return nullptr;
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty() || syncSourceSelected >= animation.clips.size()) return nullptr;
    const SequenceData& sequence = animation.clips.at(syncSourceSelected).sequence;
    const PlayerBinding* fallback = nullptr;
    for (const PlayerBinding& player : sequence.players)
    {
        if (player.syncGroup.empty() || player.syncMethod == SyncMethod::DoNotSync) continue;
        if (fallback == nullptr) fallback = &player;
        if (!activeSyncGroup.empty() && player.syncGroup == activeSyncGroup) return &player;
    }
    if (fallback != nullptr) return fallback;
    return sequence.players.empty() ? nullptr : &sequence.players.front();
}

const std::vector<SyncMarker>* SkeletalPlayback::SyncSourceMarkers() const
{
    if (!syncSourceValid) return nullptr;
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty() || syncSourceSelected >= animation.clips.size()) return nullptr;
    return &animation.clips.at(syncSourceSelected).sequence.syncMarkers;
}

SyncGroupNode SkeletalPlayback::BuildSyncGroupNode(std::string_view identity,
    std::string_view ownerKey, std::string_view localKey, bool source)
{
    SyncGroupNode node;
    node.playback = this;
    node.identity = identity;
    node.ownerKey = ownerKey;
    node.localKey = localKey;
    node.canApply = !source;
    node.active = playing;
    node.transitioning = IsBlending();
    const float alpha = BlendAlpha();

    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty()) return node;
    if (source)
    {
        if (!syncSourceValid || syncSourceSelected >= animation.clips.size())
        {
            node.method = SyncMethod::DoNotSync;
            node.active = false;
            return node;
        }
        // 影子时钟只代表交叉淡入的旧片段：混合结束后它已不在画面上；非循环片段
        // 播完后相位 clamp 在 rangeEnd，继续当 leader 会把目标循环钉在末帧
        // （表现就是“卡脚”）。两种情况都不能参与选举。
        node.active = playing && IsBlending() &&
            (syncSourceLooping || syncSourceTime < syncSourceRangeEnd - 1e-4f);
        const SequenceData& sequence = animation.clips.at(syncSourceSelected).sequence;
        node.time = syncSourceTime;
        node.rangeStart = syncSourceRangeStart;
        node.rangeEnd = syncSourceRangeEnd;
        node.rate = syncSourceRate;
        node.looping = syncSourceLooping;
        node.markers = &sequence.syncMarkers;
        node.weight = std::clamp(1.0f - alpha, 0.0f, 1.0f);
        if (const PlayerBinding* binding = SyncSourceBinding())
        {
            node.group = binding->syncGroup;
            node.role = binding->syncRole;
            node.method = binding->syncMethod;
            node.scope = binding->syncGroupScope;
        }
        return node;
    }

    const SequenceData& sequence = animation.clips.at(selected).sequence;
    node.time = time;
    node.rangeStart = PlaybackRangeStart();
    node.rangeEnd = PlaybackRangeEnd();
    node.rate = rate;
    node.looping = looping;
    node.markers = &sequence.syncMarkers;
    node.weight = IsBlending() ? std::clamp(alpha, 0.0f, 1.0f) : 1.0f;
    if (const PlayerBinding* binding = SyncBinding())
    {
        node.group = binding->syncGroup;
        node.role = binding->syncRole;
        node.method = binding->syncMethod;
        node.scope = binding->syncGroupScope;
    }
    return node;
}

void SkeletalPlayback::ApplySyncGroupResult(const SyncGroupNodeResult& result,
    std::string_view leaderIdentity)
{
    syncDecision = result;
    syncDecisionValid = true;
    syncLeader.assign(leaderIdentity);
    syncPendingValid = true;
    syncPendingHasTime = result.follower && result.hasFollow;
    if (syncPendingHasTime)
    {
        syncPendingTime = result.followTime;
        syncPendingRate = result.followRate;
    }
    else
    {
        // leader/无 leader/DoNotSync 都回到作者请求速率，避免上次 follower 的
        // 时长比速率泄漏到下一段片段。
        syncPendingTime = time;
        syncPendingRate = requestedRate;
    }
}

bool SkeletalPlayback::SyncActive() const
{
    return playing && syncDecisionValid && !syncDecision.doNotSync &&
        (syncDecision.leader || syncDecision.follower);
}
}
