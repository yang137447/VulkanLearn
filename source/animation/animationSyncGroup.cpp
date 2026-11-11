#include "animationSyncGroup.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace VL::Animation
{
namespace
{
constexpr float RangeEpsilon = 1e-4f;
}

const char* SyncGroupDiagnosticName(SyncGroupDiagnostic diagnostic)
{
    switch (diagnostic)
    {
    case SyncGroupDiagnostic::None: return "none";
    case SyncGroupDiagnostic::GraphNotExecuted: return "sync-graph-not-executed";
    case SyncGroupDiagnostic::ExclusiveLeaderRejected: return "sync-exclusive-leader-rejected";
    case SyncGroupDiagnostic::NoLeader: return "sync-no-leader";
    case SyncGroupDiagnostic::LeaderMarkerMissing: return "sync-leader-marker-missing";
    case SyncGroupDiagnostic::FollowerMarkerMissing: return "sync-follower-marker-missing";
    case SyncGroupDiagnostic::TransitionFollowerInactive:
        return "sync-transition-follower-inactive";
    case SyncGroupDiagnostic::InvalidRange: return "sync-invalid-range";
    }
    return "sync-unknown";
}

void AnimationSyncGroupRegistry::BeginTick()
{
    nodes.clear();
    results.clear();
    groups.clear();
    nodeGroup.clear();
    members.clear();
    memberOffsets.clear();
    memberCursor.clear();
}

SyncGroupNode& AnimationSyncGroupRegistry::AddNode(SyncGroupNode node)
{
    nodes.push_back(std::move(node));
    return nodes.back();
}

float AnimationSyncGroupRegistry::RangeLength(const SyncGroupNode& node)
{
    const float length = node.rangeEnd - node.rangeStart;
    return std::isfinite(length) && length > 0 ? length : 0.0f;
}

float AnimationSyncGroupRegistry::NodePhase(const SyncGroupNode& node)
{
    const float length = RangeLength(node);
    if (length <= 0 || !std::isfinite(node.time)) return 0;
    float phase = (node.time - node.rangeStart) / length;
    if (node.looping) phase -= std::floor(phase);
    return std::clamp(phase, 0.0f, 1.0f);
}

float AnimationSyncGroupRegistry::FollowerRate(float leaderRate, float leaderDuration,
    float followerDuration, bool& clamped)
{
    clamped = false;
    if (!std::isfinite(leaderRate) || leaderRate <= 0 ||
        !std::isfinite(leaderDuration) || leaderDuration <= 0 ||
        !std::isfinite(followerDuration) || followerDuration <= 0)
    {
        return 1.0f;
    }
    const float rate = leaderRate * followerDuration / leaderDuration;
    if (!std::isfinite(rate) || rate <= 0)
    {
        clamped = true;
        return 1.0f;
    }
    const float limited = std::clamp(rate, RateMin, RateMax);
    clamped = std::abs(limited - rate) > 1e-6f;
    return limited;
}

std::string_view AnimationSyncGroupRegistry::CurrentMarkerName(
    const std::vector<SyncMarker>& markers, float time, float rangeStart, float rangeEnd,
    bool looping)
{
    if (markers.empty() || !std::isfinite(time)) return {};
    const float length = std::isfinite(rangeEnd - rangeStart) && rangeEnd > rangeStart ?
        rangeEnd - rangeStart : 0.0f;
    const SyncMarker* previous = nullptr;
    for (const SyncMarker& marker : markers)
    {
        if (marker.time <= time) previous = &marker;
        else break;
    }
    // time 落在首 marker 之前时仍属于“上一轮最后 marker”的区间。
    if (previous == nullptr && looping && length > 0) previous = &markers.back();
    return previous == nullptr ? std::string_view() : std::string_view(previous->name);
}

bool AnimationSyncGroupRegistry::MarkerInterval(const std::vector<SyncMarker>& markers,
    std::string_view name, float time, float rangeStart, float rangeEnd, bool looping,
    float& previousTime, float& nextTime)
{
    if (name.empty() || markers.empty() || !std::isfinite(time)) return false;
    const float length = std::isfinite(rangeEnd - rangeStart) && rangeEnd > rangeStart ?
        rangeEnd - rangeStart : 0.0f;

    const SyncMarker* previous = nullptr;
    const SyncMarker* next = nullptr;
    for (const SyncMarker& marker : markers)
    {
        if (marker.name != name) continue;
        if (marker.time <= time)
        {
            previous = &marker;
            continue;
        }
        if (next == nullptr) next = &marker;
    }

    if (previous == nullptr)
    {
        // 还没到本轮首个同名 marker：循环时把上一轮最后一个同名 marker 平移一轮。
        const SyncMarker* lastNamed = nullptr;
        const SyncMarker* firstNamed = nullptr;
        for (const SyncMarker& marker : markers)
        {
            if (marker.name != name) continue;
            if (firstNamed == nullptr) firstNamed = &marker;
            lastNamed = &marker;
        }
        if (firstNamed == nullptr) return false;
        if (!looping)
        {
            previousTime = rangeStart;
            nextTime = firstNamed->time;
            return nextTime > previousTime;
        }
        if (!(length > 0)) return false;
        previousTime = lastNamed->time - length;
        nextTime = firstNamed->time;
        return nextTime > previousTime;
    }

    previousTime = previous->time;
    if (next != nullptr)
    {
        nextTime = next->time;
        return nextTime > previousTime;
    }
    if (!looping)
    {
        nextTime = rangeEnd;
        return nextTime > previousTime;
    }
    if (!(length > 0)) return false;
    const SyncMarker* firstNamed = nullptr;
    for (const SyncMarker& marker : markers)
        if (marker.name == name)
        {
            firstNamed = &marker;
            break;
        }
    if (firstNamed == nullptr) return false;
    nextTime = firstNamed->time + length;
    return nextTime > previousTime;
}

AnimationSyncGroupRegistry::Election AnimationSyncGroupRegistry::Elect(
    const std::vector<SyncGroupNode>& sourceNodes, const std::vector<int>& groupMembers,
    size_t begin, size_t end)
{
    Election election;
    // 1) ExclusiveAlwaysLeader：存在多个时只有注册顺序最早的一个能领导，其余退化为
    //    DoNotSync（UE 语义），不能让它们悄悄变成 follower 去追另一个独占节点。
    for (size_t member = begin; member < end; ++member)
    {
        const int index = groupMembers[member];
        const SyncGroupNode& node = sourceNodes[static_cast<size_t>(index)];
        if (node.role != SyncRole::ExclusiveAlwaysLeader || !node.active) continue;
        if (election.leader < 0) election.leader = index;
    }
    if (election.leader >= 0) return election;

    // 2) AlwaysLeader。
    for (size_t member = begin; member < end; ++member)
    {
        const int index = groupMembers[member];
        const SyncGroupNode& node = sourceNodes[static_cast<size_t>(index)];
        if (node.role == SyncRole::AlwaysLeader && node.active)
        {
            election.leader = index;
            return election;
        }
    }

    // 3) TransitionLeader 只在混合窗口内领导；窗口结束后它转为 follower。
    for (size_t member = begin; member < end; ++member)
    {
        const int index = groupMembers[member];
        const SyncGroupNode& node = sourceNodes[static_cast<size_t>(index)];
        if (node.role == SyncRole::TransitionLeader && node.active && node.transitioning)
        {
            election.leader = index;
            return election;
        }
    }

    // 4) CanBeLeader 按权重最大者；同权重按注册顺序保持确定性。
    float bestWeight = -std::numeric_limits<float>::infinity();
    for (size_t member = begin; member < end; ++member)
    {
        const int index = groupMembers[member];
        const SyncGroupNode& node = sourceNodes[static_cast<size_t>(index)];
        if (node.role != SyncRole::CanBeLeader || !node.active || !(node.weight > 0)) continue;
        if (election.leader < 0 || node.weight > bestWeight)
        {
            election.leader = index;
            bestWeight = node.weight;
        }
    }
    return election;
}

SyncGroupDiagnostic AnimationSyncGroupRegistry::ResolveFollower(const SyncGroupNode& follower,
    const SyncGroupNode& leader, SyncGroupNodeResult& result)
{
    const float followerLength = RangeLength(follower);
    const float leaderLength = RangeLength(leader);
    if (followerLength <= 0 || leaderLength <= 0)
    {
        result.hasFollow = false;
        return SyncGroupDiagnostic::InvalidRange;
    }

    bool clamped = false;
    result.followRate = FollowerRate(leader.rate, leaderLength, followerLength, clamped);
    result.rateClamped = clamped;

    if (follower.method == SyncMethod::MarkerSync)
    {
        if (leader.markers == nullptr || leader.markers->empty())
        {
            result.effectiveMethod = SyncMethod::SyncGroup;
            result.followTime = follower.rangeStart + NodePhase(leader) * followerLength;
            result.hasFollow = true;
            return SyncGroupDiagnostic::LeaderMarkerMissing;
        }
        const std::string_view markerName = CurrentMarkerName(*leader.markers, leader.time,
            leader.rangeStart, leader.rangeEnd, leader.looping);
        if (markerName.empty())
        {
            // 缺 leader marker 时回退 SyncGroup；不能静默停在旧相位。
            result.effectiveMethod = SyncMethod::SyncGroup;
            result.followTime = follower.rangeStart + NodePhase(leader) * followerLength;
            result.hasFollow = true;
            return SyncGroupDiagnostic::LeaderMarkerMissing;
        }

        float leaderPrevious = 0;
        float leaderNext = 0;
        if (!MarkerInterval(*leader.markers, markerName, leader.time, leader.rangeStart,
                leader.rangeEnd, leader.looping, leaderPrevious, leaderNext))
        {
            result.effectiveMethod = SyncMethod::SyncGroup;
            result.followTime = follower.rangeStart + NodePhase(leader) * followerLength;
            result.hasFollow = true;
            return SyncGroupDiagnostic::LeaderMarkerMissing;
        }

        float followerPrevious = 0;
        float followerNext = 0;
        if (follower.markers == nullptr ||
            !MarkerInterval(*follower.markers, markerName, follower.time, follower.rangeStart,
                follower.rangeEnd, follower.looping, followerPrevious, followerNext))
        {
            // follower 没有同名 marker：回退 SyncGroup 并明确诊断，避免每帧漂移。
            result.effectiveMethod = SyncMethod::SyncGroup;
            result.followTime = follower.rangeStart + NodePhase(leader) * followerLength;
            result.hasFollow = true;
            return SyncGroupDiagnostic::FollowerMarkerMissing;
        }

        const float span = leaderNext - leaderPrevious;
        const float alpha = span > RangeEpsilon ?
            std::clamp((leader.time - leaderPrevious) / span, 0.0f, 1.0f) : 0.0f;
        result.effectiveMethod = SyncMethod::MarkerSync;
        result.followTime = followerPrevious + alpha * (followerNext - followerPrevious);
        result.hasFollow = true;
        return SyncGroupDiagnostic::None;
    }

    result.effectiveMethod = SyncMethod::SyncGroup;
    result.followTime = follower.rangeStart + NodePhase(leader) * followerLength;
    result.hasFollow = true;
    return SyncGroupDiagnostic::None;
}

void AnimationSyncGroupRegistry::Resolve()
{
    ++tickCount;
    results.assign(nodes.size(), SyncGroupNodeResult{});
    nodeGroup.assign(nodes.size(), -1);
    groups.clear();
    members.clear();
    memberOffsets.clear();

    // 分组：group 名 + scope 键。DoNotSync/空组不进入任何 group。
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        SyncGroupNode& node = nodes[index];
        results[index].role = node.role;
        results[index].authoredMethod = node.method;
        results[index].scope = node.scope;
        if (node.group.empty() || node.method == SyncMethod::DoNotSync)
        {
            results[index].doNotSync = true;
            continue;
        }
        if (node.method == SyncMethod::Graph)
        {
            results[index].doNotSync = true;
            results[index].diagnostic = SyncGroupDiagnostic::GraphNotExecuted;
            continue;
        }

        const std::string_view scopeKey =
            node.scope == SyncGroupScope::Component ? node.ownerKey : node.localKey;
        int groupIndex = -1;
        for (size_t candidate = 0; candidate < groups.size(); ++candidate)
        {
            if (groups[candidate].group == node.group &&
                groups[candidate].scopeKey == scopeKey &&
                groups[candidate].scope == node.scope)
            {
                groupIndex = static_cast<int>(candidate);
                break;
            }
        }
        if (groupIndex < 0)
        {
            groupIndex = static_cast<int>(groups.size());
            groups.push_back({node.group, scopeKey, node.scope, -1, 0, 0});
        }
        nodeGroup[index] = groupIndex;
        ++groups[static_cast<size_t>(groupIndex)].participantCount;
    }

    // 每个 group 的成员连续存放，便于 Elect 只扫描本组成员。
    memberOffsets.assign(groups.size() + 1, 0);
    for (size_t index = 0; index < nodeGroup.size(); ++index)
        if (nodeGroup[index] >= 0)
            ++memberOffsets[static_cast<size_t>(nodeGroup[index]) + 1];
    for (size_t group = 0; group < groups.size(); ++group)
        memberOffsets[group + 1] += memberOffsets[group];
    members.resize(memberOffsets.back());
    memberCursor.assign(memberOffsets.begin(), memberOffsets.end() - 1);
    for (size_t index = 0; index < nodeGroup.size(); ++index)
    {
        const int group = nodeGroup[index];
        if (group < 0) continue;
        members[memberCursor[static_cast<size_t>(group)]++] = static_cast<int>(index);
    }

    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex)
    {
        const int begin = static_cast<int>(memberOffsets[groupIndex]);
        const int end = static_cast<int>(memberOffsets[groupIndex + 1]);
        const Election election = Elect(nodes, members, static_cast<size_t>(begin),
            static_cast<size_t>(end));
        groups[groupIndex].leaderIndex = election.leader;

        for (int member = begin; member < end; ++member)
        {
            const int index = members[static_cast<size_t>(member)];
            SyncGroupNodeResult& result = results[static_cast<size_t>(index)];
            const SyncGroupNode& node = nodes[static_cast<size_t>(index)];
            result.leaderIndex = election.leader;
            result.effectiveMethod = node.method;

            if (index == election.leader)
            {
                result.leader = true;
                continue;
            }

            if (node.role == SyncRole::ExclusiveAlwaysLeader)
            {
                result.doNotSync = true;
                result.diagnostic = SyncGroupDiagnostic::ExclusiveLeaderRejected;
                continue;
            }
            if (node.role == SyncRole::TransitionFollower && node.transitioning)
            {
                // 混合期间完全不参与组；窗口结束后才作为 follower 加入。
                result.doNotSync = true;
                result.diagnostic = SyncGroupDiagnostic::TransitionFollowerInactive;
                continue;
            }

            result.follower = true;
            ++groups[groupIndex].followerCount;
            if (election.leader < 0)
            {
                result.diagnostic = SyncGroupDiagnostic::NoLeader;
                continue;
            }
            result.diagnostic = ResolveFollower(node,
                nodes[static_cast<size_t>(election.leader)], result);
        }
    }
}
} // namespace VL::Animation
