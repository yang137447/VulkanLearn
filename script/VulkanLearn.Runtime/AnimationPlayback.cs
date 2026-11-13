using System.Text.Json;
using System.Numerics;

namespace VulkanLearn;

public readonly record struct AnimationState(string Clip, float Time, float Duration, bool Completed, bool Playing,
    float Rate = 1, Vector3 RootMotion = default, float RootMotionYaw = 0,
    Vector3 RootMotionTotal = default, Vector3 RootMotionRemaining = default, string Warp = "",
    AnimationNotify[]? Notifies = null, string Montage = "", float MontageLength = 0,
    Vector2 MontageRange = default, string SyncGroup = "", float SyncPhase = 0,
    string SyncMarker = "", float SyncMarkerAlpha = 0, bool SyncActive = false,
    string SyncLeader = "", string SyncRole = "", string SyncMethod = "",
    string SyncEffectiveMethod = "", string SyncScope = "", string SyncDiagnostic = "",
    int MontageSegment = 0, string MontageSegmentId = "", string BlendClip = "", float BlendWeight = 0,
    string AdditiveClip = "", float AdditiveAlpha = 0, string CachedPose = "",
    string LayerClip = "", float LayerAlpha = 0, int LayerMaskCount = 0);
public readonly record struct AnimationSyncMarker(string Name, float Time);
public readonly record struct AnimationPlayerInfo(string SyncGroup, string SyncRole, string SyncMethod,
    float StartPosition, bool Loop, string SyncGroupScope = "Local");
// 播放期一条 notify 事件；time 是实际发生时刻，startTime/duration 始终是作者原区间。
public readonly record struct AnimationNotify(string Name, string Track, string Kind, string Phase,
    float Time, float StartTime, float Duration, bool Interrupted,
    int SegmentIndex = 0, string SegmentId = "");
public readonly record struct MontageSectionInfo(string Name, float StartTime, float EndTime,
    string NextSection, float NextTime, string LinkMethod);
// warp 字段只做数据透传；执行层支持 None|Bone|Static、Linear 与 Slerp；原生负责
// section 链推进与 staticPose 消费，C# 不再按 section 数/Static 提前拒绝。
public readonly record struct MontageNotifyInfo(string Kind, string Track, string Name, float Time,
    float Duration, bool HasWarp = false, string WarpTarget = "", string WarpProvider = "",
    string WarpBone = "", bool SubtractRemainingRootMotion = false, bool IgnoreZAxis = false,
    bool WarpRotation = false, float RotationTimeMultiplier = 1, string Easing = "",
    string RotationMethod = "Slerp");
// Montage warp target：位置必填，旋转可选（UE FMotionWarpingTarget.Location/Rotation）。
// Rotation 为目标世界朝向（System.Numerics 四元数 x,y,z,w）；null 表示只提交位置，
// 与旧 3 分量调用等价，原生不会把默认单位四元数当成目标朝向。
public readonly record struct WarpTarget(Vector3 Location, Quaternion? Rotation = null);
public readonly record struct MontageWindowInfo(string Target, float Start, float End);
// 多 AnimSegment Montage 的一段；Start/End 是 Montage 时间轴，ClipStart/ClipEnd 是
// sidecar 本地时间。C# 只读 catalog/feedback，不维护第二份段时钟。
public readonly record struct MontageSegmentInfo(int Index, string SegmentId, string SequencePath,
    string ClipId, float StartTime, float EndTime, float ClipStartTime, float ClipEndTime,
    float RateScale, bool Loop);
// catalog 的 Montage 数据；ClipId 由 loader 从 sidecar 的 `sequence` 字段派生，仅运行时诊断/播放。
public readonly record struct MontageInfo(string Id, string SequencePath, string ClipId, float Length,
    string Slot, IReadOnlyList<MontageSectionInfo> Sections, IReadOnlyList<MontageNotifyInfo> Notifies,
    IReadOnlyList<MontageWindowInfo> Windows, float RateScale = 1, string AssetPath = "",
    IReadOnlyList<MontageSegmentInfo>? Segments = null);
public readonly record struct SpeedMatchingState(bool Enabled, bool Active, float SourceSpeed, float ActualSpeed,
    float TargetRate, float Rate, float ResidualSpeed, string Reason);
public readonly record struct FootPlacementFootState(bool Grounded, bool Planted, Vector3 Anchor,
    Vector3 PreSole, Vector3 PostSole, float TargetError, string SupportIdentity,
    Vector3 GroundPoint, Vector3 GroundNormal, Vector3 ProbeOrigin, Vector3 ProbeDisplacement,
    bool HeelGrounded, Vector3 PreHeel, Vector3 PostHeel, Vector3 HeelGroundPoint, Vector3 HeelGroundNormal,
    Vector3 HeelProbeOrigin, Vector3 HeelProbeDisplacement, string HeelSupportIdentity, string ContactMode);
public readonly record struct FootPlacementState(bool Enabled, bool Active, float PelvisOffset,
    FootPlacementFootState Left, FootPlacementFootState Right, string Reason);
public readonly record struct RootOffsetState(bool Enabled, bool Active, Vector3 WorldTranslation,
    float YawRadians, string Reason);
public readonly record struct SlopeWarpingState(bool Enabled, bool Active, float SlopeAngle,
    float TargetSlopeAngle, float AppliedSlopeAngle, float Weight, float PelvisOffset,
    Vector3 GroundNormal, string SupportIdentity, string Reason);
// PoseDriver 诊断只暴露代码与细节；曲线是只读求值结果，不回写姿态或播放器状态。
public readonly record struct PoseDriverDiagnostic(string Driver, string Code, string Detail);
public readonly record struct PoseDriverState(bool Enabled, bool Active, int SolvedDrivers,
    string Asset, string Digest, IReadOnlyDictionary<string, float> Curves,
    IReadOnlyList<PoseDriverDiagnostic> Diagnostics);
// PostProcess（Twist/Helpers）姿态修正的只读反馈；诊断项标识被降级的条目。
public readonly record struct PostProcessDiagnostic(string Entry, string Code, string Detail);
public readonly record struct PostProcessState(bool Enabled, string Asset, string Digest,
    int Entries, int TwistEntries, int HelperEntries, int ConstraintEntries, long Applications,
    int RotatedBones, float MaxTwistDegrees,
    IReadOnlyList<PostProcessDiagnostic> Diagnostics);
public readonly record struct BonePhysicsBody(string Bone, string Parent, Vector3 Center,
    Quaternion Rotation, float Radius, float HalfHeight, bool Simulated);
public readonly record struct BonePhysicsState(bool Enabled, bool Active, int SimulatedBodies,
    int Constraints, float MaxDisplacement, string Reason, BonePhysicsBody[]? Bodies = null);
public readonly record struct MotionMatchingState(string DatabaseIdentity, bool QueryReady, string Decision,
    long SearchCount, string SelectedClip, float SelectedTime, SpeedMatchingState SpeedMatching = default);
internal sealed record AnimationCommand(string Clip, bool Loop, float Rate, bool InPlace,
    string Database = "", string[]? AllowedClips = null, bool ActionLocked = false, bool MatchSpeed = false,
    bool FootPlacement = false, bool Inertialization = false, bool RootOffset = false,
    string FootLock = "Auto", string Montage = "", IReadOnlyDictionary<string, WarpTarget>? MontageTargets = null,
    float MontageStartTime = 0, bool Transition = false, float TransitionStartTime = 0, float BlendSeconds = 0,
    string SyncGroup = "", float SyncPhase = -1);
internal sealed record SlopeWarpingCommand(bool Enabled, float Weight, float MaxSlopeDegrees,
    float PelvisHeightOffset, float FootCompensation, float ResponseSpeed, float MaxPelvisOffset);
internal sealed record PoseDriverCommand(bool Enabled, string Asset);
internal sealed record PostProcessCommand(bool Enabled, string Asset);
internal sealed record BonePhysicsCommand(bool Enabled, string Asset, bool Debug);
// BlendSpace1D 是播放命令之后的附加候选：只更新第二片段与权重，不推进第二时钟。
internal sealed record BlendSpaceCommand(bool Enabled, string ClipA, string ClipB, float Weight)
{
    public static readonly BlendSpaceCommand Disabled = new(false, "", "", 0);
}
// ApplyAdditive（LocalSpaceBase）与 Save/UseCachedPose 的附加候选。
internal sealed record AdditiveCommand(bool Enabled, string Clip, float Alpha, bool Loop)
{
    public static readonly AdditiveCommand Disabled = new(false, "", 0, false);
}
internal sealed record CachedPoseCommand(bool Enabled, bool Capture, string Name)
{
    public static readonly CachedPoseCommand Disabled = new(false, false, "");
}
// LayeredBoneBlend：overlay 片段按骨骼 mask 混合到当前姿态。
internal sealed record LayeredPoseCommand(bool Enabled, string Clip, float Alpha, bool Loop,
    string[] Mask)
{
    public static readonly LayeredPoseCommand Disabled =
        new(false, "", 0, false, Array.Empty<string>());
}

