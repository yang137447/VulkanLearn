#pragma once

#include "animationAssets.h"
#include "animationSyncGroup.h"
#include "animationRetarget.h"
#include "animationMontage.h"
#include "animationSkeleton.h"
#include "motionWarping.h"
#include "skinPose.h"
#include "footPlacement.h"
#include "slopeWarping.h"
#include "poseDriver.h"
#include "postProcess.h"
#include "inertialization.h"
#include "rootOffset.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <cstdint>

namespace VL::Animation
{
struct SkeletalAsset
{
    SkeletonMesh mesh;
    // 可选 Skeleton 资产；id 为空表示旧资源没有 SK，行为与引入 SK 前一致。
    // 文件摘要保存在 skeleton.sourceDigest（无 SK 时为空），不复制第二份身份。
    SkeletonAsset skeleton;
    std::vector<Clip> clips;
    // SM skinning.montages 导入的动作编排数据；只按值保存数据，不持有播放器/notify 回调。
    // 后续 catalog/C# 播放按 id 或 Montage 资源路径查找，不复制第二份序列数据。
    std::vector<Montage> montages;
    // 正式加载时捕获源字节身份；MM 绑定统计前核对同一代模型和片段。
    std::vector<std::pair<std::string, std::string>> sourceInputs;
    // 可选的动画源资产。目标资产拥有最终渲染 mesh；动作源只提供 clips、
    // montages 和采样骨架，避免把 source 骨架冒充成 target 骨架。
    std::shared_ptr<const SkeletalAsset> animationSource;
    std::shared_ptr<const RetargetProfile> retargetProfile;

