#include "motionRuntime.h"
#include <algorithm>

namespace VL::Animation
{
namespace
{
struct QueryPrediction
{
    Eigen::Vector3f velocity;
    bool blocked = false;
};

QueryPrediction MakeQueryPrediction(const MotionTrack& track)
{
    QueryPrediction result{track.walkVelocity};
    const float desiredSquared = track.walkVelocity.x() * track.walkVelocity.x() +
        track.walkVelocity.z() * track.walkVelocity.z();
    const float achievedSquared = track.achievedVelocity.x() * track.achievedVelocity.x() +
        track.achievedVelocity.z() * track.achievedVelocity.z();
    // 与正式控制器 Idle 的 2 cm/s 阈值一致：物理已拒绝当前地面移动时，
    // 查询不能继续预测穿墙行走。仅外推已达到的水平速度，不求解未来碰撞或部分滑墙。
    result.blocked = track.grounded && desiredSquared >= 4 && achievedSquared < 4;
    if (result.blocked)
        result.velocity = {track.achievedVelocity.x(), 0, track.achievedVelocity.z()};
    return result;
}

void ClearMotionQuery(MotionRuntime& runtime)
{
    runtime.rawQuery.clear();
    runtime.normalizedQuery.clear();
    runtime.queryRoot.setIdentity();
    runtime.queryFutureRoots.clear();
    runtime.queryUsesAchievedVelocity = false;
    runtime.queryReady = false;
}

void ApplyMotionSpeedMatch(MotionRuntime& runtime, const MotionTrack& track,
    SkeletalPlayback& player, const Eigen::Matrix4f& component, float deltaSeconds,
    const char* resetReason = nullptr)
{
    MotionLoopVelocity source;
    const auto& clips = runtime.database->assetClips;
    const auto member = std::find(clips.begin(), clips.end(), player.SelectedClip());
    if (player.IsLooping() && member != clips.end())
        source = runtime.database->loopVelocities.at(static_cast<size_t>(member - clips.begin()));
    auto state = ComputeMotionSpeedMatch(source, track, component, runtime.matchSpeed,
        player.IsBlending(), runtime.actionLocked, player.Rate(), deltaSeconds);
    if (resetReason)
    {
        state.active = false;
        state.rate = state.targetRate = 1;
        state.residualSpeed = state.actualSpeed - state.sourceSpeed;
        state.reason = runtime.matchSpeed ? resetReason : "disabled";
    }
    player.SetRate(state.rate);
    runtime.speedMatch = std::move(state);
}
}

MotionSpeedMatchState ComputeMotionSpeedMatch(const MotionLoopVelocity& source,
    const MotionTrack& track, const Eigen::Matrix4f& component, bool enabled,
    bool blending, bool actionLocked, float currentRate, float deltaSeconds)
{
    MotionSpeedMatchState state;
    state.enabled = enabled;
    // 模型原根速度先经过组件朝向和缩放，再和 Jolt 已达世界速度比较；不用期望输入速度。
    Eigen::Vector3f sourceVelocity = component.block<3, 3>(0, 0) * source.velocity;
    sourceVelocity.y() = 0;
    const Eigen::Vector3f actualVelocity{track.achievedVelocity.x(), 0, track.achievedVelocity.z()};
    state.sourceSpeed = sourceVelocity.norm();
    state.actualSpeed = actualVelocity.norm();
    if (!enabled) state.reason = "disabled";
    else if (!source.available) state.reason = "source-unavailable";
    else if (!track.grounded) state.reason = "airborne";
    else if (blending) state.reason = "blending";
    else if (actionLocked) state.reason = "action-locked";
    else if (state.actualSpeed < 10) state.reason = "actual-speed-low";
    else if (state.sourceSpeed < 10) state.reason = "source-speed-low";
    else if (sourceVelocity.dot(actualVelocity) / (state.sourceSpeed * state.actualSpeed) < 0.95f)
        state.reason = "direction-mismatch";
    else
    {
        state.active = true;
        const float ratio = state.actualSpeed / state.sourceSpeed;
        state.targetRate = std::clamp(ratio, 0.8f, 1.2f);
        const float maximumChange = 2 * deltaSeconds;
        state.rate = currentRate + std::clamp(state.targetRate - currentRate, -maximumChange, maximumChange);
        state.reason = state.targetRate == ratio ? "matching" : "rate-clamped";
    }
    // 退出条件立即回 1；该残差是速度预算，不是足底接触或滑步测量。
    state.residualSpeed = state.actualSpeed - state.sourceSpeed * state.rate;
    return state;
}

MotionObservation ObserveMotion(const SkeletalPlayback& player, const Eigen::Matrix4f& component,
    const std::vector<size_t>& joints, double time)
{
    MotionObservation observation;
    observation.time = time;
    observation.poseEpoch = player.Pose(0)->discontinuity;
    observation.root = HeadingFrame(component * player.MotionModels().front());
    for (size_t joint : joints)
        observation.joints.push_back((component * player.MotionModels().at(joint)).block<3, 1>(0, 3));
    return observation;
}

bool MotionHistoryDiscontinuous(const MotionTrack& track, const MotionObservation& observation)
{
    MotionObservation previous;
    return track.history.Sample(track.time, previous) &&
        (observation.time - previous.time > 0.5 ||
            (observation.root.block<3, 1>(0, 3) - previous.root.block<3, 1>(0, 3)).norm() > 2000);
}

bool BuildRuntimeMotionQuery(const MotionTrack& track, std::vector<float>& raw,
    const MotionTrack* displayedHistory)
{
    MotionObservation current, previous;
    if (!track.history.Sample(track.time, current) ||
        !track.history.Sample(track.time - track.schema.velocityStep, previous) ||
        previous.poseEpoch != current.poseEpoch || !current.poseValid || !previous.poseValid)
        return false;
    const auto prediction = MakeQueryPrediction(track);
    std::vector<Eigen::Matrix4f> trajectory;
    for (float offset : track.schema.trajectoryTimes)
    {
        if (offset >= 0)
            trajectory.push_back(PredictCharacterRoot(current.root, track.achievedVelocity,
                prediction.velocity, track.grounded, offset));
        else
        {
            MotionObservation past;
            if (!track.history.Sample(track.time + offset, past)) return false;
            trajectory.push_back(past.root);
        }
    }
    std::vector<Eigen::Vector3f> velocities;
    if (displayedHistory)
    {
        MotionObservation displayed, preceding;
        if (!displayedHistory->history.Sample(displayedHistory->time, displayed) ||
            !displayedHistory->history.Sample(displayedHistory->time - track.schema.velocityStep, preceding) ||
            displayed.poseEpoch != preceding.poseEpoch || displayed.poseEpoch != current.poseEpoch ||
            !displayed.poseValid || !preceding.poseValid)
            return false;
        const Eigen::Vector3f oldRootVelocity = (displayed.root.block<3, 1>(0, 3) -
            preceding.root.block<3, 1>(0, 3)) / track.schema.velocityStep;
        const Eigen::Vector3f currentRootVelocity = track.achievedVelocity;
        const Eigen::Matrix3f turn = current.root.block<3, 3>(0, 0) *
            displayed.root.block<3, 3>(0, 0).transpose();
        // 位置使用当前显示姿态；速度沿用最近两次真实 post-blend 差分并搬到当前朝向，
        // 根速度使用最近完成 Jolt 子步的世界速度；120 Hz 渲染根差分会交替 0/双倍，
        // 不能用渲染观察间隔再次解释物理速度。直接对临时重复姿态差分会抹掉局部动画速度。
        // v_now = R_now * R_displayed^-1 * (v_joint_displayed - v_root_displayed) + v_root_now。
        for (size_t index = 0; index < current.joints.size(); ++index)
            velocities.push_back(turn * ((displayed.joints[index] - preceding.joints[index]) /
                track.schema.velocityStep - oldRootVelocity) + currentRootVelocity);
    }
    else
        for (size_t index = 0; index < current.joints.size(); ++index)
            velocities.push_back((current.joints[index] - previous.joints[index]) / track.schema.velocityStep);
    raw = EncodeMotionFeatures(track.schema, current.root, trajectory, current.joints, velocities);
    return true;
}

void TickMotionRuntime(MotionRuntime& runtime, MotionTrack& track, SkeletalPlayback& player,
    const Eigen::Matrix4f& component, float deltaSeconds)
{
    if (deltaSeconds <= 0) return;
    // 自然到端点仍可接受新动作；显式暂停优先于 Completed，筛选变化也不能擅自恢复时钟。
    if (player.IsExplicitlyPaused()) return;
    if (!player.IsPlaying() && !player.Completed()) return;
    if (!player.InPlace())
        player.TransitionTo(player.Asset().clips.at(player.SelectedClip()).id, player.Time(),
            player.IsLooping(), 0, true);
    const auto& data = runtime.database->search->Data();
    const auto current = ObserveMotion(player, component, track.joints, track.time + deltaSeconds);
    const bool discontinuous = MotionHistoryDiscontinuous(track, current);
    if (discontinuous)
    {
        track.history.Reset();
        runtime.matcher.Reset();
        runtime.result = {};
        ClearMotionQuery(runtime);
        // 传送不把旧混合姿态拖到新位置；硬切当前目标也建立新的姿态差分代际。
        player.TransitionTo(player.Asset().clips.at(player.SelectedClip()).id, player.Time(),
            player.IsLooping(), 0, true);
    }
    // 搜索读本帧物理后的根与当前显示姿态；临时历史不覆盖稍后提交的 post-blend 观察。
    MotionTrack queryTrack = track;
    queryTrack.time += deltaSeconds;
    queryTrack.history.Record(ObserveMotion(player, component, track.joints, queryTrack.time));
    std::vector<float> currentRaw;
    const bool currentReady = BuildRuntimeMotionQuery(queryTrack, currentRaw, &track);
    const std::string clipId = player.Asset().clips.at(player.SelectedClip()).id;
    const bool allowed = std::find(runtime.allowedClips.begin(), runtime.allowedClips.end(), clipId) != runtime.allowedClips.end();
    const bool lockedTail = runtime.actionLocked && allowed && !player.IsLooping() &&
        !CanExtractClipFeatures(player.Asset().clips.at(player.SelectedClip()), data.schema, player.Time(), false);
    if (lockedTail)
    {
        runtime.result = {};
        runtime.result.reason = "action-locked-tail";
        ClearMotionQuery(runtime);
        ApplyMotionSpeedMatch(runtime, track, player, component, deltaSeconds, "action-locked-tail");
        player.Tick(deltaSeconds);
        return;
    }
    if (!currentReady)
    {
        runtime.result = {};
        runtime.result.reason = allowed ? "warming-up" : "warming-up-current-filtered";
        ClearMotionQuery(runtime);
        ApplyMotionSpeedMatch(runtime, track, player, component, deltaSeconds, "warming-up");
        if (allowed) player.Tick(deltaSeconds);
        return;
    }
    ApplyMotionSpeedMatch(runtime, track, player, component, deltaSeconds,
        discontinuous ? "teleported" : nullptr);
    const auto query = BindMotionQuery(runtime.database->search, data.schema, currentRaw);
    std::optional<MotionContinuation> continuation;
    if (allowed)
    {
        const auto& clip = player.Asset().clips.at(player.SelectedClip());
        MotionContinuation value{clipId, player.Time(), player.Duration(), {}};
        // 准确继续候选按当前真实倍率外推；数据库的新候选仍保持名义倍率 1。
        if (CanExtractClipFeatures(clip, data.schema, player.Time(), player.IsLooping(), player.Rate()))
            value.rawFeatures = ExtractClipFeatures(player.Asset().mesh, clip, data.schema,
                player.Time(), player.IsLooping(), player.Rate());
        continuation = std::move(value);
    }
    MotionMatchingPolicy policy;
    policy.actionLocked = runtime.actionLocked;
    runtime.result = runtime.matcher.Update(query, runtime.allowedClips, continuation, deltaSeconds, policy);
    if (runtime.result.searched)
    {
        ++runtime.searchCount;
        // 成本和查询必须属于同一次搜索；节流期间继续发布这组冻结值，不能用新查询解释旧排名。
        runtime.rawQuery = std::move(currentRaw);
        runtime.normalizedQuery = query.normalized;
        runtime.queryRoot = current.root;
        runtime.queryFutureRoots.clear();
        const auto prediction = MakeQueryPrediction(queryTrack);
        runtime.queryUsesAchievedVelocity = prediction.blocked;
        // 世界根和未来点与 raw/normalized/代价在同一次搜索提交；节流与 Pause 不重算。
        for (float offset : data.schema.trajectoryTimes)
            if (offset >= 0)
                runtime.queryFutureRoots.push_back(PredictCharacterRoot(current.root,
                    queryTrack.achievedVelocity, prediction.velocity, queryTrack.grounded, offset).block<3, 1>(0, 3));
        runtime.queryReady = true;
    }
    if (!runtime.result.selected)
    {
        ApplyMotionSpeedMatch(runtime, track, player, component, deltaSeconds, "no-selected");
        return; // 空候选显式冻结，不扩大 CLR 允许的集合。
    }
    const auto& selected = *runtime.result.selected;
    if (runtime.result.switched)
    {
        player.TransitionTo(selected.clipId, selected.time, data.looping.at(selected.memberIndex),
            player.InPlace() ? 0.18f : 0, true);
        ApplyMotionSpeedMatch(runtime, track, player, component, deltaSeconds, "transition");
    }
    player.Tick(deltaSeconds);
}
}