// catalog 曲线键：times/values 等长且已按时间排序；C# 只在本地做线性 clamp 采样。
internal sealed class AnimationCurve(float[] times, float[] values)
{
    public readonly float[] Times = times;
    public readonly float[] Values = values;
}

// 只提交片段意图并读取原生反馈；不在 CLR 维护第二份播放时钟或骨骼姿态。
public sealed class AnimationPlayback
{
    internal Action CheckAccess = null!;
    internal Action CheckRequest = null!;
    internal AnimationCommand? Pending;
    internal AnimationState Feedback;
    internal MotionMatchingState MotionFeedback;
    internal FootPlacementState FootPlacementFeedback;
    internal RootOffsetState RootOffsetFeedback;
    internal SlopeWarpingState SlopeWarpingFeedback;
    internal SlopeWarpingCommand? PendingSlopeWarping;
    internal SlopeWarpingCommand? ActiveSlopeWarping;
    internal string[] ClipIds = [];
    // clipId -> 曲线名（大小写不敏感）-> 键数据；只由 animation_catalog 的 assets 段填充。
    internal readonly Dictionary<string, Dictionary<string, AnimationCurve>> CurveData = new(StringComparer.Ordinal);
    internal PoseDriverState PoseDriverFeedback = new(false, false, 0, "", "",
        new Dictionary<string, float>(StringComparer.Ordinal), Array.Empty<PoseDriverDiagnostic>());
    internal PoseDriverCommand? PendingPoseDriver;
    internal PoseDriverCommand? ActivePoseDriver;
    internal PostProcessState PostProcessFeedback = new(false, "", "", 0, 0, 0, 0, 0, 0, 0,
        Array.Empty<PostProcessDiagnostic>());
    internal PostProcessCommand? PendingPostProcess;
    internal PostProcessCommand? ActivePostProcess;
    internal BonePhysicsState BonePhysicsFeedback = default;
    internal BonePhysicsCommand? PendingBonePhysics;
    internal BonePhysicsCommand? ActiveBonePhysics;
    internal BlendSpaceCommand? PendingBlendSpace;
    internal AdditiveCommand? PendingAdditive;
    internal CachedPoseCommand? PendingCachedPose;
    internal LayeredPoseCommand? PendingLayeredPose;
    internal bool BonePhysicsDebugRequested;
    internal readonly Dictionary<string, MontageInfo> Montages = new(StringComparer.Ordinal);
    internal readonly Dictionary<string, AnimationPlayerInfo[]> ClipPlayers = new(StringComparer.Ordinal);
    internal readonly Dictionary<string, AnimationSyncMarker[]> ClipSyncMarkers = new(StringComparer.Ordinal);
    public IReadOnlyList<string> Clips { get { CheckAccess(); return Array.AsReadOnly(ClipIds); } }
    public AnimationState State { get { CheckAccess(); return Feedback; } }
    public MotionMatchingState Motion { get { CheckAccess(); return MotionFeedback; } }
    public FootPlacementState FootPlacement { get { CheckAccess(); return FootPlacementFeedback; } }
    public RootOffsetState RootOffset { get { CheckAccess(); return RootOffsetFeedback; } }
    public SlopeWarpingState SlopeWarping { get { CheckAccess(); return SlopeWarpingFeedback; } }
    public PoseDriverState PoseDriver { get { CheckAccess(); return PoseDriverFeedback; } }
    public PostProcessState PostProcess { get { CheckAccess(); return PostProcessFeedback; } }
    public BonePhysicsState BonePhysics { get { CheckAccess(); return BonePhysicsFeedback; } }

    // UE PostProcess AnimBP 的 PoseDriver/DriveCurves 阶段。资产由资源侧声明，
    // 原生按骨架解析 SourceBones 并求解 RBF；这里只提交开关与路径。
    public void SetPoseDriver(bool enabled, string asset = "")
    {
        CheckRequest();
        if (enabled)
        {
            if (string.IsNullOrEmpty(asset) || asset.Length > 1024 ||
                !asset.EndsWith(".json", StringComparison.Ordinal) ||
                asset.Contains('\\') || asset.Contains(':') ||
                !(asset.StartsWith("Maps/", StringComparison.Ordinal) ||
                    asset.StartsWith("Common/", StringComparison.Ordinal)))
                throw new ArgumentException("Pose driver requires a formal resource path.", nameof(asset));
        }
        else asset = "";
        var next = new PoseDriverCommand(enabled, asset);
        if (ActivePoseDriver == next && PoseDriverFeedback.Enabled == next.Enabled) return;
        ActivePoseDriver = next;
        PendingPoseDriver = next;
    }

    // 缺少该曲线返回 false；曲线名大小写敏感，与 UE FName 一致。
    public bool TryGetPoseCurve(string name, out float value)
    {
        CheckAccess();
        return PoseDriverFeedback.Curves.TryGetValue(name, out value);
    }

    // UE PostProcess AnimBP 的 Twist/Helpers 阶段（当前实现 Twist ControlRig 的原生替代）。
    public void SetPostProcess(bool enabled, string asset = "")
    {
        CheckRequest();
        if (enabled)
        {
            if (string.IsNullOrEmpty(asset) || asset.Length > 1024 ||
                !asset.EndsWith(".json", StringComparison.Ordinal) ||
                asset.Contains('\\') || asset.Contains(':') ||
                !(asset.StartsWith("Maps/", StringComparison.Ordinal) ||
                    asset.StartsWith("Common/", StringComparison.Ordinal)))
                throw new ArgumentException("Post-process requires a formal resource path.", nameof(asset));
        }
        else asset = "";
        var next = new PostProcessCommand(enabled, asset);
        if (ActivePostProcess == next && PostProcessFeedback.Enabled == next.Enabled) return;
        ActivePostProcess = next;
        PendingPostProcess = next;
    }

    // UE AnimNode_RigidBody 的正式原生替代：Jolt 骨骼刚体。
    public void SetBonePhysics(bool enabled, string asset = "")
    {
        CheckRequest();
        if (enabled)
        {
            if (string.IsNullOrEmpty(asset) || asset.Length > 1024 ||
                !asset.EndsWith(".json", StringComparison.Ordinal) ||
                asset.Contains('\\') || asset.Contains(':') ||
                !(asset.StartsWith("Maps/", StringComparison.Ordinal) ||
                    asset.StartsWith("Common/", StringComparison.Ordinal)))
                throw new ArgumentException("Bone physics requires a formal resource path.", nameof(asset));
        }
        else asset = "";
        var next = new BonePhysicsCommand(enabled, asset, BonePhysicsDebugRequested);
        if (ActiveBonePhysics == next && BonePhysicsFeedback.Enabled == next.Enabled) return;
        ActiveBonePhysics = next;
        PendingBonePhysics = next;
    }

    // 只在需要时（例如起跳→落地）请求刚体描述，避免每帧多加 21 条 JSON。
    public void SetBonePhysicsDebug(bool enabled)
    {
        CheckRequest();
        if (BonePhysicsDebugRequested == enabled) return;
        BonePhysicsDebugRequested = enabled;
        // 复用同一条 configureBonePhysics：只改 debug 字段也要重新提交一次。
        if (ActiveBonePhysics is { } active)
            PendingBonePhysics = active with { Debug = enabled };
    }

