#include "skeletalPlaybackInternal.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace VL::Animation
{
void SkeletalPlayback::TickMontageSections(float deltaSeconds,
    const Eigen::Matrix4f* meshTransform, const Eigen::Vector3f* actorPosition, float actorYaw)
{
    const auto& clip = asset->AnimationAsset().clips.at(selected);
    const float previousTime = time;
    const float elapsed = IsBlending() ? std::min(blendElapsed + deltaSeconds, blendDuration) : 0;
    double remaining = static_cast<double>(deltaSeconds) * rate;
    float position = time;
    Eigen::Vector3f translation = Eigen::Vector3f::Zero();
    float yaw = 0;
    // 段切换的时间跳跃不计入根运动：只累加“实际移动过”的子区间，等价 UE 每个
    // sub-step 先 ExtractRootMotionFromTrackRange 再 Accumulate（AnimMontage.cpp:2649-2670）；
    // 跳转本身不产生根位移/根旋转。
    int jumps = 0;
    // UE 单 tick 最多 10 个 sub-step（AnimMontage.cpp:2575）。超过时放弃本 tick 剩余
    // 时间、保留当前位置继续下一 tick，避免 section 自连接时死循环。
    constexpr int maxSectionSteps = 10;
    // 链接落点与段末相差小于该值视为连续过渡（导入/序列化浮点误差），不按“向后跳”处理。
    constexpr float sectionJumpEpsilon = 1e-4f;
    int steps = 0;
    for (; remaining > 0 && steps < maxSectionSteps; ++steps)
    {
        const float segmentStart = position;
        const float segmentEnd = PlaybackRangeEnd();
        const double target = static_cast<double>(segmentStart) + remaining;
        const bool reachesEnd = target >= static_cast<double>(segmentEnd);
        const float segmentStop = reachesEnd ? segmentEnd : static_cast<float>(target);
        translation += RootTranslation(clip, segmentStop) - RootTranslation(clip, segmentStart);
        yaw = std::remainder(yaw + YawFromQuaternion(RootRotation(clip, segmentStop) *
            RootRotation(clip, segmentStart).inverse()), 2.0f * 3.14159265358979323846f);
        remaining -= std::max(0.0,
            static_cast<double>(segmentStop) - static_cast<double>(segmentStart));
        position = segmentStop;
        const int nextSection = reachesEnd ? MontageNextSectionIndex() : -1;
        // 段边界按半开区间 [segmentStart, segmentStop) 扫描：恰好落在段末的事件属于下一段，
        // 由下一段的 from 触发（下一段起点 == 段末时只触发一次，不重复也不丢）。整条链的
        // 最后一段沿用一次性播放的 terminal 语义（允许 time==end 的瞬时 notify），
        // 与未安装 section 链的单段路径一致。
        ScanNotifySegment(static_cast<double>(segmentStart), static_cast<double>(segmentStop),
            reachesEnd && nextSection < 0);
        if (!reachesEnd) break;
        if (nextSection < 0)
        {
            // nextSection 为空：停在段末并保留当前姿态；Completed()/Stop 语义与单段一致。
            EndActiveNotifyStates(segmentEnd, true, false);
            break;
        }
        const MontageSection& currentSection = montageSections.at(montageSection);
        const float linkStart = ResolveMontageSectionLinkTime(currentSection);
        // 向后跳（section 自连接/回前段）沿用既有循环回绕语义：先在段末补
        // end(interrupted)，再扫描新段头部；向前跳/连续过渡不结束活动区间
        // （UE 只改 Position，活动 notifyState 由后续时间穿越结束，见
        // AnimMontage.cpp:2715-2733）。前跳越过 notifyState 结束点时，下一次扫描
        // 会以新段起点补 end，不会留下永久的活动区间。
        if (linkStart < segmentEnd - sectionJumpEpsilon)
            EndActiveNotifyStates(segmentEnd, true, false);
        const MontageSection& next = montageSections.at(static_cast<size_t>(nextSection));
        montageSection = static_cast<size_t>(nextSection);
        // 只改范围，不经过公开 SetPlaybackRange（那会清空刚推进的 section 链）。
        SetPlaybackRangeInternal(next.startTime, next.endTime);
        // PlayMontage 已校验 linkStart 落在目标 section 内；这里只做防御性 clamp，
        // 保证时钟永远在 [rangeStart,rangeEnd]。
        position = std::clamp(linkStart, PlaybackRangeStart(), PlaybackRangeEnd());
        if (std::abs(position - segmentStop) > sectionJumpEpsilon) ++jumps;
    }
    // 时间跳变是真实姿态不连续：与 Seek/Select 一样推进 discontinuity。连续 section
    // 过渡（position == 段末）不推进，post-blend 姿态历史保持连续。
    const uint64_t epoch = discontinuity + (jumps > 0 ? 1 : 0);
    auto locals = Sample(selected, position);
    if (!inertialization.Empty() && elapsed < blendDuration)
    {
        const auto target = ReadKinematics(locals, asset->mesh.joints.size());
        locals = WriteKinematics(std::move(locals), inertialization.Evaluate(target, elapsed));
    }
    else if (!blendSource.empty() && elapsed < blendDuration)
        locals = Crossfade(blendSource, std::move(locals), elapsed / blendDuration, asset->mesh);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> nextModels;
    auto nextPoses = Evaluate(locals, epoch, nextModels);
    poses.swap(nextPoses);
    jointModels.swap(nextModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    displayedLocals.swap(locals);
    rootMotionDelta = translation;
    float yawDelta = yaw;
    // warp 窗口时间与 clip-local 时间同源；窗口队列在切段时不重建。若本 tick 最终位置
    // 在起点之前（回前段），MotionWarpRuntime 会因 current<=previous 跳过窗口，本 tick
    // 只保留原始根运动——这是一次真实的位置跳变，不应把跳跃区间算成 warp 位移。
    if (!motionWarp.Empty())
    {
        if (meshTransform && actorPosition)
            rootMotionDelta = motionWarp.Apply(asset->AnimationAsset().mesh, clip, previousTime, position,
                translation, *meshTransform, *actorPosition, actorYaw, &yawDelta);
        motionWarpState = motionWarp.DebugState(previousTime);
    }
    else motionWarpState.clear();
    rootMotionYawDelta = yawDelta;
    time = position;
    RefreshActiveNotifyStates();
    blendElapsed = elapsed;
    if (IsBlending() && elapsed >= blendDuration)
    {
        blendSource.clear();
        inertialization.Clear();
    }
    if (Completed()) playing = false;
    discontinuity = epoch;
}

Eigen::Vector3f SkeletalPlayback::ApplySegmentedMontageWarp(
    const MontageSegment& segment, const Clip& clip, float sourceFrom, float sourceTo,
    const Eigen::Vector3f& rawDelta, const Eigen::Matrix4f& meshTransform,
    const Eigen::Vector3f& actorPosition, float actorYaw, float& yawDelta) const
{
    if (motionWarp.Empty()) return rawDelta;

    const float clipDuration = clip.runtime->duration();
    std::vector<MotionWarpWindow> mapped;
    mapped.reserve(motionWarp.Windows().size());
    for (const MotionWarpWindow& window : motionWarp.Windows())
    {
        const float begin = std::max(window.startTime, segment.startTime);
        const float end = std::min(window.endTime, segment.endTime);
        if (!(end > begin)) continue;
        // 窗口时间从 Montage 时间轴映射到该段 clip 本地时间；跨段窗口在每段
        // 各自裁剪，避免用上一段的 clip 时间采样下一段的根/骨骼。
        MotionWarpWindow local = window;
        local.startTime = MontageSourceTime(segment, begin, clipDuration);
        local.endTime = MontageSourceTime(segment, end, clipDuration);
        if (!(local.endTime > local.startTime)) continue;
        mapped.push_back(std::move(local));
    }
    if (mapped.empty()) return rawDelta;

    MotionWarpRuntime mappedWarp;
    mappedWarp.SetWindows(std::move(mapped));
    return mappedWarp.Apply(asset->AnimationAsset().mesh, clip, sourceFrom, sourceTo,
        rawDelta, meshTransform, actorPosition, actorYaw, &yawDelta);
}

void SkeletalPlayback::TickSegmentedMontage(float deltaSeconds,
    const Eigen::Matrix4f* meshTransform, const Eigen::Vector3f* actorPosition,
    float actorYaw)
{
    const auto& clips = asset->AnimationAsset().clips;
    const float previousTime = time;
    const float elapsed = IsBlending() ? std::min(blendElapsed + deltaSeconds, blendDuration) : 0;
    double remaining = static_cast<double>(deltaSeconds) * rate;
    float position = time;
    Eigen::Vector3f translation = Eigen::Vector3f::Zero();
    float yaw = 0;
    int jumps = 0;
    constexpr int maxSteps = 10;
    constexpr float epsilon = 1e-5f;
    int steps = 0;

    while (remaining > epsilon && steps < maxSteps)
    {
        ++steps;
        if (montageSections.empty()) break;
        const MontageSection& section = montageSections.at(montageSection);
        const float sectionEnd = section.endTime;
        MontageSample sample = SampleMontageTimeline(position, position >= sectionEnd);
        const size_t segmentIndex = sample.segment;
        const MontageSegment& segment = montageSegments.at(segmentIndex);
        const Clip& clip = clips.at(montageSegmentClips.at(segmentIndex));
        const float clipDuration = clip.runtime->duration();

        const double target = static_cast<double>(position) + remaining;
        const float segmentEnd = std::min(sectionEnd, segment.endTime);
        const float pieceStop = std::min(static_cast<float>(target), segmentEnd);
        const bool reachesSectionEnd = pieceStop >= sectionEnd - epsilon;
        if (!(pieceStop > position + epsilon))
        {
            // 已经站在段末：只推进段身份，不消耗时间。
            if (segmentIndex + 1 < montageSegments.size() && position < sectionEnd)
            {
                montageSegment = segmentIndex + 1;
                selected = montageSegmentClips[montageSegment];
                ++jumps;
                continue;
            }
            break;
        }

        const float sourceFrom = MontageSourceTime(segment, position, clipDuration);
        const float sourceTo = MontageSourceTime(segment, pieceStop, clipDuration);
        Eigen::Vector3f pieceDelta = RootTranslation(clip, sourceTo) -
            RootTranslation(clip, sourceFrom);
        float pieceYaw = YawFromQuaternion(RootRotation(clip, sourceTo) *
            RootRotation(clip, sourceFrom).inverse());
        if (!motionWarp.Empty() && meshTransform && actorPosition)
        {
            pieceDelta = ApplySegmentedMontageWarp(segment, clip, sourceFrom, sourceTo,
                pieceDelta, *meshTransform, *actorPosition, actorYaw, pieceYaw);
        }
        translation += pieceDelta;
        yaw = std::remainder(yaw + pieceYaw, 2.0f * 3.14159265358979323846f);

        remaining -= std::max(0.0,
            static_cast<double>(pieceStop) - static_cast<double>(position));
        const float pieceStart = position;
        position = pieceStop;
        const int nextSection = reachesSectionEnd ? MontageNextSectionIndex() : -1;
        ScanNotifySegment(static_cast<double>(pieceStart), static_cast<double>(pieceStop),
            reachesSectionEnd && nextSection < 0);

        if (reachesSectionEnd)
        {
            if (nextSection < 0)
            {
                EndActiveNotifyStates(sectionEnd, true, false);
                break;
            }
            const float linkStart = ResolveMontageSectionLinkTime(section);
            if (linkStart < sectionEnd - 1e-4f)
                EndActiveNotifyStates(sectionEnd, true, false);
            const MontageSection& next = montageSections.at(static_cast<size_t>(nextSection));
            montageSection = static_cast<size_t>(nextSection);
            SetPlaybackRangeInternal(next.startTime, next.endTime);
            position = std::clamp(linkStart, PlaybackRangeStart(), PlaybackRangeEnd());
            if (std::abs(position - sectionEnd) > 1e-4f) ++jumps;
            continue;
        }
        if (segmentIndex + 1 < montageSegments.size() &&
            position >= segment.endTime - epsilon)
        {
            montageSegment = segmentIndex + 1;
            selected = montageSegmentClips[montageSegment];
            ++jumps;
            continue;
        }
        break;
    }

    const MontageSample finalSample = SampleMontageTimeline(position,
        position >= PlaybackRangeEnd() - epsilon);
    montageSegment = finalSample.segment;
    selected = finalSample.clip;
    auto locals = Sample(selected, finalSample.clipTime);
    if (!inertialization.Empty() && elapsed < blendDuration)
    {
        const auto target = ReadKinematics(locals, asset->mesh.joints.size());
        locals = WriteKinematics(std::move(locals), inertialization.Evaluate(target, elapsed));
    }
    else if (!blendSource.empty() && elapsed < blendDuration)
        locals = Crossfade(blendSource, std::move(locals), elapsed / blendDuration, asset->mesh);
    const uint64_t epoch = discontinuity + (jumps > 0 ? 1 : 0);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> nextModels;
    auto nextPoses = Evaluate(locals, epoch, nextModels);
    poses.swap(nextPoses);
    jointModels.swap(nextModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    displayedLocals.swap(locals);
    rootMotionDelta = translation;
    rootMotionYawDelta = yaw;
    motionWarpState = motionWarp.Empty() ? std::string() :
        motionWarp.DebugState(previousTime);
    time = position;
    RefreshActiveNotifyStates();
    blendElapsed = elapsed;
    if (IsBlending() && elapsed >= blendDuration)
    {
        blendSource.clear();
        inertialization.Clear();
    }
    if (Completed()) playing = false;
    discontinuity = epoch;
}

void SkeletalPlayback::SetMotionWarpWindows(std::vector<MotionWarpWindow> windows)
{
    motionWarp.SetWindows(std::move(windows));
    motionWarpState.clear();
}

void SkeletalPlayback::ClearMotionWarp()
{
    motionWarp.Clear();
    motionWarpState.clear();
}

void SkeletalPlayback::SetRate(float speed)
{
    if (!std::isfinite(speed) || speed <= 0 || speed > 4)
        throw std::invalid_argument("Animation rate must be in (0,4]");
    rate = speed;
    requestedRate = speed;
}

void SkeletalPlayback::SetRootOffsetEnabled(bool enabled)
{
    if (rootOffset.State().enabled == enabled) return;
    if (enabled) rootOffset.SetEnabled(true);
    else
    {
        SkeletalPlayback candidate(*this);
        candidate.rootOffset.SetEnabled(false);
        candidate.ResetRootOffset("disabled");
        *this = std::move(candidate);
    }
}

void SkeletalPlayback::ResetRootOffset(const char* reason)
{
    auto nextRoot = rootOffset;
    nextRoot.Reset(reason);
    auto nextFoot = footPlacement;
    if (nextFoot) nextFoot->Reset(reason);
    if (jointModels != motionModels)
    {
        std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
        auto restored = Evaluate(displayedLocals, discontinuity, models);
        poses.swap(restored);
        jointModels = motionModels;
    }
    preIkModels = motionModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    footPlacement = std::move(nextFoot);
    rootOffset = std::move(nextRoot);
}

void SkeletalPlayback::ApplyVisualRoot(const Eigen::Matrix4f& component,
    const Eigen::Vector3f& achievedVelocity, float deltaSeconds)
{
    if (!rootOffset.State().enabled || deltaSeconds <= 0 || explicitlyPaused ||
        (!playing && !Completed())) return;
    if (!std::isfinite(deltaSeconds) || !achievedVelocity.allFinite())
        throw std::invalid_argument("Visual root input must be finite");
    if (!inPlace)
    {
        ResetRootOffset("root-motion");
        return;
    }
    FootPlacementSolver::ValidateComponent(component);
    auto candidate = rootOffset;
    candidate.Update(component, achievedVelocity, deltaSeconds);
    auto joints = ReadKinematics(displayedLocals, asset->mesh.joints.size());
    Eigen::Matrix4f worldCorrection = Eigen::Matrix4f::Identity();
    worldCorrection.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(candidate.State().yawRadians, Eigen::Vector3f::UnitY()).toRotationMatrix();
    const Eigen::Vector3f pivot = component.block<3, 1>(0, 3);
    worldCorrection.block<3, 1>(0, 3) = pivot + candidate.State().worldTranslation -
        worldCorrection.block<3, 3>(0, 0) * pivot;
    const Eigen::Matrix4f localCorrection = component.inverse() * worldCorrection * component;
    joints.front().translation =
        (localCorrection * joints.front().translation.homogeneous()).head<3>();
    joints.front().rotation = (Eigen::Quaternionf(localCorrection.block<3, 3>(0, 0)).normalized() *
        joints.front().rotation).normalized();
    auto locals = WriteKinematics(displayedLocals, joints);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
    auto corrected = Evaluate(locals, discontinuity, models);
    poses.swap(corrected);
    preIkModels = models;
    jointModels.swap(models);
    visualLocals.swap(locals);
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    rootOffset = std::move(candidate);
}

void SkeletalPlayback::SetFootPlacementEnabled(bool enabled)
{
    if (enabled && !footPlacement)
    {
        footPlacement.emplace(asset->mesh);
        footPlacement->SetLockType(footLockType);
    }
    if (!footPlacement || footPlacement->State().enabled == enabled) return;
    footPlacement->SetEnabled(enabled);
    footPlacementSource.reset();
    if (!enabled) ResetFootPlacement("disabled");
}

void SkeletalPlayback::SetFootPlacementLockType(FootLockType type)
{
    if (footLockType == type) return;
    footLockType = type;
    if (!footPlacement) return;
    footPlacement->SetLockType(type);
    footPlacementSource.reset();
    ResetFootPlacement("lock-mode");
}

const FootPlacementState& SkeletalPlayback::FootPlacement() const
{
    static const FootPlacementState disabled;
    return footPlacement ? footPlacement->State() : disabled;
}

void SkeletalPlayback::SetSlopeWarpingEnabled(bool enabled)
{
    if (enabled)
    {
        if (!slopeWarping) slopeWarping.emplace(asset->mesh);
        slopeWarping->SetParameters(slopeWarpingSettings);
        slopeWarping->SetEnabled(true);
        return;
    }
    if (!slopeWarping) slopeWarping.emplace(asset->mesh);
    if (!slopeWarping->Enabled()) return;
    // 先复位并恢复基础姿态，再关闭 solver；ResetSlopeWarping 需要读取 active 状态。
    ResetSlopeWarping("disabled");
    slopeWarping->SetEnabled(false);
}

bool SkeletalPlayback::SlopeWarpingEnabled() const
{
    return slopeWarping && slopeWarping->Enabled();
}

void SkeletalPlayback::SetSlopeWarpingParameters(const SlopeWarpingParameters& parameters)
{
    slopeWarpingSettings = parameters;
    if (slopeWarping) slopeWarping->SetParameters(parameters);
}

void SkeletalPlayback::ResetSlopeWarping(const char* reason)
{
    if (!slopeWarping) return;
    const bool wasActive = slopeWarping->State().active;
    slopeWarping->Reset(reason);
    if (!wasActive) return;
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
    auto restored = Evaluate(visualLocals.empty() ? displayedLocals : visualLocals,
        discontinuity, models);
    poses.swap(restored);
    jointModels.swap(models);
    preIkModels = jointModels;
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
}

void SkeletalPlayback::ApplySlopeWarping(const Eigen::Matrix4f& component, bool grounded,
    const std::string& owner, float deltaSeconds, const GroundProbe& probe)
{
    if (!slopeWarping || !slopeWarping->Enabled() || deltaSeconds <= 0 ||
        explicitlyPaused || (!playing && !Completed())) return;
    FootPlacementSolver::ValidateComponent(component);
    const LocalPose& base = visualLocals.empty() ? displayedLocals : visualLocals;
    auto candidate = *slopeWarping;
    bool changed = false;
    const auto solved = candidate.Solve(asset->mesh, base, *preIkModels, component,
        grounded, owner, deltaSeconds, probe, changed);
    if (!changed)
    {
        const bool wasActive = slopeWarping->State().active;
        *slopeWarping = std::move(candidate);
        if (wasActive)
        {
            std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
            auto restored = Evaluate(base, discontinuity, models);
            poses.swap(restored);
            jointModels.swap(models);
            preIkModels = jointModels;
            slopeWarpedLocals.clear();
            footPlacementSource.reset();
        }
        return;
    }
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
    auto corrected = Evaluate(solved, discontinuity, models);
    poses.swap(corrected);
    jointModels.swap(models);
    preIkModels = jointModels;
    slopeWarpedLocals = solved;
    footPlacementSource.reset();
    *slopeWarping = std::move(candidate);
}

const SlopeWarpingState& SkeletalPlayback::SlopeWarping() const
{
    static const SlopeWarpingState disabled;
    return slopeWarping ? slopeWarping->State() : disabled;
}

void SkeletalPlayback::SetPoseDriver(std::shared_ptr<const PoseDriverSet> set)
{
    if (!set)
    {
        poseDriver.reset();
        return;
    }
    // 绑定期解析关节并预解 RBF 系数；失败直接抛，候选播放器由上层整体丢弃。
    poseDriver = std::make_shared<const PoseDriverSolver>(asset->mesh, std::move(set));
}

PoseDriverFrame SkeletalPlayback::SamplePoseDriver() const
{
    if (!poseDriver) return {};
    // PostProcess 在基础 locomotion 与斜坡修正之后执行；足 IK 不在 UE PostProcess 之前，
    // 所以这里刻意不使用 footPlacementSource。
    const LocalPose& base = slopeWarpedLocals.empty() ? displayedLocals : slopeWarpedLocals;
    if (base.empty()) return {};
    return poseDriver->Solve(base);
}

void SkeletalPlayback::SetPostProcess(std::shared_ptr<const PostProcessAsset> asset)
{
    if (!asset)
    {
        postProcess.reset();
        footPlacementLocals.clear();
        return;
    }
    // PostProcess 按目标网格解析驱动骨/目标骨；缺骨只降级该条目，不使构造失败。
    postProcess = std::make_shared<const PostProcessSolver>(this->asset->mesh, std::move(asset));
}

void SkeletalPlayback::ApplyPostProcess(float deltaSeconds)
{
    if (!postProcess || !postProcess->Valid() || deltaSeconds <= 0 || explicitlyPaused ||
        (!playing && !Completed()))
        return;
    // 依次回退：足 IK 结果 → 斜坡修正 → 基础姿态。PostProcess 是最后一道姿态修正，
    // 因此不能从 displayedLocals 重新求值，否则会丢掉足 IK。
    // footPlacementSource==preIkModels 是足 IK 结果的唯一新鲜度标记；Tick/Reset 会换掉
    // preIkModels，旧 IK 局部姿态随之失效。
    const bool hasFootIk = !footPlacementLocals.empty() && footPlacementSource &&
        footPlacementSource == preIkModels;
    const LocalPose& base = hasFootIk ? footPlacementLocals :
        (!slopeWarpedLocals.empty() ? slopeWarpedLocals : displayedLocals);
    if (base.empty()) return;
    LocalPose corrected = base;
    if (!postProcess->Apply(corrected, &postProcessStats)) return;
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
    auto next = Evaluate(corrected, discontinuity, models);
    poses.swap(next);
    jointModels.swap(models);
    ++postProcessApplications;
    // preIkModels 保持“足 IK 前”的语义，下一帧 FootPlacement 仍以它为求解源。
}

void SkeletalPlayback::CopyPoseGlobals(const Eigen::Matrix4f& component,
    std::vector<Eigen::Matrix4f>& globals) const
{
    // 与 PostProcess 使用同一份“最终基础姿态”。
    const bool hasFootIk = !footPlacementLocals.empty() && footPlacementSource &&
        footPlacementSource == preIkModels;
    const LocalPose& base = hasFootIk ? footPlacementLocals :
        (!slopeWarpedLocals.empty() ? slopeWarpedLocals : displayedLocals);
    if (base.empty())
    {
        globals.clear();
        return;
    }
    globals = LocalToModels(asset->mesh, base);
    for (auto& value : globals)
        value = component * value;
}

void SkeletalPlayback::ApplyPoseGlobals(const Eigen::Matrix4f& component,
    const std::vector<Eigen::Matrix4f>& globals)
{
    if (globals.size() != asset->mesh.joints.size() || globals.empty())
        return;
    const bool hasFootIk = !footPlacementLocals.empty() && footPlacementSource &&
        footPlacementSource == preIkModels;
    LocalPose base = hasFootIk ? footPlacementLocals :
        (!slopeWarpedLocals.empty() ? slopeWarpedLocals : displayedLocals);
    if (base.empty()) return;

    const Eigen::Matrix4f inverseComponent = component.inverse();
    std::vector<Eigen::Matrix4f> models(globals.size());
    for (size_t joint = 0; joint < globals.size(); ++joint)
        models[joint] = inverseComponent * globals[joint];
    for (size_t joint = 0; joint < models.size(); ++joint)
    {
        const int parent = asset->mesh.joints[joint].parent;
        const Eigen::Matrix4f parentModel = parent >= 0 ?
            models[static_cast<size_t>(parent)] : Eigen::Matrix4f::Identity();
        const Eigen::Matrix4f local = parentModel.inverse() * models[joint];
        const size_t lane = joint % 4;
        const auto& translation = local.block<3, 1>(0, 3);
        auto& value = base[joint / 4].translation;
        float lanes[4];
        ozz::math::StorePtrU(value.x, lanes);
        lanes[lane] = translation.x();
        value.x = ozz::math::simd_float4::LoadPtrU(lanes);
        ozz::math::StorePtrU(value.y, lanes);
        lanes[lane] = translation.y();
        value.y = ozz::math::simd_float4::LoadPtrU(lanes);
        ozz::math::StorePtrU(value.z, lanes);
        lanes[lane] = translation.z();
        value.z = ozz::math::simd_float4::LoadPtrU(lanes);

        const Eigen::Quaternionf rotation(local.block<3, 3>(0, 0));
        auto& quaternion = base[joint / 4].rotation;
        ozz::math::StorePtrU(quaternion.x, lanes);
        lanes[lane] = rotation.x();
        quaternion.x = ozz::math::simd_float4::LoadPtrU(lanes);
        ozz::math::StorePtrU(quaternion.y, lanes);
        lanes[lane] = rotation.y();
        quaternion.y = ozz::math::simd_float4::LoadPtrU(lanes);
        ozz::math::StorePtrU(quaternion.z, lanes);
        lanes[lane] = rotation.z();
        quaternion.z = ozz::math::simd_float4::LoadPtrU(lanes);
        ozz::math::StorePtrU(quaternion.w, lanes);
        lanes[lane] = rotation.w();
        quaternion.w = ozz::math::simd_float4::LoadPtrU(lanes);
    }
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> modelsOut;
    auto next = Evaluate(base, discontinuity, modelsOut);
    poses.swap(next);
    jointModels.swap(modelsOut);
}

void SkeletalPlayback::ResetFootPlacement(const char* reason)
{
    if (!footPlacement) return;
    // 重建纯 pre-IK palette 后才提交，旧不可变姿态仍供已发布快照使用。
    if (jointModels != preIkModels)
    {
        std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
        auto restored = Evaluate(!slopeWarpedLocals.empty() ? slopeWarpedLocals :
            visualLocals.empty() ? displayedLocals : visualLocals, discontinuity, models);
        poses.swap(restored);
        jointModels = preIkModels;
    }
    footPlacement->Reset(reason);
    footPlacementSource.reset();
}

void SkeletalPlayback::ApplyFootPlacement(const Eigen::Matrix4f& component, bool grounded,
    const std::string& owner, float deltaSeconds, const GroundProbe& probe)
{
    if (!footPlacement || !footPlacement->State().enabled || deltaSeconds <= 0 || explicitlyPaused ||
        (!playing && !Completed()) ||
        (footPlacementSource == preIkModels && !Completed())) return;
    if (!inPlace)
    {
        ResetFootPlacement("root-motion");
        return;
    }
    // 脚锁状态与求解后的各 section 姿态先在候选上完成；任何异常不污染活动反馈。
    auto candidate = *footPlacement;
    const auto solved = candidate.Solve(asset->mesh,
        !slopeWarpedLocals.empty() ? slopeWarpedLocals :
            visualLocals.empty() ? displayedLocals : visualLocals, *preIkModels,
        component, grounded, owner, deltaSeconds, probe);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
    auto corrected = Evaluate(solved, discontinuity, models);
    candidate.Measure(*models, component);
    poses.swap(corrected);
    jointModels.swap(models);
    *footPlacement = std::move(candidate);
    footPlacementSource = preIkModels;
    footPlacementLocals = solved;
}
}
