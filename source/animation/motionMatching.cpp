#include "motionMatching.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace VL::Animation
{
namespace
{
void ValidatePolicy(const MotionMatchingPolicy& policy)
{
    for (double value : {policy.searchInterval, policy.continuationBias, policy.minimumImprovement,
        policy.reselectionWindow, policy.sameClipMinimumJump})
        if (!std::isfinite(value) || value < 0)
            throw std::invalid_argument("Motion matching policy requires finite nonnegative values");
    if (policy.topK == 0 || policy.topK > 32)
        throw std::invalid_argument("Motion matching topK must be in [1,32]");
}

void ValidateQuery(const MotionQuery& query)
{
    if (!query.database || query.normalized.size() != query.database->Weights().size())
        throw std::invalid_argument("Motion query is not bound to a compatible database");
    for (float value : query.normalized)
        if (!std::isfinite(value))
            throw std::invalid_argument("Motion query requires finite normalized features");
}

std::vector<std::string> CanonicalAllowed(const std::vector<std::string>& allowed)
{
    if (allowed.size() > 256) throw std::invalid_argument("Motion allowed-member budget exceeded");
    std::set<std::string> unique(allowed.begin(), allowed.end());
    return {unique.begin(), unique.end()};
}

bool IsAllowed(const std::vector<std::string>& allowed, const std::string& clipId)
{
    return std::binary_search(allowed.begin(), allowed.end(), clipId);
}

bool BetterCandidate(const MotionCandidate& left, const MotionCandidate& right)
{
    if (left.totalCost != right.totalCost) return left.totalCost < right.totalCost;
    if (left.clipId != right.clipId) return left.clipId < right.clipId;
    if (left.memberIndex != right.memberIndex) return left.memberIndex < right.memberIndex;
    if (left.frameIndex != right.frameIndex) return left.frameIndex < right.frameIndex;
    return left.continuing && !right.continuing;
}

double TimeDistance(double left, double right, bool loop, double duration)
{
    const double delta = std::abs(left - right);
    return loop ? std::min(delta, duration - delta) : delta;
}

// 同时核对业务许可和当前时刻的完整特征窗口；不能仅凭同名片段延续旧数据库的资格。
std::optional<MotionCandidate> FindContinuation(const MotionQuery& query,
    const std::vector<std::string>& allowed, const std::optional<MotionContinuation>& input)
{
    if (!input) return std::nullopt;
    const auto& current = *input;
    if (!std::isfinite(current.time) || !std::isfinite(current.duration) ||
        current.duration <= 0 || current.duration > 600 || current.time < 0 ||
        current.time > current.duration)
        throw std::invalid_argument("Motion continuation time/duration is invalid");
    const auto& database = query.database->Data();
    if (!IsAllowed(allowed, current.clipId)) return std::nullopt;
    const auto found = std::find(database.clipIds.begin(), database.clipIds.end(), current.clipId);
    if (found == database.clipIds.end()) return std::nullopt;
    const size_t member = static_cast<size_t>(found - database.clipIds.begin());
    const bool loop = database.looping[member];
    const auto& schema = database.schema;
    if (!loop && (current.time + std::min(-schema.velocityStep, schema.trajectoryTimes.front()) < 0 ||
        current.time + std::max(0.0f, schema.trajectoryTimes.back()) > current.duration))
        return std::nullopt;
    std::optional<MotionCandidate> result;
    double nearest = std::numeric_limits<double>::max();
    for (size_t i = 0; i < database.frames.size(); ++i)
    {
        const auto& frame = database.frames[i];
        if (frame.clip != member) continue;
        if (frame.time > current.duration || (loop && frame.time == current.duration)) continue;
        const double distance = TimeDistance(frame.time, current.time, loop, current.duration);
        if (distance < nearest)
        {
            nearest = distance;
            MotionCandidate candidate;
            candidate.memberIndex = member;
            candidate.frameIndex = i;
            candidate.clipId = current.clipId;
            candidate.time = current.time;
            candidate.continuing = true;
            candidate.quantizationSeconds = current.rawFeatures.empty() ? distance : 0;
            result = std::move(candidate);
        }
    }
    // 量化路径不能借稀疏表的远端点续播；准确 raw 已通过真实时刻的窗口资格，不依赖网格对齐。
    if (!result || (current.rawFeatures.empty() && nearest > 0.5 / schema.sampleRate + 1e-5))
        return std::nullopt;
    return result;
}

void ScoreCandidate(const MotionQuery& query, const std::vector<float>& features,
    MotionCandidate& candidate, const MotionMatchingPolicy& policy)
{
    const size_t trajectoryDimensions = query.database->Data().schema.trajectoryTimes.size() * 4;
    const auto& weights = query.database->Weights();
    for (size_t i = 0; i < features.size(); ++i)
    {
        const double delta = double(query.normalized[i]) - features[i];
        const double cost = weights[i] * delta * delta;
        if (i < trajectoryDimensions) candidate.trajectoryCost += cost;
        else candidate.poseCost += cost;
    }
    candidate.distanceCost = candidate.trajectoryCost + candidate.poseCost;
    candidate.policyCost = candidate.continuing ? -policy.continuationBias : 0;
    candidate.totalCost = candidate.distanceCost + candidate.policyCost;
    if (!std::isfinite(candidate.totalCost))
        throw std::invalid_argument("Motion distance exceeds finite double range");
}

void KeepTopCandidate(std::vector<MotionCandidate>& top, const MotionCandidate& candidate, size_t topK)
{
    // 只保留 K 个值，不为完整数据库创建第二份候选表；拒绝原因仍随各行发布。
    const auto position = std::lower_bound(top.begin(), top.end(), candidate, BetterCandidate);
    top.insert(position, candidate);
    if (top.size() > topK) top.pop_back();
}

void ApplySelectionPolicy(MotionCandidate& candidate, const MotionCandidate& continuation,
    const MotionContinuation& input, const MotionMatchingPolicy& policy, double selectionAge,
    bool loop)
{
    if (policy.actionLocked) candidate.rejection = "action-locked";
    else if (selectionAge < policy.reselectionWindow) candidate.rejection = "reselection-window";
    else if (candidate.memberIndex == continuation.memberIndex &&
        TimeDistance(candidate.time, input.time, loop, input.duration) < policy.sameClipMinimumJump)
        candidate.rejection = "same-clip-nearby";
    else if (continuation.totalCost - candidate.totalCost <= policy.minimumImprovement)
        candidate.rejection = "minimum-improvement";
    candidate.eligible = candidate.rejection.empty();
}

bool RejectsNonloopTailRewind(const MotionDatabase& database, const MotionCandidate& candidate,
    const std::optional<MotionContinuation>& input)
{
    if (!input || database.looping[candidate.memberIndex] || candidate.clipId != input->clipId)
        return false;
    const float futureWindow = std::max(0.0f, database.schema.trajectoryTimes.back());
    // 非循环尾窗不再有可评分的继续帧，但当前播放身份仍有效；不能因失去继续代价
    // 就把同片段早帧当全新动作反复重启。其他片段继续按原始成本竞争。
    return (input->time >= input->duration || input->time + futureWindow > input->duration) &&
        candidate.time < input->time;
}
}

std::shared_ptr<const MotionSearchDatabase> FreezeMotionDatabase(MotionDatabase database)
{
    // 按值接管，避免外部持有可写 alias；解析边界复用数据库格式的完整有限值/有序性校验。
    auto result = std::shared_ptr<MotionSearchDatabase>(new MotionSearchDatabase);
    result->database = ParseMotionDatabase(MotionDatabaseJson(database));
    result->weights = FeatureChannelWeights(result->database.schema);
    result->normalizedFrames.reserve(result->database.frames.size());
    for (const auto& frame : result->database.frames)
        result->normalizedFrames.push_back(result->database.Normalize(frame.features));
    return result;
}

MotionQuery BindMotionQuery(std::shared_ptr<const MotionSearchDatabase> database,
    const FeatureSchema& schema, const std::vector<float>& rawFeatures)
{
    if (!database || FeatureSchemaJson(schema) != FeatureSchemaJson(database->Data().schema))
        throw std::invalid_argument("Motion query schema differs from bound database");
    MotionQuery result;
    result.normalized = database->Data().Normalize(rawFeatures);
    result.database = std::move(database);
    return result;
}

MotionSearchResult SearchMotionDatabase(const MotionQuery& query,
    const std::vector<std::string>& allowedClipIds,
    const std::optional<MotionContinuation>& input,
    const MotionMatchingPolicy& policy, double selectionAge)
{
    ValidatePolicy(policy);
    ValidateQuery(query);
    if (!std::isfinite(selectionAge) || selectionAge < 0)
        throw std::invalid_argument("Motion selection age requires finite nonnegative seconds");
    const auto allowed = CanonicalAllowed(allowedClipIds);
    const auto& database = query.database->Data();
    MotionSearchResult result;
    result.searched = true;
    result.continuation = FindContinuation(query, allowed, input);
    if (result.continuation)
    {
        const auto features = input->rawFeatures.empty() ?
            query.database->NormalizedFrames()[result.continuation->frameIndex] :
            database.Normalize(input->rawFeatures);
        ScoreCandidate(query, features, *result.continuation, policy);
        result.selected = result.continuation;
        KeepTopCandidate(result.topCandidates, *result.continuation, policy.topK);
    }
    for (size_t i = 0; i < database.frames.size(); ++i)
    {
        const auto& frame = database.frames[i];
        const auto& clipId = database.clipIds[frame.clip];
        if (!IsAllowed(allowed, clipId)) continue;
        ++result.allowedFrameCount;
        // 仅同一时刻或同一量化特征才去重；准确的半帧继续姿态不能误删附近的有效候选。
        if (result.continuation && result.continuation->frameIndex == i &&
            (input->rawFeatures.empty() ||
                TimeDistance(frame.time, input->time, database.looping[frame.clip], input->duration) < 1e-5))
            continue;
        MotionCandidate candidate;
        candidate.memberIndex = frame.clip;
        candidate.frameIndex = i;
        candidate.clipId = clipId;
        candidate.time = frame.time;
        ScoreCandidate(query, query.database->NormalizedFrames()[i], candidate, policy);
        if (result.continuation)
            ApplySelectionPolicy(candidate, *result.continuation, *input, policy, selectionAge,
                database.looping[frame.clip]);
        else if (RejectsNonloopTailRewind(database, candidate, input))
        {
            candidate.eligible = false;
            candidate.rejection = "nonloop-tail-rewind";
        }
        if (!candidate.eligible) ++result.policyRejectedCount;
        else if (!result.selected || BetterCandidate(candidate, *result.selected)) result.selected = candidate;
        KeepTopCandidate(result.topCandidates, candidate, policy.topK);
    }
    result.switched = result.selected && !result.selected->continuing;
    if (!result.selected) result.reason = "no-allowed-candidates";
    else if (result.switched) result.reason = "selected";
    else if (policy.actionLocked) result.reason = "action-locked";
    else if (selectionAge < policy.reselectionWindow) result.reason = "reselection-window";
    else result.reason = "continuing";
    return result;
}

MotionSearchResult MotionMatcher::Update(const MotionQuery& query,
    const std::vector<std::string>& allowedClipIds,
    const std::optional<MotionContinuation>& continuation, double deltaSeconds,
    const MotionMatchingPolicy& policy)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0)
        throw std::invalid_argument("Motion matching delta requires finite nonnegative seconds");
    // Pause/零倍率没有首帧特例，不积累搜索债务，也不接纳此时的新数据库/业务筛选。
    if (deltaSeconds == 0)
    {
        auto result = lastResult;
        result.searched = false;
        result.switched = false;
        result.reason = "paused";
        return result;
    }
    ValidatePolicy(policy);
    ValidateQuery(query);
    const auto newAllowed = CanonicalAllowed(allowedClipIds);
    const bool changed = database != query.database || allowed != newAllowed;
    const bool continuing = FindContinuation(query, newAllowed, continuation).has_value();
    const double nextSearchAge = searchAge + deltaSeconds;
    const double nextSelectionAge = selectionAge + deltaSeconds;
    if (!std::isfinite(nextSearchAge) || !std::isfinite(nextSelectionAge))
        throw std::invalid_argument("Motion matching accumulated clock exceeds finite range");
    const bool lostContinuation = lastResult.selected.has_value() && !continuing;
    if (!initialized || changed || lostContinuation || nextSearchAge >= policy.searchInterval)
    {
        auto result = SearchMotionDatabase(query, newAllowed, continuation, policy, nextSelectionAge);
        database = query.database;
        allowed = newAllowed;
        initialized = true;
        searchAge = 0;
        selectionAge = result.switched ? 0 : nextSelectionAge;
        lastResult = result;
        return result;
    }
    searchAge = nextSearchAge;
    selectionAge = nextSelectionAge;
    auto result = lastResult;
    result.searched = false;
    result.switched = false;
    // 节流不能掩盖空筛选的停止原因；有选中帧时才显示正常搜索间隔。
    result.reason = result.selected ? "search-interval" : lastResult.reason;
    return result;
}

void MotionMatcher::Reset()
{
    database.reset();
    allowed.clear();
    searchAge = 0;
    selectionAge = 0;
    initialized = false;
    lastResult = {};
}
}