    // 提交斜坡姿态修正参数；原生只改视觉局部 TRS，Actor 位置与 root motion 归胶囊。
    public void SetSlopeWarping(bool enabled, float weight = 1, float maxSlopeDegrees = 45,
        float pelvisHeightOffset = 0, float footCompensation = 1, float responseSpeed = 12,
        float maxPelvisOffset = 30)
    {
        CheckRequest();
        if (!float.IsFinite(weight) || weight < 0 || weight > 1)
            throw new ArgumentOutOfRangeException(nameof(weight));
        if (!float.IsFinite(maxSlopeDegrees) || maxSlopeDegrees < 0 || maxSlopeDegrees >= 90)
            throw new ArgumentOutOfRangeException(nameof(maxSlopeDegrees));
        if (!float.IsFinite(pelvisHeightOffset) || MathF.Abs(pelvisHeightOffset) > 200)
            throw new ArgumentOutOfRangeException(nameof(pelvisHeightOffset));
        if (!float.IsFinite(footCompensation) || footCompensation < 0 || footCompensation > 2)
            throw new ArgumentOutOfRangeException(nameof(footCompensation));
        if (!float.IsFinite(responseSpeed) || responseSpeed < 0 || responseSpeed > 120)
            throw new ArgumentOutOfRangeException(nameof(responseSpeed));
        if (!float.IsFinite(maxPelvisOffset) || maxPelvisOffset < 0 || maxPelvisOffset > 200)
            throw new ArgumentOutOfRangeException(nameof(maxPelvisOffset));
        var next = new SlopeWarpingCommand(enabled, weight, maxSlopeDegrees, pelvisHeightOffset,
            footCompensation, responseSpeed, maxPelvisOffset);
        // 蓝图每帧声明同一组参数；只在变化时提交，避免 Stop/换 World 时把
        // 已销毁组件的配置请求留在命令队列里。
        // 播放器在 Stop/换 World 时会重建并回到 disabled；反馈未追上声明值时重发一次。
        if (ActiveSlopeWarping == next && SlopeWarpingFeedback.Enabled == next.Enabled) return;
        ActiveSlopeWarping = next;
        PendingSlopeWarping = next;
    }
    // 只播放 clip；warp 的 target/窗口完全由 Montage 资产声明，这里不再接受 warpTargets。
    public void Play(string clip, bool loop = true, float rate = 1, bool inPlace = true)
    {
        CheckRequest();
        if (!ClipIds.Contains(clip, StringComparer.Ordinal)) throw new ArgumentException($"Unknown animation clip: {clip}");
        if (!float.IsFinite(rate) || rate <= 0 || rate > 4) throw new ArgumentOutOfRangeException(nameof(rate));
        Pending = new(clip, loop, rate, inPlace);
    }

    // UE 状态机过渡：从当前显示姿态交叉淡入目标片段；startTime 是目标片段的入口秒。
    // 与 Play 不同，TransitionTo 保留当前姿态作为 blend source，支持 0 秒硬切。
    public void PlayTransition(string clip, float startTime, bool loop, float blendSeconds,
        bool inPlace = true, float rate = 1, bool inertialization = false,
        string syncGroup = "", float syncPhase = -1)
    {
        CheckRequest();
        if (!ClipIds.Contains(clip, StringComparer.Ordinal)) throw new ArgumentException($"Unknown animation clip: {clip}");
        if (!float.IsFinite(startTime) || startTime < 0)
            throw new ArgumentOutOfRangeException(nameof(startTime));
        if (!float.IsFinite(blendSeconds) || blendSeconds < 0 || blendSeconds > 10)
            throw new ArgumentOutOfRangeException(nameof(blendSeconds));
        if (!float.IsFinite(rate) || rate <= 0 || rate > 4)
            throw new ArgumentOutOfRangeException(nameof(rate));
        // 相位必须配组；只有组没有相位是合法的（本次过渡不做相位对齐，
        // 原生 TransitionToSynced 会按 startTime 直接进入）。
        if (syncPhase >= 0 && string.IsNullOrEmpty(syncGroup))
            throw new ArgumentException("Sync phase requires a sync group.", nameof(syncPhase));
        if (syncGroup.Length > 128 || syncGroup.Contains('\0'))
            throw new ArgumentException("Sync group is invalid.", nameof(syncGroup));
        if (!float.IsFinite(syncPhase) || syncPhase < -1 || syncPhase > 1)
            throw new ArgumentOutOfRangeException(nameof(syncPhase));
        Pending = new(clip, loop, rate, inPlace, Inertialization: inertialization,
            Transition: true, TransitionStartTime: startTime, BlendSeconds: blendSeconds,
            SyncGroup: syncGroup, SyncPhase: syncPhase);
    }

    // UE BlendSpace1D：A 是唯一时钟片段，B 按 A 的归一化相位采样后加权混合。
    // 权重越界按 [0,1] 夹紧；A/B 必须是 catalog 里的片段，A 必须等于本帧已播放的片段。
    public void SetBlendSpace(string clipA, string clipB, float weight)
    {
        CheckRequest();
        if (!ClipIds.Contains(clipA, StringComparer.Ordinal))
            throw new ArgumentException($"Unknown animation clip: {clipA}", nameof(clipA));
        if (!ClipIds.Contains(clipB, StringComparer.Ordinal))
            throw new ArgumentException($"Unknown animation clip: {clipB}", nameof(clipB));
        if (!float.IsFinite(weight)) throw new ArgumentOutOfRangeException(nameof(weight));
        if (string.Equals(clipA, clipB, StringComparison.Ordinal))
        {
            ClearBlendSpace();
            return;
        }
        PendingBlendSpace = new(true, clipA, clipB, Math.Clamp(weight, 0, 1));
    }

    public void ClearBlendSpace()
    {
        CheckRequest();
        PendingBlendSpace = BlendSpaceCommand.Disabled;
    }

    // UE ApplyAdditive（LocalSpaceBase）：clip 必须是 LocalSpaceBase 片段，原生负责
    // 增量数学与独立时钟；alpha 夹到 [0,1]，loop 决定叠加片段是否循环。
    public void SetAdditive(string clip, float alpha, bool loop = false)
    {
        CheckRequest();
        if (!ClipIds.Contains(clip, StringComparer.Ordinal))
            throw new ArgumentException($"Unknown animation clip: {clip}", nameof(clip));
        if (!float.IsFinite(alpha)) throw new ArgumentOutOfRangeException(nameof(alpha));
        PendingAdditive = new(true, clip, Math.Clamp(alpha, 0, 1), loop);
    }

    public void ClearAdditive()
    {
        CheckRequest();
        PendingAdditive = AdditiveCommand.Disabled;
    }

    // 缓存姿态：capture 保存当前最终姿态，use 以该快照为 Base；名字为空由调用方拒绝。
    public void CaptureCachedPose(string name)
    {
        CheckRequest();
        if (string.IsNullOrWhiteSpace(name) || name.Length > 128 || name.Contains('\0'))
            throw new ArgumentException("Cached pose name is invalid.", nameof(name));
        PendingCachedPose = new(true, true, name);
    }

    public void UseCachedPose(string name)
    {
        CheckRequest();
        if (string.IsNullOrWhiteSpace(name) || name.Length > 128 || name.Contains('\0'))
            throw new ArgumentException("Cached pose name is invalid.", nameof(name));
        PendingCachedPose = new(true, false, name);
    }

    public void ClearCachedPose()
    {
        CheckRequest();
        PendingCachedPose = CachedPoseCommand.Disabled;
    }

    // LayeredBoneBlend：mask 为空表示全身；未知骨骼名由原生拒绝。
    public void SetLayeredPose(string clip, float alpha, bool loop, IReadOnlyList<string>? mask)
    {
        CheckRequest();
        if (!ClipIds.Contains(clip, StringComparer.Ordinal))
            throw new ArgumentException($"Unknown animation clip: {clip}", nameof(clip));
        if (!float.IsFinite(alpha)) throw new ArgumentOutOfRangeException(nameof(alpha));
        var bones = mask is null ? Array.Empty<string>() : mask.ToArray();
        if (bones.Length > 64)
            throw new ArgumentException("Layer mask exceeds 64 bones.", nameof(mask));
        foreach (var bone in bones)
            if (string.IsNullOrWhiteSpace(bone) || bone.Length > 128 || bone.Contains('\0'))
                throw new ArgumentException("Layer mask contains an invalid bone name.", nameof(mask));
        PendingLayeredPose = new(true, clip, Math.Clamp(alpha, 0, 1), loop, bones);
    }

    public void ClearLayeredPose()
    {
        CheckRequest();
        PendingLayeredPose = LayeredPoseCommand.Disabled;
    }

    // 任意时刻曲线采样：秒、clip-local、键间线性、区间外 clamp 到首/末值、单键常量。
    // 纯读取，不推进时钟/姿态/notify；曲线键来自一次性 catalog，不逐帧跨桥。
    public float CurveValue(string clipId, string curveName, float time)
    {
        CheckAccess();
        ValidateCurveArguments(clipId, curveName, time);
        var curves = RequireCurves(clipId, nameof(clipId));
        if (!curves.TryGetValue(curveName, out var curve))
            throw new ArgumentException(
                $"Unknown animation curve '{curveName}' for clip '{clipId}'. {DescribeCurves(curves)}",
                nameof(curveName));
        return SampleCurve(curve, curveName, clipId, time);
    }

    // 当前片段已发布的曲线名；已知片段没有 sidecar/曲线时返回空数组，未知片段是硬错误。
    public IReadOnlyList<string> CurveNames(string clipId)
    {
        CheckAccess();
        ValidateClipId(clipId);
        if (!CurveData.TryGetValue(clipId, out var curves) || curves.Count == 0)
            return Array.Empty<string>();
        return Array.AsReadOnly(curves.Keys.ToArray());
    }

