#pragma once

#include "animationSequenceData.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace VL::Animation
{
class SkeletalPlayback;

// Registry 的诊断码。字符串面向 HUD/日志，枚举面向测试，避免测试依赖文案。
enum class SyncGroupDiagnostic
{
    None,
    GraphNotExecuted,
    ExclusiveLeaderRejected,
    NoLeader,
    LeaderMarkerMissing,
    FollowerMarkerMissing,
    TransitionFollowerInactive,
    InvalidRange
};

const char* SyncGroupDiagnosticName(SyncGroupDiagnostic diagnostic);

// 一个 scope 键内的参与者。group/identity/key 都是只读视图，所有权留在 World/资产，
// Registry 不复制字符串；每 tick 只复用内部 vector 容量。
struct SyncGroupNode
{
    // 仅用于 World 把结果回写到唯一播放器；纯 Registry 测试可以留空。
    SkeletalPlayback* playback = nullptr;
    std::string_view identity;
    std::string_view group;
    // Component scope 用 ownerKey（Actor id），Local scope 用 localKey（组件身份）。
    std::string_view ownerKey;
    std::string_view localKey;
    SyncRole role = SyncRole::CanBeLeader;
    SyncMethod method = SyncMethod::DoNotSync;
    SyncGroupScope scope = SyncGroupScope::Local;
    // 选举权重只对 CanBeLeader 生效；过渡中的源/目标通常取 1-alpha / alpha。
    float weight = 1;
    bool active = true;
    bool transitioning = false;
    // true = 这是真正可写入时间的播放器；过渡源只是影子时钟，不能回写。
    bool canApply = false;
    float time = 0;
    float rangeStart = 0;
    // rangeEnd <= rangeStart 时视为无效区间；纯测试夹具可只填 duration。
    float rangeEnd = 0;
    float rate = 1;
    bool looping = true;
    const std::vector<SyncMarker>* markers = nullptr;
};

struct SyncGroupNodeResult
{
    bool leader = false;
    bool follower = false;
    bool doNotSync = false;
    bool hasFollow = false;
    bool rateClamped = false;
    int leaderIndex = -1;
    SyncRole role = SyncRole::CanBeLeader;
    SyncMethod authoredMethod = SyncMethod::DoNotSync;
    SyncGroupScope scope = SyncGroupScope::Local;
    SyncMethod effectiveMethod = SyncMethod::DoNotSync;
    SyncGroupDiagnostic diagnostic = SyncGroupDiagnostic::None;
    // 已换算回 follower 的 clip-local 绝对时间（不是相位）。
    float followTime = 0;
    float followRate = 1;
};

struct SyncGroupResolution
{
    std::string_view group;
    std::string_view scopeKey;
    SyncGroupScope scope = SyncGroupScope::Local;
    int leaderIndex = -1;
    size_t participantCount = 0;
    size_t followerCount = 0;
};

// 每 group（按 scope 键分区）只产生一个 leader。所有容器在首次 tick 后不再扩容；
// 调用方每帧 BeginTick -> AddNode* -> Resolve，不创建第二份播放时钟。
class AnimationSyncGroupRegistry
{
public:
    static constexpr float RateMin = 0.1f;
    static constexpr float RateMax = 4.0f;

    void BeginTick();
    SyncGroupNode& AddNode(SyncGroupNode node);
    void Resolve();

    const std::vector<SyncGroupNode>& Nodes() const { return nodes; }
    const std::vector<SyncGroupNodeResult>& Results() const { return results; }
    const std::vector<SyncGroupResolution>& Groups() const { return groups; }
    size_t TickCount() const { return tickCount; }
    size_t NodeCapacity() const { return nodes.capacity(); }
    size_t ResultCapacity() const { return results.capacity(); }
    size_t GroupCapacity() const { return groups.capacity(); }

    // follower 速率：归一化相位速度相等 => rate_f / duration_f == rate_l / duration_l。
    // 结果限制在 [RateMin, RateMax]，调用方通过 rateClamped 标记诊断。
    static float FollowerRate(float leaderRate, float leaderDuration,
        float followerDuration, bool& clamped);

    // 最近一个 marker 名称（任意名字）；time 落在首个 marker 前时，循环资产回绕到
    // 上一轮最后一个 marker。找不到 marker 返回空。
    static std::string_view CurrentMarkerName(const std::vector<SyncMarker>& markers,
        float time, float rangeStart, float rangeEnd, bool looping);

    // 同名 marker 区间：previous 是 <=time 的最近同名 marker，next 是其后最近的同名
    // marker；循环资产在末尾没有下一个同名 marker 时按 duration 回绕。
    static bool MarkerInterval(const std::vector<SyncMarker>& markers, std::string_view name,
        float time, float rangeStart, float rangeEnd, bool looping,
        float& previousTime, float& nextTime);

private:
    struct Election
    {
        int leader = -1;
    };

    static float RangeLength(const SyncGroupNode& node);
    static float NodePhase(const SyncGroupNode& node);
    static Election Elect(const std::vector<SyncGroupNode>& nodes,
        const std::vector<int>& members, size_t begin, size_t end);
    static SyncGroupDiagnostic ResolveFollower(const SyncGroupNode& follower,
        const SyncGroupNode& leader, SyncGroupNodeResult& result);

    std::vector<SyncGroupNode> nodes;
    std::vector<SyncGroupNodeResult> results;
    std::vector<SyncGroupResolution> groups;
    // node -> group 下标；与 nodes 同长，复用容量。
    std::vector<int> nodeGroup;
    // 每个 group 的成员下标；复用外层的扁平存储，避免每 group 一个 vector。
    std::vector<int> members;
    std::vector<size_t> memberOffsets;
    std::vector<size_t> memberCursor;
    uint64_t tickCount = 0;
};
} // namespace VL::Animation
