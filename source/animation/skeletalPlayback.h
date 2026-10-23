#pragma once

#include "animationAssets.h"
#include "animationRetarget.h"
#include "animationMontage.h"
#include "animationSkeleton.h"
#include "motionWarping.h"
#include "skinPose.h"
#include "footPlacement.h"
#include "inertialization.h"
#include "rootOffset.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
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
    // 当前 section 下标/名字；没有 section 链时下标为 0、名字为空。
    // 仅供诊断/测试，不参与时钟或姿态求值。
    size_t MontageSectionIndex() const { return montageSection; }
    std::string MontageSectionName() const;
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
    std::shared_ptr<SkeletalPlayback> NewInstance() const;
    void SetPlaying(bool value) { playing = value; explicitlyPaused = !value; }
    bool IsPlaying() const { return playing; }
    bool IsExplicitlyPaused() const { return explicitlyPaused; }
    float Time() const { return time; }
    float Duration() const;
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
        float EndTime() const { return time + duration; }
        bool IsState() const { return kind == "notifyState" && duration > 0; }
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
    // 当前 section 的 nextSection 目标下标；空/未命中返回 -1（未命中的拒绝发生在
    // PlayMontage 装配期，运行期只做防御性停止）。
    int MontageNextSectionIndex() const;
    // 多 section 播放推进：逐段累积根运动（跳跃不计入），段边界按半开区间扫描 notify，
    // 段末尾按链接切换范围；姿态只在推进结束后采样/发布一次。
    void TickMontageSections(float deltaSeconds, const Eigen::Matrix4f* meshTransform,
        const Eigen::Vector3f* actorPosition, float actorYaw);
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
};

// 与 shader 共用全部 CSR 权重的 CPU 参考；边界来自当前变形顶点，不沿用绑定姿态。
Eigen::Matrix4f BlendSkinMatrix(const SkinSection& section, size_t vertex,
    const std::vector<Eigen::Matrix4f>& palette);
// 运行期入口：section.jointBindBounds 可用时走 O(joints×8) 的保守包围盒快路径，
// 缺失时退回逐顶点精确参考路径（两者 palette 内容/顺序完全相同）。
std::shared_ptr<const SkinPose> BuildSkinPose(const SkinSection& section,
    const std::vector<Eigen::Matrix4f>& models, uint64_t discontinuity, uint64_t playbackIdentity = 0);
// 逐顶点混合 + 精确 AABB + 逐顶点奇异拒绝的原始实现；仅供测试对照与诊断，
// 不参与运行时每帧路径。
std::shared_ptr<const SkinPose> BuildSkinPoseReference(const SkinSection& section,
    const std::vector<Eigen::Matrix4f>& models, uint64_t discontinuity, uint64_t playbackIdentity = 0);
std::vector<uint32_t> PackSkinInfluences(const SkinSection& section);
}