    // 曲线存在性查询；仍校验片段身份，避免把拼错的 clip 当成“曲线缺失”。
    public bool HasCurve(string clipId, string curveName)
    {
        CheckAccess();
        ValidateClipId(clipId);
        if (string.IsNullOrEmpty(curveName))
            throw new ArgumentException("Animation curve name is required.", nameof(curveName));
        return CurveData.TryGetValue(clipId, out var curves) && curves.ContainsKey(curveName);
    }

    public bool HasCurrentCurve(string curveName)
    {
        var state = State;
        return HasCurve(state.Clip, curveName);
    }

    // 只采样调用方显式列出的名字；不会把所有曲线复制进 State/feedback。
    public IReadOnlyDictionary<string, float> CurveSnapshot(IReadOnlyList<string> names)
    {
        CheckAccess();
        ArgumentNullException.ThrowIfNull(names);
        if (names.Count == 0 || names.Count > 64)
            throw new ArgumentOutOfRangeException(nameof(names), "Curve snapshot requires 1..64 names.");
        var state = State;
        var curves = RequireCurves(state.Clip, nameof(names));
        var result = new Dictionary<string, float>(names.Count, StringComparer.OrdinalIgnoreCase);
        foreach (var name in names)
        {
            if (string.IsNullOrEmpty(name))
                throw new ArgumentException("Animation curve name is required.", nameof(names));
            if (!curves.TryGetValue(name, out var curve))
                throw new ArgumentException(
                    $"Unknown animation curve '{name}' for clip '{state.Clip}'. {DescribeCurves(curves)}",
                    nameof(names));
            if (!result.TryAdd(name, SampleCurve(curve, name, state.Clip, state.Time)))
                throw new ArgumentException($"Duplicate animation curve '{name}' in snapshot request.", nameof(names));
        }
        return result;
    }

    private Dictionary<string, AnimationCurve> RequireCurves(string clipId, string parameterName)
    {
        ValidateClipId(clipId, parameterName);
        if (!CurveData.TryGetValue(clipId, out var curves) || curves.Count == 0)
            throw new ArgumentException(
                $"Animation clip '{clipId}' has no published Float curves (no sidecar or an empty curve set).",
                parameterName);
        return curves;
    }

    private void ValidateClipId(string clipId, string parameterName = "clipId")
    {
        if (string.IsNullOrEmpty(clipId))
            throw new ArgumentException("Animation clip id is required.", parameterName);
        if (!ClipIds.Contains(clipId, StringComparer.Ordinal))
            throw new ArgumentException($"Unknown animation clip: {clipId}", parameterName);
    }

    private static void ValidateCurveArguments(string clipId, string curveName, float time)
    {
        if (!float.IsFinite(time)) throw new ArgumentOutOfRangeException(nameof(time));
        if (string.IsNullOrEmpty(clipId))
            throw new ArgumentException("Animation clip id is required.", nameof(clipId));
        if (string.IsNullOrEmpty(curveName))
            throw new ArgumentException("Animation curve name is required.", nameof(curveName));
    }

    private static string DescribeCurves(Dictionary<string, AnimationCurve> curves)
    {
        const int limit = 12;
        string[] names = curves.Keys.Take(limit).ToArray();
        string suffix = curves.Count > limit ? $", ... ({curves.Count} total)" : "";
        return $"Available curves: {string.Join(", ", names)}{suffix}.";
    }

    private static float SampleCurve(AnimationCurve curve, string curveName, string clipId, float time)
    {
        float[] times = curve.Times, values = curve.Values;
        if (times.Length == 0)
            throw new ArgumentException($"Animation curve '{curveName}' for clip '{clipId}' has no keys.", nameof(curveName));
        if (time <= times[0]) return values[0];
        if (time >= times[^1]) return values[^1];
        var index = Array.BinarySearch(times, time);
        if (index >= 0) return values[index];
        index = ~index; // 插入点；时间落在 [index-1,index) 内。
        var span = times[index] - times[index - 1];
        if (span <= 0) return values[index - 1];
        return values[index - 1] + (time - times[index - 1]) / span * (values[index] - values[index - 1]);
    }

    // Montage 静态数据（catalog）；未知 id 是硬错误，不做返回 null 的静默降级。
    public MontageInfo Montage(string id)
    {
        CheckAccess();
        if (string.IsNullOrEmpty(id) || !Montages.TryGetValue(id, out var montage))
            throw new ArgumentException($"Unknown animation montage: {id}", nameof(id));
        return montage;
    }

    // 动作层使用资源路径而不是 catalog id；AssetPath 来自原生 sourcePath。
    public MontageInfo Montage(AssetId asset)
    {
        CheckAccess();
        string path = asset.Path ?? "";
        if (path.Length > 0)
            foreach (var montage in Montages.Values)
                if (string.Equals(montage.AssetPath, path, StringComparison.Ordinal))
                    return montage;
        throw new ArgumentException($"Unknown animation montage asset: {path}", nameof(asset));
    }

    // AnimBP Sequence Player 元数据；没有声明 player 的 clip 返回空数组。
    public IReadOnlyList<AnimationPlayerInfo> Players(string clipId)
    {
        CheckAccess();
        if (string.IsNullOrEmpty(clipId)) throw new ArgumentException("Animation clip id is required.", nameof(clipId));
        return ClipPlayers.TryGetValue(clipId, out var players) ?
            Array.AsReadOnly(players) : Array.Empty<AnimationPlayerInfo>();
    }

    public IReadOnlyList<AnimationSyncMarker> SyncMarkers(string clipId)
    {
        CheckAccess();
        if (string.IsNullOrEmpty(clipId)) throw new ArgumentException("Animation clip id is required.", nameof(clipId));
        return ClipSyncMarkers.TryGetValue(clipId, out var markers) ?
            Array.AsReadOnly(markers) : Array.Empty<AnimationSyncMarker>();
    }

    // 第一个 Sequence Player 的入口起点；没有声明时返回 0。Stop/WalkStart 的 UE
    // StartPosition 就在这里供动画蓝图读取。
    public float StartPosition(string clipId) =>
        Players(clipId) is { Count: > 0 } players ? players[0].StartPosition : 0;

    public bool HasSyncGroup(string clipId, string group)
    {
        if (string.IsNullOrEmpty(group)) return false;
        foreach (var player in Players(clipId))
            if (player.SyncGroup == group && player.SyncMethod is "SyncGroup" or "MarkerSync") return true;
        return false;
    }

    public string ActiveSyncGroup => State.SyncGroup;
    public float SyncPhase => State.SyncPhase;
    public string SyncMarker => State.SyncMarker;
    public bool SyncActive => State.SyncActive;
    public string SyncLeader => State.SyncLeader;
    public string SyncRole => State.SyncRole;
    public string SyncMethod => State.SyncMethod;
    public string SyncEffectiveMethod => State.SyncEffectiveMethod;
    public string SyncScope => State.SyncScope;
    public string SyncDiagnostic => State.SyncDiagnostic;
    // 常驻便捷采样：等价于 CurveValue(State.Clip, name, State.Time)，仍由调用方决定
    // 何时消费；播放器不会自动执行曲线。
    public float CurrentCurve(string name) { var state = State; return CurveValue(state.Clip, name, state.Time); }

    // 提交 Montage 播放意图；Montage 的 section 路由/provider/easing/target 校验在 C# 入口
    // 先给诊断，原生再按同一规则拒绝。clip id 由 catalog 从 sidecar 派生，调用方不提交。
    // 这个重载保留旧的“名称 -> 位置”调用形状；需要目标旋转时用 WarpTarget 重载。
    // startTime 是入口起始时间（秒、clip-local，缺省 0 = 从 section 0 起点播放）：
    // 原生把它当一次初始 seek，跳过之前的 notify 与根位移；落在 section 0 之外由原生拒绝。
    public void PlayMontage(string montageId, IReadOnlyDictionary<string, Vector3> targets,
        bool loop = false, float rate = 1, bool inPlace = true, float startTime = 0)
    {
        CheckRequest();
        ArgumentNullException.ThrowIfNull(targets);
        var converted = new Dictionary<string, WarpTarget>(StringComparer.Ordinal);
        foreach (var pair in targets) converted[pair.Key] = new WarpTarget(pair.Value);
        PlayMontageCore(montageId, converted, loop, rate, inPlace, startTime);
    }

    // 目标可带世界旋转；只有资产窗口 warpRotation=true 且 target.Rotation 非 null 时，
    // 原生才把根 yaw warp 到该朝向。C# 不持有窗口状态、也不做旋转数学。
    public void PlayMontage(string montageId, IReadOnlyDictionary<string, WarpTarget> targets,
        bool loop = false, float rate = 1, bool inPlace = true, float startTime = 0)
    {
        CheckRequest();
        ArgumentNullException.ThrowIfNull(targets);
        PlayMontageCore(montageId, targets, loop, rate, inPlace, startTime);
    }