    bool HasSkeleton() const { return !skeleton.id.empty(); }
    const SkeletalAsset& AnimationAsset() const
    {
        return animationSource ? *animationSource : *this;
    }
    bool IsRetargeted() const { return animationSource != nullptr; }
    // 按 Montage id 或资源相对路径（Montage::sourcePath）查找；未命中返回 nullptr。
    const Montage* FindMontage(std::string_view idOrPath) const;
};

// 播放期触发的一条 notify 事件。时间单位秒、clip-local，与 Time()/SequenceData 同一时间轴：
//   time       事件实际发生时刻（state 的 end 会因播放范围结束/中断而早于作者 EndTime）
//   startTime  作者原区间起点；duration 为作者原区间长度（不按片段或播放范围裁剪）
//   phase      "instant"（瞬时 notify）/"begin"/"end"（notifyState）
//   interrupted 只有 end 有意义：true 表示没有走到作者 EndTime（Stop/切片段/回绕/播放范围结束）
struct FiredNotify
{
    std::string name;
    std::string track;
    std::string kind;
    std::string phase;
    float time = 0;
    float startTime = 0;
    float duration = 0;
    bool interrupted = false;
    // 多 AnimSegment Montage 的作者段；单 sequence/普通片段为空、0。
    int segmentIndex = 0;
    std::string segmentId;
};

// 调试读数：当前仍在区间内的 notifyState。endTime 是作者原值，可能越过播放范围末尾；
// 不返回 warp/modifier 数据，执行层只消费事件名与区间。
struct ActiveNotifyState
{
    std::string name;
    std::string track;
    float startTime = 0;
    float endTime = 0;
};

// 单实例唯一原生时钟；同 actor 的各 section 共享播放器，RT 只读冻结的 SkinPose。
class SkeletalPlayback
{
public:
    explicit SkeletalPlayback(std::shared_ptr<const SkeletalAsset> asset);
    // Mesh 变换、胶囊中心与 Actor 世界 yaw 只服务原生 MotionWarping；缺省时保持纯片段
    // 根位移/根 yaw。actorYaw 是弧度、绕 VL Y-up 的世界朝向，用于把 target 世界旋转
    // 换成根 yaw 增量；没有旋转 target 的窗口不消费它。
    void Tick(float deltaSeconds, const Eigen::Matrix4f* meshTransform = nullptr,
        const Eigen::Vector3f* actorPosition = nullptr, float actorYaw = 0.0f);
    void Seek(float seconds);
    void Select(size_t clipIndex);
    void Play(const std::string& clipId, bool loop, float rate, bool inPlace);
    // 可选的 Montage notify 事件源：与当前片段 Sequence notifies 合并成一张按 time 稳定排序的
    // 事件表（同刻事件保持“先 Sequence 后 Montage”的作者顺序）。Play/Select/TransitionTo
    // 切换播放段时清空，避免旧 Montage 的 notifyState 残留到普通片段。
    // id/length 只服务 feedback 的 montage 读数；缺省表示调用方没有 Montage 身份。
    void SetMontageNotifies(std::vector<MontageNotify> notifies,
        std::string id = {}, float length = 0);
    // 当前播放段所属的 Montage 身份（feedback 用）；普通片段为空、length 为 0。
    const std::string& MontageId() const { return montageId; }
    float MontageLength() const { return montageLength; }
    // 多 section Montage 的 section 链。装配时把播放范围设为 section 0 的
    // [max(startTime,entryStartTime),endTime] 并停在 section 0；段末尾按 nextSection/linkMethod
    // 切换。只允许一次性播放（PlayMontage 已拒绝 loop），空数组/循环播放抛异常。
    // entryStartTime 是本次播放的入口时间（秒、clip-local、必须已由调用方校验落在 section 0 内）；
    // 缺省表示从 section 0 起点进入。入口只截短第一段的范围，后续段仍按链的
    // startTime/endTime 推进，时钟由调用方随后 Seek(entryStartTime) 对齐。
    // 切段语义（UE FAnimMontageInstance::Advance，AnimMontage.cpp:2715-2733 直接改
    // Position 并在下一 sub-step 继续，最多 10 次，行 2575）：
    //   - 时间轴连续时（目标起点 == 段末尾）等价连续播放，活动 notifyState 跨段继续；
    //   - 起始时间向后跳（循环回前段）时在段末尾补 end(interrupted)，再扫描新段头部，
    //     与既有循环回绕语义一致；
    //   - 向前跳过一段区间时不重放被跳过的事件；仍活动的 notifyState 若结束点落在
    //     被跳过的区间，会在新段第一次扫描时补 end（时间记为扫描起点），不会卡死。
    void SetMontageSections(std::vector<MontageSection> sections,
        std::optional<float> entryStartTime = std::nullopt);
    // 多 AnimSegment Montage 的时间轴映射。所有段必须已绑定 derivedClipId，
    // 且 timeline 连续覆盖 [0,length]；安装后 selected 随段切换，但 Montage 时钟
    // 不重置。空数组恢复单 sequence 播放。
    void SetMontageSegments(std::vector<MontageSegment> segments, float length);
    // 当前 section 下标/名字；没有 section 链时下标为 0、名字为空。
    // 仅供诊断/测试，不参与时钟或姿态求值。
    size_t MontageSectionIndex() const { return montageSection; }
    std::string MontageSectionName() const;
    size_t MontageSegmentIndex() const { return montageSegment; }
    std::string MontageSegmentId() const;
    // 播放范围 [start,end]（秒、clip-local）。默认 0..clip duration；只影响播放长度、
    // 根位移积分与 notify 有效域，不改变姿态采样。end 超出片段时长 1e-3 以内按片段末尾
    // clamp，超出更多则拒绝；非 0 起点不移动当前时钟，调用方应随后 Seek(start) 对齐
    // （Seek 不补发区间内历史事件）。直接改范围会清空 section 链（范围是播放段身份）；
    // PlayMontage 是 Montage 范围的唯一装配入口。
    void SetPlaybackRange(float start, float end);
    float PlaybackRangeStart() const { return playbackRangeStart; }
    float PlaybackRangeEnd() const;
    // 停止推进并结束活动 notifyState（interrupted=true），保留时钟与当前姿态；
    // 与 SetPlaying(false) 的暂停不同：暂停保留活动区间，恢复播放后继续。
    void Stop();
    // 本 tick 触发的事件；下一次 Tick 开始时重建。Play/Stop/Select/TransitionTo 补发的
    // end 先写入本缓冲并留存到下一 Tick，保证控制面事件不会被紧随其后的 Tick 丢掉。
    const std::vector<FiredNotify>& FiredNotifies() const { return firedNotifies; }
    // 调试读数；Tick 与每次 enter/leave 后重建，顺序与 notify 表（按 time）一致。
    const std::vector<ActiveNotifyState>& ActiveNotifyStates() const { return activeNotifyStates; }
    // 通用 modifier 队列由原生拥有；C# 只提交目标与窗口，播放器消费修改后的根位移。
    // 所有 traversal（Hurdle/Climb/Mantle/Vault）共用这条消费路径，CLR 不做 warp 数学。
    void SetMotionWarpWindows(std::vector<MotionWarpWindow> windows);
    void ClearMotionWarp();
    const std::string& MotionWarpState() const { return motionWarpState; }
    // Motion Matching 只交付目标帧；原生拥有唯一目标时钟和有界交叉混合源。
    void TransitionTo(const std::string& clipId, float seconds, bool loop,
        float blendSeconds, bool inPlace = true);
    // UE SyncGroup transition：目标剪辑必须声明同名 SyncGroup。syncPhase 是 [0,1]
    // 的归一化时间；目标为循环片段时映射到 phase*duration，非循环时 clamp 到末帧前。
    void TransitionToSynced(const std::string& clipId, float seconds, bool loop,
        float blendSeconds, bool inPlace, const std::string& syncGroup, float syncPhase);
    // UE BlendSpace1D 的运行时：A 仍是唯一推进时钟的片段（根位移/notify 仍属于它），
    // B 只按 A 的归一化相位采样后与 A 做加权局部姿态混合。clipA 必须与当前选中
    // 片段一致；权重在 [0,1] 内夹紧，0 = 纯 A，1 = 纯 B。切片段/Seek/Montage 会清除它。
    void SetBlendSpace1D(const std::string& clipA, const std::string& clipB, float weight);
    void ClearBlendSpace();
    bool BlendSpaceActive() const { return blendSpaceActive; }
    float BlendSpaceWeight() const { return blendSpaceActive ? blendSpaceWeight : 0.0f; }
    std::string BlendSpaceClipId() const;
    // 只调节唯一目标时钟，不重采样或重启混合；混合时长仍按实际 delta 推进。
    void SetRate(float speed);
    // 模式只作用于后续过渡；切换不重采样当前姿态或重启唯一时钟。
    void SetInertializationEnabled(bool enabled) { inertializationEnabled = enabled; }
    bool InertializationEnabled() const { return inertializationEnabled; }
    void SetRootOffsetEnabled(bool enabled);
    bool RootOffsetEnabled() const { return rootOffset.State().enabled; }
    void ApplyVisualRoot(const Eigen::Matrix4f& component, const Eigen::Vector3f& achievedVelocity,
        float deltaSeconds);
    void ResetRootOffset(const char* reason);
    const RootOffsetState& RootOffset() const { return rootOffset.State(); }
    // 显式启用时验证腿链；组件刚性正 uniform scale 在应用入口验证。
    void SetFootPlacementEnabled(bool enabled);
    void SetFootPlacementLockType(FootLockType type);
    FootLockType FootPlacementLockType() const { return footLockType; }
    void ResetFootPlacement(const char* reason);
    void ApplyFootPlacement(const Eigen::Matrix4f& component, bool grounded,
        const std::string& owner, float deltaSeconds, const GroundProbe& probe);
    const FootPlacementState& FootPlacement() const;
    // Echo_SlopeWarping_CtrlRig 的正式原生替代。与 FootPlacement 共用本地 TRS
    // 姿态阶段，但只写视觉姿态：Actor 位置、胶囊与 rootMotionDelta 不变。
    void SetSlopeWarpingEnabled(bool enabled);
    bool SlopeWarpingEnabled() const;
    void SetSlopeWarpingParameters(const SlopeWarpingParameters& parameters);
    const SlopeWarpingParameters& SlopeWarpingSettings() const { return slopeWarpingSettings; }
    void ResetSlopeWarping(const char* reason);
    void ApplySlopeWarping(const Eigen::Matrix4f& component, bool grounded,
        const std::string& owner, float deltaSeconds, const GroundProbe& probe);
    const SlopeWarpingState& SlopeWarping() const;
    // UE PostProcess AnimBP 的 PoseDriver/DriveCurves 阶段：读取本帧最终基础姿态
    // （斜坡修正之后、足 IK 之前），产出只读命名曲线。曲线不改变姿态，也不写 Actor。
    // 传 null 关闭；非法资产抛异常，调用方必须先在候选播放器上应用。
    void SetPoseDriver(std::shared_ptr<const PoseDriverSet> set);
    void ClearPoseDriver() { poseDriver.reset(); }
    bool PoseDriverEnabled() const { return poseDriver != nullptr; }
    const PoseDriverSet* PoseDriverAsset() const { return poseDriver ? poseDriver->Set().get() : nullptr; }
    // 每帧按需求值一次；不缓存，避免与播放相位/斜坡修正的提交顺序耦合。
    PoseDriverFrame SamplePoseDriver() const;
    // UE PostProcess AnimBP 的 Twist/Helpers 阶段：当前实现 Echo_Twist_CtrlRig 的
    // 正式原生替代（bind→current 相对旋转 → swing/twist → 写 twist 骨）。
    // 在足 IK 之后、蒙皮快照之前执行；不改 Actor/Jolt，也不改 locomotion 时钟。
    void SetPostProcess(std::shared_ptr<const PostProcessAsset> asset);
    void ClearPostProcess() { postProcess.reset(); footPlacementLocals.clear(); }
    bool PostProcessEnabled() const { return postProcess && postProcess->Valid(); }
    // 避免成员函数名与类型名同名：类作用域内同名会遮蔽 PostProcessAsset 类型。
    const PostProcessAsset* PostProcess() const
    {
        return postProcess ? postProcess->Asset().get() : nullptr;
    }
    const std::vector<PostProcessDiagnostic>& PostProcessDiagnostics() const
    {
        static const std::vector<PostProcessDiagnostic> none;
        return postProcess ? postProcess->BindDiagnostics() : none;
    }
    void ApplyPostProcess(float deltaSeconds);
    // 只用于反馈/验收：成功写入过姿态的累计帧数。
    uint64_t PostProcessApplications() const { return postProcessApplications; }
    const PostProcessApplyStats& PostProcessStats() const { return postProcessStats; }
    // 骨骼物理桥：World 用外部 Jolt 求解器改写“最终姿态”。
    // CopyPoseGlobals 取当前最终局部姿态的世界全局矩阵（cm，已乘 component）；
    // ApplyPoseGlobals 把修改后的全局矩阵写回局部姿态并重新发布。
    void CopyPoseGlobals(const Eigen::Matrix4f& component,
        std::vector<Eigen::Matrix4f>& globals) const;
    void ApplyPoseGlobals(const Eigen::Matrix4f& component,
        const std::vector<Eigen::Matrix4f>& globals);
    std::shared_ptr<SkeletalPlayback> NewInstance() const;
    void SetPlaying(bool value) { playing = value; explicitlyPaused = !value; }
    bool IsPlaying() const { return playing; }
    bool IsExplicitlyPaused() const { return explicitlyPaused; }
    float Time() const { return time; }
    float Duration() const;
    // 只读曲线采样：name 大小写不敏感，seconds 与 Time() 同为 clip-local 秒。
    // 未知曲线（包括没有 sidecar 的片段）返回 nullopt；非有限时间抛 invalid_argument。
    // 该入口不自动执行曲线、不写入反馈，也不把全部曲线复制到每帧快照。
    std::optional<float> SampleCurve(std::string_view name, float seconds) const;
    // 使用当前播放时钟的快捷入口；其余语义与显式时间重载一致。
    std::optional<float> SampleCurve(std::string_view name) const;
    // 编辑器播放时间轴以当前播放范围为零点；原生 Time() 仍保持 clip-local 绝对时间，
    // 以便 notify、warp 与跨 section 链继续共用同一条资产时间轴。
    float PlaybackPosition() const
    {
        return std::max(0.0f, Time() - PlaybackRangeStart());
    }
    size_t SelectedClip() const { return selected; }
    // 调试读数用只读片段身份；与索引同源，不暴露可变资产。
    const std::string& SelectedClipId() const
    {
        return asset->AnimationAsset().clips.at(selected).id;
    }
    // 根轨道上一播放 tick 的模型空间位移（cm）；Climb 等有明确根轨迹的动作消费。
    const Eigen::Vector3f& RootMotionDelta() const { return rootMotionDelta; }
    Eigen::Vector3f RootMotionTotal() const;
    Eigen::Vector3f RootMotionRemaining() const;
    // 根轨道上一 tick 的 yaw 增量（弧度）。有旋转 warp 窗口时是 warp 后的值，
    // 否则是原始根旋转；C# 在位移之后单独加到 Actor yaw。
    float RootMotionYawDelta() const { return rootMotionYawDelta; }
    bool Completed() const { return !looping && time >= PlaybackRangeEnd() && !IsBlending(); }
    bool IsBlending() const { return !blendSource.empty() || !inertialization.Empty(); }
    float BlendAlpha() const { return IsBlending() ? blendElapsed / blendDuration : 1.0f; }
    bool InPlace() const { return inPlace; }
    bool IsLooping() const { return looping; }
    float Rate() const { return rate; }
    // 外部作者/调用方请求的速率；SyncGroup follower 的生效速率可能被时长比重算。
    float RequestedRate() const { return requestedRate; }
    // 当前片段名下的同步组相位（clip-local time / duration）；无组时仍可作诊断。
    float SyncPhase() const;
    const std::string& SyncGroup() const { return activeSyncGroup; }
    // 最近一个已越过的 sync marker 与其到下一个 marker 的 alpha；无 marker 返回空。
    const std::string& SyncMarkerName() const { return syncMarkerName; }
    float SyncMarkerAlpha() const { return syncMarkerAlpha; }
    // 当前片段的 Sequence Player 绑定；activeSyncGroup 非空时优先匹配同名组，
    // 否则返回第一个可执行同步的绑定（DoNotSync 只作为回退诊断）。
    const PlayerBinding* SyncBinding() const;
    // 过渡源片段（blendSource）在混合期间保留一份只用于同步的影子相位时钟；
    // 它不是第二个姿态播放器，也不参与采样/notify。
    const PlayerBinding* SyncSourceBinding() const;
    bool HasSyncSourceClock() const { return syncSourceValid; }
    const std::vector<SyncMarker>* SyncSourceMarkers() const;
    // 由 World 在唯一播放器 Tick 之前装配；identity/ownerKey/localKey 只被读取，
    // source=true 时返回过渡源的只读影子节点（canApply=false）。
    SyncGroupNode BuildSyncGroupNode(std::string_view identity, std::string_view ownerKey,
        std::string_view localKey, bool source);
    // 写回本 tick 的选举结果。follower 的时间/速率在 Tick 开头应用一次；
    // leader/DoNotSync 只更新反馈，并以 requestedRate 继续推进。
    void ApplySyncGroupResult(const SyncGroupNodeResult& result, std::string_view leaderIdentity);
    bool SyncDecisionValid() const { return syncDecisionValid; }
    bool SyncActive() const;
    const std::string& SyncLeader() const { return syncLeader; }
    SyncRole CurrentSyncRole() const { return syncDecision.role; }
    SyncMethod CurrentSyncMethod() const { return syncDecision.effectiveMethod; }
    SyncMethod CurrentSyncAuthoredMethod() const { return syncDecision.authoredMethod; }
    SyncGroupScope CurrentSyncScope() const { return syncDecision.scope; }
    const char* CurrentSyncRoleName() const { return VL::Animation::SyncRoleName(syncDecision.role); }
    const char* CurrentSyncMethodName() const
    {
        return VL::Animation::SyncMethodName(syncDecision.effectiveMethod);
    }
    const char* CurrentSyncAuthoredMethodName() const
    {
        return VL::Animation::SyncMethodName(syncDecision.authoredMethod);
    }
    const char* CurrentSyncScopeName() const
    {
        return VL::Animation::SyncGroupScopeName(syncDecision.scope);
    }
    const char* SyncDiagnosticName() const
    {
        return VL::Animation::SyncGroupDiagnosticName(syncDecision.diagnostic);
    }
    const SkeletalAsset& Asset() const { return *asset; }
    const std::vector<Eigen::Matrix4f>& Models() const { return *jointModels; }
    // MM 使用规范 post-blend 姿态；pre-IK 另含可选视觉根修正，不反馈给搜索。
    const std::vector<Eigen::Matrix4f>& MotionModels() const { return *motionModels; }
    const std::vector<Eigen::Matrix4f>& PreIkModels() const { return *preIkModels; }
    const std::shared_ptr<const SkinPose>& Pose(size_t section) const { return poses.at(section); }

private:
    // 归一后的 notify 事件表：Sequence notify 没有 kind 字段，duration==0 视为瞬时 notify，
    // duration>0 视为 notifyState；Montage notify 保留显式 kind，但只有 duration>0 的
    // notifyState 才按区间执行（零长度区间没有可执行的 begin/end，按瞬时处理）。
    struct NotifyEvent
    {
        std::string name;
        std::string track;
        std::string kind;
        float time = 0;
        float duration = 0;
        int segmentIndex = 0;
        std::string segmentId;
        float EndTime() const { return time + duration; }
        bool IsState() const { return kind == "notifyState" && duration > 0; }
    };
    struct MontageSample
    {
        size_t segment = 0;
        size_t clip = 0;
        float clipTime = 0;
    };
    std::vector<std::shared_ptr<const SkinPose>> Evaluate(
        const LocalPose& locals, uint64_t epoch,
        std::shared_ptr<const std::vector<Eigen::Matrix4f>>& models) const;
    LocalPose Sample(size_t clipIndex, float sampleTime) const;
    LocalPose PreviewPose(float aheadSeconds) const;
    float ClipDuration() const;
    // 由当前片段 Sequence notifies 与可选 Montage notifies 合成按 time 稳定排序的事件表；
    // 只重建数据，不产生事件。切换片段/更换 Montage 源时调用。
    void RebuildNotifyEvents();
    void RefreshActiveNotifyStates();
    // 扫描一个已按播放范围裁剪的区间。begin/instant 用 [from,to) 半开（正好落在 to 的
    // 事件留到下一 tick，避免同一 tick 重复触发）；end 含上界（到达区间末尾当 tick 结束）。
    // includeTerminalEnd 只用于一次性播放到达范围末尾的那一 tick：瞬时 notify 允许 time==to。
    void ScanNotifySegment(double from, double to, bool includeTerminalEnd);
    // 结束所有活动区间；interrupted=true 时标记中断。deferToNextTick=true 用于控制面
    // （Stop/切片段/更换 notify 源），让事件在紧随其后的 Tick 重建后仍可被反馈读到。
    void EndActiveNotifyStates(float atTime, bool interrupted, bool deferToNextTick);
    void PushFired(const NotifyEvent& notify, const char* phase, float eventTime,
        bool interrupted);
    // 不改变 section 链的范围写入；公开 SetPlaybackRange 以及切段内部使用。
    void SetPlaybackRangeInternal(float start, float end);
    // Montage 时间轴 -> 段/clip 本地时间的唯一换算入口。atEnd 用于段末采样：
    // 时间恰好落在边界时优先上一段，避免无 remaining 的 tick 提前切 clip。
    MontageSample SampleMontageTimeline(float timeline, bool atEnd = false) const;
    float MontageSourceTime(const MontageSegment& segment, float timeline,
        float clipDuration) const;
    // 多段根轨迹按实际移动的子区间积分；跳段本身不计位移。
    Eigen::Vector3f MontageRootDelta(float from, float to) const;
    void ClearMontageSegments();
    // 当前 section 的 nextSection 目标下标；空/未命中返回 -1（未命中的拒绝发生在
    // PlayMontage 装配期，运行期只做防御性停止）。
    int MontageNextSectionIndex() const;
    // 多 section 播放推进：逐段累积根运动（跳跃不计入），段边界按半开区间扫描 notify，
    // 段末尾按链接切换范围；姿态只在推进结束后采样/发布一次。
    void TickMontageSections(float deltaSeconds, const Eigen::Matrix4f* meshTransform,
        const Eigen::Vector3f* actorPosition, float actorYaw);
    // 多 AnimSegment 版本：section 边界仍是 Montage 时间轴边界，段边界只替换
    // 正在采样的 clip；每段分别映射 warp 窗口与根轨迹。
    void TickSegmentedMontage(float deltaSeconds, const Eigen::Matrix4f* meshTransform,
        const Eigen::Vector3f* actorPosition, float actorYaw);
    Eigen::Vector3f ApplySegmentedMontageWarp(const MontageSegment& segment,
        const Clip& clip, float sourceFrom, float sourceTo,
        const Eigen::Vector3f& rawDelta, const Eigen::Matrix4f& meshTransform,
        const Eigen::Vector3f& actorPosition, float actorYaw, float& yawDelta) const;
    std::shared_ptr<const SkeletalAsset> asset;
    std::shared_ptr<const CompiledRetarget> compiledRetarget;
    std::vector<std::shared_ptr<const SkinPose>> poses;
    // 与 palette 同批求值/提交，供原生 Pose History 使用，不把可写模型矩阵传给 RT。
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> jointModels;
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> preIkModels;
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> motionModels;
    // 每个片段实例一份窗口队列；窗口时间与片段本地时钟同源，切片段时由提交方覆盖。
    MotionWarpRuntime motionWarp;
    std::string motionWarpState;
    std::optional<FootPlacementSolver> footPlacement;
    FootLockType footLockType = FootLockType::Auto;
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> footPlacementSource;
    std::optional<SlopeWarpingSolver> slopeWarping;
    SlopeWarpingParameters slopeWarpingSettings;
    // 不可变求解器可在候选播放器之间廉价复制；资产身份随 solver 一起共享。
    std::shared_ptr<const PoseDriverSolver> poseDriver;
    std::shared_ptr<const PostProcessSolver> postProcess;
    // 足 IK 后的局部姿态；PostProcess 必须基于它，否则 Evaluate 会把 IK 结果覆盖掉。
    LocalPose footPlacementLocals;
    uint64_t postProcessApplications = 0;
    PostProcessApplyStats postProcessStats;
    // 斜坡修正后的局部姿态（足 IK 前）；FootPlacement 以它为求解源。
    LocalPose slopeWarpedLocals;
    LocalPose displayedLocals;
    // displayedLocals 是规范混合结果；visualLocals 独立保存根修正后、足 IK 前的求解源。
    LocalPose visualLocals;
    RootOffsetSolver rootOffset;
    FiniteInertialization inertialization;
    bool inertializationEnabled = false;
    // 中断时只冻结当前显示姿态，不保留逐层增长的旧播放器链。
    LocalPose blendSource;
    float blendDuration = 0;
    float blendElapsed = 0;
    // BlendSpace1D 的第二采样片段；不是第二个播放器，没有自己的时钟/notify/根轨迹。
    bool blendSpaceActive = false;
    size_t blendSpaceClip = 0;
    float blendSpaceWeight = 0;
    size_t selected = 0;
    float time = 0;
    Eigen::Vector3f rootMotionDelta = Eigen::Vector3f::Zero();
    float rootMotionYawDelta = 0;
    bool playing = true;
    // 自动到端点仍须响应正 delta 的接地退出；Console 显式暂停则连 IK 一并冻结。
    bool explicitlyPaused = false;
    bool looping = true;
    bool inPlace = false;
    float rate = 1;
    float requestedRate = 1;
    std::string activeSyncGroup;
    std::string syncMarkerName;
    float syncMarkerAlpha = 0;
    // 过渡源影子时钟：只追踪相位/速率，混合结束或硬切时清空。
    bool syncSourceValid = false;
    size_t syncSourceSelected = 0;
    float syncSourceTime = 0;
    float syncSourceRangeStart = 0;
    float syncSourceRangeEnd = 0;
    float syncSourceRate = 1;
    bool syncSourceLooping = true;
    // 本 tick 的组决策与待应用对齐；都是值类型/单字符串，不建逐帧容器。
    bool syncDecisionValid = false;
    SyncGroupNodeResult syncDecision;
    std::string syncLeader;
    bool syncPendingValid = false;
    bool syncPendingHasTime = false;
    float syncPendingTime = 0;
    float syncPendingRate = 1;
    void ClearSyncSourceClock();
    void AdvanceSyncSourceClock(float deltaSeconds);
    void RefreshSyncMarker();
    uint64_t discontinuity = 0;
    uint64_t playbackIdentity = 0;
    // Montage notify 覆盖层；与 Sequence notifies 合并执行，切片段时清空。
    std::vector<MontageNotify> montageNotifies;
    // 与 montageNotifies/clip.sequence.notifies 同步的只读事件表；notifyActive 与它同下标，
    // 一个作者 notify 一个状态，同名 notify 互不影响。
    std::vector<NotifyEvent> notifyEvents;
    std::vector<uint8_t> notifyActive;
    std::vector<FiredNotify> firedNotifies;
    // 控制面补发的 end 先留在这里，下一次 Tick 重建事件缓冲时并入，避免被同帧 Tick 清掉。
    std::vector<FiredNotify> pendingNotifies;
    std::vector<ActiveNotifyState> activeNotifyStates;
    // 播放范围（秒、clip-local）。end==0 表示未设置，范围末尾跟随当前片段时长。
    float playbackRangeStart = 0;
    float playbackRangeEnd = 0;
    // 当前播放段的 Montage 身份；普通 Play/Select/TransitionTo 清空，Montage 播放由
    // SetMontageNotifies 写入。只做 feedback 读数，不参与时钟或姿态求值。
    std::string montageId;
    float montageLength = 0;
    // Montage section 链（空 = 当前播放段不是链式 Montage）；Play/Select/TransitionTo/
    // SetPlaybackRange 都会清空。与 montageNotifies 同属一次 Montage 装配，由 PlayMontage
    // 在候选播放器上一起写入。
    std::vector<MontageSection> montageSections;
    size_t montageSection = 0;
    // 多 AnimSegment 数据与已解析 clip 下标；与 section 链相互独立。
    std::vector<MontageSegment> montageSegments;
    std::vector<size_t> montageSegmentClips;
    size_t montageSegment = 0;
    float montageTimelineLength = 0;
};

// 与 shader 共用全部 CSR 权重的 CPU 参考；边界来自当前变形顶点，不沿用绑定姿态。
Eigen::Matrix4f BlendSkinMatrix(const SkinSection& section, size_t vertex,
    const std::vector<Eigen::Matrix4f>& palette);
// 运行期入口：section.jointBindBounds 可用时走 O(joints×8) 的保守包围盒快路径，
// 缺失时退回逐顶点精确参考路径（两者 palette 内容/顺序完全相同）。
// joints 非空时额外填充 SkinPose::bindToPoseMatrices（全骨架、与 section 无关），
// 供跨 section 的碰撞代理/胶囊使用。
std::shared_ptr<const SkinPose> BuildSkinPose(const SkinSection& section,
    const std::vector<Eigen::Matrix4f>& models, uint64_t discontinuity, uint64_t playbackIdentity = 0,
    const std::vector<Joint>* joints = nullptr);
// 逐顶点混合 + 精确 AABB + 逐顶点奇异拒绝的原始实现；仅供测试对照与诊断，
// 不参与运行时每帧路径。
std::shared_ptr<const SkinPose> BuildSkinPoseReference(const SkinSection& section,
    const std::vector<Eigen::Matrix4f>& models, uint64_t discontinuity, uint64_t playbackIdentity = 0,
    const std::vector<Joint>* joints = nullptr);
std::vector<uint32_t> PackSkinInfluences(const SkinSection& section);
}