    private void PlayMontageCore(string montageId, IReadOnlyDictionary<string, WarpTarget> targets,
        bool loop, float rate, bool inPlace, float startTime)
    {
        if (string.IsNullOrEmpty(montageId) || !Montages.TryGetValue(montageId, out var montage))
            throw new ArgumentException($"Unknown animation montage: {montageId}", nameof(montageId));
        if (loop)
            throw new ArgumentException($"Animation montage '{montageId}' does not support looping playback.", nameof(loop));
        if (!float.IsFinite(rate) || rate <= 0 || rate > 4)
            throw new ArgumentOutOfRangeException(nameof(rate));
        // 入口时间只做 wire 级校验（有限/非负）；落在 section 0 之外由原生按资产范围拒绝，
        // C# 不复制资产时间轴判断，避免与原生 effective end（clip 时长）分叉。
        if (!float.IsFinite(startTime) || startTime < 0)
            throw new ArgumentOutOfRangeException(nameof(startTime));
        if (targets.Count > 32)
            throw new ArgumentException("Montage warp target budget exceeded.", nameof(targets));
        // 多 section 与 Static provider 已由原生执行层支持；这里只做执行层仍不支持的
        // easing/rotation method/provider 枚举与 target 集合校验，不再在 C# 提前拒绝。
        var section = montage.Sections.Count > 0 ? montage.Sections[0].Name : "";
        var required = new HashSet<string>(StringComparer.Ordinal);
        foreach (var notify in montage.Notifies)
        {
            if (!notify.HasWarp) continue;
            if (notify.WarpProvider is not ("None" or "Bone" or "Static"))
                throw new ArgumentException($"Animation montage '{montageId}' section '{section}' uses warp provider " +
                    $"'{notify.WarpProvider}', which is not supported.", nameof(montageId));
            if (notify.Easing != "Linear")
                throw new ArgumentException($"Animation montage '{montageId}' section '{section}' uses easing " +
                    $"'{notify.Easing}'; only Linear is supported.", nameof(montageId));
            if (notify.WarpRotation && notify.RotationMethod != "Slerp")
                throw new ArgumentException($"Animation montage '{montageId}' section '{section}' uses rotation method " +
                    $"'{notify.RotationMethod}'; only Slerp is supported.", nameof(montageId));
            required.Add(notify.WarpTarget);
        }
        foreach (var name in required)
            if (!targets.ContainsKey(name))
                throw new ArgumentException($"Animation montage '{montageId}' requires warp target '{name}'.", nameof(targets));
        foreach (var pair in targets)
            if (!required.Contains(pair.Key))
                throw new ArgumentException($"Animation montage '{montageId}' does not declare warp target '{pair.Key}'.",
                    nameof(targets));
        var sorted = new SortedDictionary<string, WarpTarget>(StringComparer.Ordinal);
        foreach (var pair in targets)
        {
            var target = pair.Value;
            if (!float.IsFinite(target.Location.X) || !float.IsFinite(target.Location.Y) ||
                !float.IsFinite(target.Location.Z))
                throw new ArgumentException($"Warp target '{pair.Key}' requires a finite location.", nameof(targets));
            if (target.Rotation is { } rotation &&
                (!float.IsFinite(rotation.X) || !float.IsFinite(rotation.Y) ||
                 !float.IsFinite(rotation.Z) || !float.IsFinite(rotation.W) ||
                 !(rotation.LengthSquared() > 1e-8f)))
                throw new ArgumentException($"Warp target '{pair.Key}' requires a finite non-zero rotation.",
                    nameof(targets));
            sorted[pair.Key] = target;
        }
        if (!ClipIds.Contains(montage.ClipId, StringComparer.Ordinal))
            throw new ArgumentException($"Animation montage '{montageId}' clip '{montage.ClipId}' is not in the mesh " +
                "clip catalog.", nameof(montageId));
        Pending = new("", loop, rate, inPlace, Montage: montageId, MontageTargets: sorted,
            MontageStartTime: startTime);
    }

    // CLR 只提交动作族、动作锁与可选修正开关；求值、时钟和姿态全部由原生拥有。
    public void Match(AssetId database, IReadOnlyList<string> allowedClips, bool actionLocked = false,
        bool matchSpeed = false, bool footPlacement = false, bool inertialization = false, bool rootOffset = false,
        string footLock = "Auto")
    {
        CheckRequest();
        if (footLock is not ("Auto" or "Ball" or "Ankle"))
            throw new ArgumentException("Foot lock requires Auto, Ball or Ankle.", nameof(footLock));
        string path = database.Path ?? "";
        if (path.Length == 0 || path.Length > 1024 || Path.IsPathRooted(path) ||
            path.Contains('\\') || path.Contains(':') || path.Split('/').Any(p => p is "" or "." or "..") ||
            !(path.StartsWith("Common/", StringComparison.Ordinal) || path.StartsWith("Maps/", StringComparison.Ordinal)) ||
            !path.EndsWith(".json", StringComparison.Ordinal))
            throw new ArgumentException("Motion database requires a formal resource-relative JSON path.", nameof(database));
        ArgumentNullException.ThrowIfNull(allowedClips);
        if (allowedClips.Count > 256) throw new ArgumentException("Motion candidate budget exceeded.", nameof(allowedClips));
        string[] allowed = allowedClips.ToArray();
        if (allowed.Distinct(StringComparer.Ordinal).Count() != allowed.Length ||
            allowed.Any(clip => !ClipIds.Contains(clip, StringComparer.Ordinal)))
            throw new ArgumentException("Motion candidates require unique known clip IDs.", nameof(allowedClips));
        Pending = new("", true, 1, true, path, allowed, actionLocked, matchSpeed, footPlacement,
            inertialization, rootOffset, footLock);
    }
}

public static partial class NativeBridge
{
    private static void BindMeshAnimation(Context context, Blueprint owner, SceneComponent component)
    {
        if (component is not MeshComponent mesh) return;
        mesh.Animation.CheckAccess = () =>
        {
            if (Environment.CurrentManagedThreadId != ownerThread || !mesh.IsAlive)
                throw new InvalidOperationException("Stale animation component or wrong thread.");
        };
        mesh.Animation.CheckRequest = () =>
        {
            RequireCallback(context, owner);
            mesh.Animation.CheckAccess();
            if (callbackPhase is "OnCreate" or "OnReload" or "EndPlay" || !context.Playing || owner.PendingDestroy)
                throw new InvalidOperationException("Animation requests require an active gameplay callback.");
        };
        BindAnimationCatalog(context, mesh.Animation, mesh.Mesh.Path);
    }

    // catalog 的 clip 列表与 assets 段一起绑定：clip 列表保持旧形状，curves/montages 只做本地缓存。
    private static void BindAnimationCatalog(Context context, AnimationPlayback animation, string path)
    {
        animation.ClipIds = context.AnimationCatalog.GetValueOrDefault(path) ?? [];
        animation.CurveData.Clear();
        animation.Montages.Clear();
        animation.ClipPlayers.Clear();
        animation.ClipSyncMarkers.Clear();
        if (!context.AnimationAssets.TryGetValue(path, out var assets) ||
            assets.ValueKind != JsonValueKind.Object) return;
        if (assets.TryGetProperty("curves", out var curves) && curves.ValueKind == JsonValueKind.Object)
            foreach (var clip in curves.EnumerateObject())
            {
                var byName = new Dictionary<string, AnimationCurve>(StringComparer.OrdinalIgnoreCase);
                foreach (var curve in clip.Value.EnumerateArray())
                    byName[curve.GetProperty("name").GetString()!] = new AnimationCurve(
                        curve.GetProperty("times").EnumerateArray().Select(value => value.GetSingle()).ToArray(),
                        curve.GetProperty("values").EnumerateArray().Select(value => value.GetSingle()).ToArray());
                if (byName.Count > 0) animation.CurveData[clip.Name] = byName;
            }
        if (assets.TryGetProperty("players", out var players) && players.ValueKind == JsonValueKind.Object)
            foreach (var clip in players.EnumerateObject())
            {
                var values = clip.Value.EnumerateArray().Select(player => new AnimationPlayerInfo(
                    player.GetProperty("syncGroup").GetString()!,
                    player.GetProperty("syncRole").GetString()!,
                    player.GetProperty("syncMethod").GetString()!,
                    player.GetProperty("startPosition").GetSingle(),
                    player.GetProperty("loop").GetBoolean(),
                    player.TryGetProperty("syncGroupScope", out var scope) &&
                        scope.ValueKind == JsonValueKind.String ? scope.GetString()! : "Local")).ToArray();
                if (values.Length > 0) animation.ClipPlayers[clip.Name] = values;
            }
        if (assets.TryGetProperty("syncMarkers", out var markers) && markers.ValueKind == JsonValueKind.Object)
            foreach (var clip in markers.EnumerateObject())
            {
                var values = clip.Value.EnumerateArray().Select(marker => new AnimationSyncMarker(
                    marker.GetProperty("name").GetString()!,
                    marker.GetProperty("time").GetSingle())).ToArray();
                if (values.Length > 0) animation.ClipSyncMarkers[clip.Name] = values;
            }
        if (assets.TryGetProperty("montages", out var montages) && montages.ValueKind == JsonValueKind.Array)
            foreach (var montage in montages.EnumerateArray())
            {
                var info = ReadMontage(montage);
                animation.Montages[info.Id] = info;
            }
    }

    private static MontageInfo ReadMontage(JsonElement value)
    {
        var segments = new List<MontageSegmentInfo>();
        if (value.TryGetProperty("segments", out var segmentValues))
            foreach (var segment in segmentValues.EnumerateArray())
                segments.Add(new(
                    segment.TryGetProperty("index", out var index) ? index.GetInt32() : segments.Count,
                    segment.GetProperty("segmentId").GetString()!,
                    segment.GetProperty("sequencePath").GetString()!,
                    segment.GetProperty("derivedClipId").GetString()!,
                    segment.GetProperty("startTime").GetSingle(),
                    segment.GetProperty("endTime").GetSingle(),
                    segment.GetProperty("clipStartTime").GetSingle(),
                    segment.GetProperty("clipEndTime").GetSingle(),
                    segment.GetProperty("rateScale").GetSingle(),
                    segment.GetProperty("loop").GetBoolean()));
        var sections = new List<MontageSectionInfo>();
        if (value.TryGetProperty("sections", out var sectionValues))
            foreach (var section in sectionValues.EnumerateArray())
                sections.Add(new(section.GetProperty("name").GetString()!,
                    section.GetProperty("startTime").GetSingle(), section.GetProperty("endTime").GetSingle(),
                    section.GetProperty("nextSection").GetString()!, section.GetProperty("nextTime").GetSingle(),
                    section.GetProperty("linkMethod").GetString()!));
        var notifies = new List<MontageNotifyInfo>();
        var windows = new List<MontageWindowInfo>();
        if (value.TryGetProperty("notifies", out var notifyValues))
            foreach (var notify in notifyValues.EnumerateArray())
            {
                string target = "", provider = "", bone = "", easing = "", rotationMethod = "Slerp";
                bool hasWarp = false, subtract = false, ignoreZ = false, warpRotation = false;
                var multiplier = 1.0f;
                if (notify.TryGetProperty("warp", out var warp) && warp.ValueKind == JsonValueKind.Object)
                {
                    hasWarp = true;
                    target = warp.GetProperty("target").GetString()!;
                    provider = warp.GetProperty("provider").GetString()!;
                    bone = warp.GetProperty("bone").GetString()!;
                    subtract = warp.GetProperty("subtractRemainingRootMotion").GetBoolean();
                    ignoreZ = warp.GetProperty("ignoreZAxis").GetBoolean();
                    warpRotation = warp.GetProperty("warpRotation").GetBoolean();
                    multiplier = warp.GetProperty("rotationTimeMultiplier").GetSingle();
                    easing = warp.GetProperty("easing").GetString()!;
                    // 旧 catalog（本字段引入前）没有 rotationMethod；缺省即 UE 默认 Slerp。
                    if (warp.TryGetProperty("rotationMethod", out var method) &&
                        method.ValueKind == JsonValueKind.String)
                        rotationMethod = method.GetString()!;
                }
                var time = notify.GetProperty("time").GetSingle();
                var duration = notify.GetProperty("duration").GetSingle();
                notifies.Add(new(notify.GetProperty("kind").GetString()!, notify.GetProperty("track").GetString()!,
                    notify.GetProperty("name").GetString()!, time, duration, hasWarp, target, provider, bone,
                    subtract, ignoreZ, warpRotation, multiplier, easing, rotationMethod));
                if (hasWarp) windows.Add(new(target, time, time + duration));
            }
        return new(value.GetProperty("id").GetString()!, value.GetProperty("sequencePath").GetString()!,
            value.GetProperty("derivedClipId").GetString()!, value.GetProperty("length").GetSingle(),
            value.GetProperty("slot").GetString()!, sections, notifies, windows,
            value.TryGetProperty("rateScale", out var rateScale) ? rateScale.GetSingle() : 1,
            value.TryGetProperty("path", out var path) ? path.GetString()! : "",
            segments);
    }

    private static void UpdateAnimationCatalog(Context context, JsonElement catalog, JsonElement assets)
    {
        context.AnimationCatalog.Clear();
        foreach (var entry in catalog.EnumerateObject())
            context.AnimationCatalog.Add(entry.Name, entry.Value.EnumerateArray().Select(c => c.GetString()!).ToArray());
        context.AnimationAssets.Clear();
        if (assets.ValueKind == JsonValueKind.Object)
            foreach (var entry in assets.EnumerateObject())
                context.AnimationAssets[entry.Name] = entry.Value.Clone();
        foreach (var instance in context.Instances)
            foreach (var mesh in instance.Blueprint.Components.OfType<MeshComponent>())
                BindAnimationCatalog(context, mesh.Animation, mesh.Mesh.Path);
    }

    private static void SynchronizeAnimations(Context context, JsonElement feedback)
    {
        foreach (var value in feedback.EnumerateArray())
        {
            if (!context.InstanceIds.TryGetValue(value.GetProperty("owner").GetString()!, out var instance)) continue;
            var component = instance.Blueprint.Components.OfType<MeshComponent>()
                .FirstOrDefault(c => c.Id == value.GetProperty("component").GetString());
            if (component == null) continue;
            var notifies = new List<AnimationNotify>();
            if (value.TryGetProperty("notifies", out var notifyValues))
                foreach (var notify in notifyValues.EnumerateArray())
                    notifies.Add(new(notify.GetProperty("name").GetString()!,
                        notify.GetProperty("track").GetString()!, notify.GetProperty("kind").GetString()!,
                        notify.GetProperty("phase").GetString()!, notify.GetProperty("time").GetSingle(),
                        notify.GetProperty("startTime").GetSingle(), notify.GetProperty("duration").GetSingle(),
                        notify.GetProperty("interrupted").GetBoolean(),
                        notify.TryGetProperty("segmentIndex", out var segmentIndex) ? segmentIndex.GetInt32() : 0,
                        notify.TryGetProperty("segmentId", out var segmentId) ? segmentId.GetString()! : ""));
            var montageId = "";
            var montageLength = 0.0f;
            var montageRange = default(Vector2);
            var montageSegment = 0;
            var montageSegmentId = "";
            if (value.TryGetProperty("montage", out var montage) && montage.ValueKind == JsonValueKind.Object)
            {
                montageId = montage.GetProperty("id").GetString()!;
                montageLength = montage.GetProperty("length").GetSingle();
                if (montage.TryGetProperty("segment", out var segment))
                    montageSegment = segment.GetInt32();
                if (montage.TryGetProperty("segmentId", out var segmentId))
                    montageSegmentId = segmentId.GetString()!;
                var range = montage.GetProperty("range");
                montageRange = new(range[0].GetSingle(), range[1].GetSingle());
            }
            var blendClip = "";
            var blendWeight = 0.0f;
            if (value.TryGetProperty("blendSpace", out var blendSpace) &&
                blendSpace.ValueKind == JsonValueKind.Object)
            {
                if (blendSpace.TryGetProperty("clip", out var blendClipNode))
                    blendClip = blendClipNode.GetString()!;
                if (blendSpace.TryGetProperty("weight", out var blendWeightNode))
                    blendWeight = blendWeightNode.GetSingle();
            }
            var additiveClip = "";
            var additiveAlpha = 0.0f;
            if (value.TryGetProperty("additive", out var additiveNode) &&
                additiveNode.ValueKind == JsonValueKind.Object &&
                additiveNode.TryGetProperty("active", out var additiveActive) &&
                additiveActive.GetBoolean())
            {
                if (additiveNode.TryGetProperty("clip", out var additiveClipNode))
                    additiveClip = additiveClipNode.GetString()!;
                if (additiveNode.TryGetProperty("alpha", out var additiveAlphaNode))
                    additiveAlpha = additiveAlphaNode.GetSingle();
            }
            var cachedPoseName = "";
            if (value.TryGetProperty("cachedPose", out var cachedPoseNode) &&
                cachedPoseNode.ValueKind == JsonValueKind.Object &&
                cachedPoseNode.TryGetProperty("active", out var cachedActive) &&
                cachedActive.GetBoolean() &&
                cachedPoseNode.TryGetProperty("name", out var cachedNameNode))
                cachedPoseName = cachedNameNode.GetString()!;
            var layerClip = "";
            var layerAlpha = 0.0f;
            var layerMaskCount = 0;
            if (value.TryGetProperty("layered", out var layeredNode) &&
                layeredNode.ValueKind == JsonValueKind.Object &&
                layeredNode.TryGetProperty("active", out var layeredActive) &&
                layeredActive.GetBoolean())
            {
                if (layeredNode.TryGetProperty("clip", out var layerClipNode))
                    layerClip = layerClipNode.GetString()!;
                if (layeredNode.TryGetProperty("alpha", out var layerAlphaNode))
                    layerAlpha = layerAlphaNode.GetSingle();
                if (layeredNode.TryGetProperty("maskCount", out var layerMaskNode))
                    layerMaskCount = layerMaskNode.GetInt32();
            }
            component.Animation.Feedback = new(value.GetProperty("clip").GetString()!,
                value.GetProperty("time").GetSingle(), value.GetProperty("duration").GetSingle(),
                value.GetProperty("completed").GetBoolean(), value.GetProperty("playing").GetBoolean(),
                value.TryGetProperty("rate", out var rate) ? rate.GetSingle() : 1,
                value.TryGetProperty("rootMotion", out var rootMotion) ? Vector(rootMotion) : Vector3.Zero,
                value.TryGetProperty("rootMotionYaw", out var rootYaw) ? rootYaw.GetSingle() : 0,
                value.TryGetProperty("rootMotionTotal", out var rootTotal) ? Vector(rootTotal) : Vector3.Zero,
                value.TryGetProperty("rootMotionRemaining", out var rootRemaining) ? Vector(rootRemaining) : Vector3.Zero,
                value.TryGetProperty("warp", out var warp) ? warp.GetString()! : "",
                notifies.ToArray(), montageId, montageLength, montageRange,
                value.TryGetProperty("syncGroup", out var syncGroup) ? syncGroup.GetString()! : "",
                value.TryGetProperty("syncPhase", out var syncPhase) ? syncPhase.GetSingle() : 0,
                value.TryGetProperty("syncMarker", out var syncMarker) ? syncMarker.GetString()! : "",
                value.TryGetProperty("syncMarkerAlpha", out var syncMarkerAlpha) ? syncMarkerAlpha.GetSingle() : 0,
                value.TryGetProperty("syncActive", out var syncActive) && syncActive.GetBoolean(),
                value.TryGetProperty("syncLeader", out var syncLeader) ? syncLeader.GetString()! : "",
                value.TryGetProperty("syncRole", out var syncRole) ? syncRole.GetString()! : "",
                value.TryGetProperty("syncMethod", out var syncMethod) ? syncMethod.GetString()! : "",
                value.TryGetProperty("syncEffectiveMethod", out var syncEffectiveMethod) ?
                    syncEffectiveMethod.GetString()! : "",
                value.TryGetProperty("syncScope", out var syncScope) ? syncScope.GetString()! : "",
                value.TryGetProperty("syncDiagnostic", out var syncDiagnostic) ?
                    syncDiagnostic.GetString()! : "",
                montageSegment, montageSegmentId, blendClip, blendWeight,
                additiveClip, additiveAlpha, cachedPoseName,
                layerClip, layerAlpha, layerMaskCount);
            component.Animation.FootPlacementFeedback = value.TryGetProperty("footPlacement", out var placement)
                ? new(placement.GetProperty("enabled").GetBoolean(), placement.GetProperty("active").GetBoolean(),
                    placement.GetProperty("pelvisOffset").GetSingle(), ReadFootPlacementFoot(placement.GetProperty("feet")[0]),
                    ReadFootPlacementFoot(placement.GetProperty("feet")[1]), placement.GetProperty("reason").GetString()!)
                : new(false, false, 0, default, default, "disabled");
            component.Animation.RootOffsetFeedback = value.TryGetProperty("rootOffset", out var rootOffset)
                ? new(rootOffset.GetProperty("enabled").GetBoolean(), rootOffset.GetProperty("active").GetBoolean(),
                    Vector(rootOffset.GetProperty("worldTranslation")), rootOffset.GetProperty("yawRadians").GetSingle(),
                    rootOffset.GetProperty("reason").GetString()!)
                : new(false, false, Vector3.Zero, 0, "disabled");
            component.Animation.SlopeWarpingFeedback = value.TryGetProperty("slopeWarping", out var slopeWarping)
                ? new(slopeWarping.GetProperty("enabled").GetBoolean(),
                    slopeWarping.GetProperty("active").GetBoolean(),
                    slopeWarping.GetProperty("slopeAngle").GetSingle(),
                    slopeWarping.GetProperty("targetSlopeAngle").GetSingle(),
                    slopeWarping.GetProperty("appliedSlopeAngle").GetSingle(),
                    slopeWarping.GetProperty("weight").GetSingle(),
                    slopeWarping.GetProperty("pelvisOffset").GetSingle(),
                    Vector(slopeWarping.GetProperty("groundNormal")),
                    slopeWarping.GetProperty("supportIdentity").GetString()!,
                    slopeWarping.GetProperty("reason").GetString()!)
                : new(false, false, 0, 0, 0, 0, 0, new Vector3(0, 1, 0), "", "disabled");
            if (value.TryGetProperty("poseDriver", out var poseDriver))
            {
                var curves = new Dictionary<string, float>(StringComparer.Ordinal);
                if (poseDriver.TryGetProperty("curves", out var curveValues))
                    foreach (var curve in curveValues.EnumerateObject())
                        curves[curve.Name] = curve.Value.GetSingle();
                var diagnostics = new List<PoseDriverDiagnostic>();
                if (poseDriver.TryGetProperty("diagnostics", out var diagnosticValues))
                    foreach (var diagnostic in diagnosticValues.EnumerateArray())
                        diagnostics.Add(new(diagnostic.GetProperty("driver").GetString()!,
                            diagnostic.GetProperty("code").GetString()!,
                            diagnostic.GetProperty("detail").GetString()!));
                component.Animation.PoseDriverFeedback = new(
                    poseDriver.GetProperty("enabled").GetBoolean(),
                    poseDriver.GetProperty("active").GetBoolean(),
                    poseDriver.GetProperty("solvedDrivers").GetInt32(),
                    poseDriver.GetProperty("asset").GetString()!,
                    poseDriver.GetProperty("digest").GetString()!,
                    curves, diagnostics);
            }
            else
                component.Animation.PoseDriverFeedback = new(false, false, 0, "", "",
                    new Dictionary<string, float>(StringComparer.Ordinal),
                    Array.Empty<PoseDriverDiagnostic>());
            if (value.TryGetProperty("postProcess", out var postProcess))
            {
                var postDiagnostics = new List<PostProcessDiagnostic>();
                if (postProcess.TryGetProperty("diagnostics", out var postDiagnosticValues))
                    foreach (var diagnostic in postDiagnosticValues.EnumerateArray())
                        postDiagnostics.Add(new(diagnostic.GetProperty("entry").GetString()!,
                            diagnostic.GetProperty("code").GetString()!,
                            diagnostic.GetProperty("detail").GetString()!));
                component.Animation.PostProcessFeedback = new(
                    postProcess.GetProperty("enabled").GetBoolean(),
                    postProcess.GetProperty("asset").GetString()!,
                    postProcess.GetProperty("digest").GetString()!,
                    postProcess.GetProperty("entries").GetInt32(),
                    postProcess.GetProperty("twistEntries").GetInt32(),
                    postProcess.GetProperty("helperEntries").GetInt32(),
                    postProcess.GetProperty("constraintEntries").GetInt32(),
                    postProcess.GetProperty("applications").GetInt64(),
                    postProcess.GetProperty("rotatedBones").GetInt32(),
                    postProcess.GetProperty("maxTwistDegrees").GetSingle(),
                    postDiagnostics);
            }
            else
                component.Animation.PostProcessFeedback = new(false, "", "", 0, 0, 0, 0, 0, 0, 0,
                    Array.Empty<PostProcessDiagnostic>());
            component.Animation.BonePhysicsFeedback = value.TryGetProperty("bonePhysics", out var bonePhysics)
                ? new(bonePhysics.GetProperty("enabled").GetBoolean(),
                    bonePhysics.GetProperty("active").GetBoolean(),
                    bonePhysics.GetProperty("simulatedBodies").GetInt32(),
                    bonePhysics.GetProperty("constraints").GetInt32(),
                    bonePhysics.GetProperty("maxDisplacement").GetSingle(),
                    bonePhysics.GetProperty("reason").GetString()!,
                    ReadBonePhysicsBodies(bonePhysics))
                : default;
            component.Animation.MotionFeedback = value.TryGetProperty("motion", out var motion)
                ? new(motion.GetProperty("databaseIdentity").GetString()!, motion.GetProperty("queryReady").GetBoolean(),
                    motion.GetProperty("decision").GetString()!, motion.GetProperty("searchCount").GetInt64(),
                    motion.GetProperty("selectedClip").GetString()!, motion.GetProperty("selectedTime").GetSingle(),
                    motion.TryGetProperty("speedMatching", out var speedMatching)
                        ? new(speedMatching.GetProperty("enabled").GetBoolean(), speedMatching.GetProperty("active").GetBoolean(),
                            speedMatching.GetProperty("sourceSpeed").GetSingle(), speedMatching.GetProperty("actualSpeed").GetSingle(),
                            speedMatching.GetProperty("targetRate").GetSingle(), speedMatching.GetProperty("rate").GetSingle(),
                            speedMatching.GetProperty("residualSpeed").GetSingle(), speedMatching.GetProperty("reason").GetString()!)
                        : new(false, false, 0, 0, 1, 1, 0, "disabled"))
                : default;
        }
    }

    private static FootPlacementFootState ReadFootPlacementFoot(JsonElement foot) =>
        new(foot.GetProperty("grounded").GetBoolean(), foot.GetProperty("planted").GetBoolean(),
            Vector(foot.GetProperty("anchor")), Vector(foot.GetProperty("preSole")), Vector(foot.GetProperty("postSole")),
            foot.GetProperty("targetError").GetSingle(), foot.GetProperty("supportIdentity").GetString()!,
            Vector(foot.GetProperty("groundPoint")), Vector(foot.GetProperty("groundNormal")),
            Vector(foot.GetProperty("probeOrigin")), Vector(foot.GetProperty("probeDisplacement")),
            foot.GetProperty("heelGrounded").GetBoolean(), Vector(foot.GetProperty("preHeel")),
            Vector(foot.GetProperty("postHeel")), Vector(foot.GetProperty("heelGroundPoint")),
            Vector(foot.GetProperty("heelGroundNormal")), Vector(foot.GetProperty("heelProbeOrigin")),
            Vector(foot.GetProperty("heelProbeDisplacement")), foot.GetProperty("heelSupportIdentity").GetString()!,
            foot.GetProperty("contactMode").GetString()!);

    // 桥接形状：3 个数 = 位置；7 个数 = 位置 + UE 四元数 (x,y,z,w)。
    // 原生 ParseMontageTargets 按数组长度区分，旧 3 分量调用与带旋转调用共用同一字段。
    private static float[] DescribeWarpTarget(WarpTarget target)
    {
        if (target.Rotation is not { } rotation)
            return new[] { target.Location.X, target.Location.Y, target.Location.Z };
        return new[]
        {
            target.Location.X, target.Location.Y, target.Location.Z,
            rotation.X, rotation.Y, rotation.Z, rotation.W
        };
    }

    private static object[] TakeAnimationCommands(Blueprint owner)
    {
        var commands = new List<object>();
        foreach (var mesh in owner.Components.OfType<MeshComponent>())
        {
            if (mesh.Animation.Pending is { } request)
            {
                if (request.Database.Length > 0)
                    commands.Add(new { component = mesh.Id, mode = "motionMatching", database = request.Database,
                        allowedClips = request.AllowedClips, actionLocked = request.ActionLocked,
                        matchSpeed = request.MatchSpeed, footPlacement = request.FootPlacement,
                        inertialization = request.Inertialization, rootOffset = request.RootOffset,
                        footLock = request.FootLock });
                else if (request.Montage.Length > 0)
                    commands.Add(new { component = mesh.Id, mode = "playMontage", montage = request.Montage,
                        loop = request.Loop, rate = request.Rate, inPlace = request.InPlace,
                        startTime = request.MontageStartTime,
                        targets = request.MontageTargets?.ToDictionary(pair => pair.Key,
                            pair => DescribeWarpTarget(pair.Value),
                            StringComparer.Ordinal) ?? new Dictionary<string, float[]>() });
                else if (request.Transition)
                    commands.Add(new { component = mesh.Id, mode = "transitionTo", clip = request.Clip,
                        startTime = request.TransitionStartTime, loop = request.Loop, rate = request.Rate,
                        blendSeconds = request.BlendSeconds, inPlace = request.InPlace,
                        inertialization = request.Inertialization, syncGroup = request.SyncGroup,
                        syncPhase = request.SyncPhase });
                else
                    commands.Add(new { component = mesh.Id, clip = request.Clip, loop = request.Loop,
                        rate = request.Rate, inPlace = request.InPlace });
                mesh.Animation.Pending = null;
            }
            // BlendSpace1D 必须紧跟播放命令提交：A 由同批 play/transitionTo 选定，
            // 原生只在这个候选上更新第二片段与权重。
            if (mesh.Animation.PendingBlendSpace is { } blendSpace)
            {
                commands.Add(new { component = mesh.Id, mode = "blendSpace",
                    enabled = blendSpace.Enabled, clipA = blendSpace.ClipA,
                    clipB = blendSpace.ClipB, weight = blendSpace.Weight });
                mesh.Animation.PendingBlendSpace = null;
            }
            if (mesh.Animation.PendingAdditive is { } additive)
            {
                commands.Add(new { component = mesh.Id, mode = "additive",
                    enabled = additive.Enabled, clip = additive.Clip,
                    alpha = additive.Alpha, loop = additive.Loop });
                mesh.Animation.PendingAdditive = null;
            }
            if (mesh.Animation.PendingCachedPose is { } cachedPose)
            {
                commands.Add(new { component = mesh.Id, mode = "cachedPose",
                    enabled = cachedPose.Enabled, capture = cachedPose.Capture,
                    name = cachedPose.Name });
                mesh.Animation.PendingCachedPose = null;
            }
            if (mesh.Animation.PendingLayeredPose is { } layered)
            {
                commands.Add(new { component = mesh.Id, mode = "layeredPose",
                    enabled = layered.Enabled, clip = layered.Clip,
                    alpha = layered.Alpha, loop = layered.Loop, mask = layered.Mask });
                mesh.Animation.PendingLayeredPose = null;
            }
            // 斜率配置放在主命令之后：非 slope 命令仍保持“同一 batch 只允许一条”的
            // 旧边界，slope 作为后续候选组合应用到刚提交的播放状态上。
            if (mesh.Animation.PendingSlopeWarping is { } slope)
            {
                commands.Add(new { component = mesh.Id, mode = "configureSlopeWarping",
                    enabled = slope.Enabled, weight = slope.Weight,
                    maxSlopeDegrees = slope.MaxSlopeDegrees,
                    pelvisHeightOffset = slope.PelvisHeightOffset,
                    footCompensation = slope.FootCompensation,
                    responseSpeed = slope.ResponseSpeed, maxPelvisOffset = slope.MaxPelvisOffset });
                mesh.Animation.PendingSlopeWarping = null;
            }
            // PoseDriver 与 slope 同属后处理配置，跟随播放命令之后应用；
            // 关闭时也显式提交一次，确保播放器重建后不会继承旧的曲线输出。
            if (mesh.Animation.PendingPoseDriver is { } poseDriver)
            {
                commands.Add(new { component = mesh.Id, mode = "configurePoseDriver",
                    enabled = poseDriver.Enabled, asset = poseDriver.Asset });
                mesh.Animation.PendingPoseDriver = null;
            }
            if (mesh.Animation.PendingPostProcess is { } postProcess)
            {
                commands.Add(new { component = mesh.Id, mode = "configurePostProcess",
                    enabled = postProcess.Enabled, asset = postProcess.Asset });
                mesh.Animation.PendingPostProcess = null;
            }
            if (mesh.Animation.PendingBonePhysics is { } bonePhysics)
            {
                commands.Add(new { component = mesh.Id, mode = "configureBonePhysics",
                    enabled = bonePhysics.Enabled, asset = bonePhysics.Asset,
                    debug = bonePhysics.Debug });
                mesh.Animation.PendingBonePhysics = null;
            }
        }
        return commands.ToArray();
    }

    private static BonePhysicsBody[]? ReadBonePhysicsBodies(JsonElement bonePhysics)
    {
        if (!bonePhysics.TryGetProperty("bodies", out var bodies) || bodies.ValueKind != JsonValueKind.Array)
            return null;
        var result = new BonePhysicsBody[bodies.GetArrayLength()];
        var index = 0;
        foreach (var body in bodies.EnumerateArray())
        {
            result[index++] = new BonePhysicsBody(
                body.GetProperty("bone").GetString()!,
                body.GetProperty("parent").GetString()!,
                Vector(body.GetProperty("center")),
                ReadQuaternion(body.GetProperty("rotation")),
                body.GetProperty("radius").GetSingle(),
                body.GetProperty("halfHeight").GetSingle(),
                body.GetProperty("simulated").GetBoolean());
        }
        return result;
    }

    private static Quaternion ReadQuaternion(JsonElement value) =>
        new(value[0].GetSingle(), value[1].GetSingle(), value[2].GetSingle(), value[3].GetSingle());
}
