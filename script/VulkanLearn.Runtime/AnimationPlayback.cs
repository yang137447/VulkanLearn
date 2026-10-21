using System.Text.Json;
using System.Numerics;

namespace VulkanLearn;

public readonly record struct AnimationState(string Clip, float Time, float Duration, bool Completed, bool Playing,
    float Rate = 1, Vector3 RootMotion = default, float RootMotionYaw = 0,
    Vector3 RootMotionTotal = default, Vector3 RootMotionRemaining = default, string Warp = "",
    AnimationNotify[]? Notifies = null, string Montage = "", float MontageLength = 0,
    Vector2 MontageRange = default);
// 播放期一条 notify 事件；time 是实际发生时刻，startTime/duration 始终是作者原区间。
public readonly record struct AnimationNotify(string Name, string Track, string Kind, string Phase,
    float Time, float StartTime, float Duration, bool Interrupted);
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
// catalog 的 Montage 数据；ClipId 由 loader 从 sidecar 的 `sequence` 字段派生，仅运行时诊断/播放。
public readonly record struct MontageInfo(string Id, string SequencePath, string ClipId, float Length,
    string Slot, IReadOnlyList<MontageSectionInfo> Sections, IReadOnlyList<MontageNotifyInfo> Notifies,
    IReadOnlyList<MontageWindowInfo> Windows);
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
public readonly record struct MotionMatchingState(string DatabaseIdentity, bool QueryReady, string Decision,
    long SearchCount, string SelectedClip, float SelectedTime, SpeedMatchingState SpeedMatching = default);
internal sealed record AnimationCommand(string Clip, bool Loop, float Rate, bool InPlace,
    string Database = "", string[]? AllowedClips = null, bool ActionLocked = false, bool MatchSpeed = false,
    bool FootPlacement = false, bool Inertialization = false, bool RootOffset = false,
    string FootLock = "Auto", string Montage = "", IReadOnlyDictionary<string, WarpTarget>? MontageTargets = null,
    float MontageStartTime = 0);

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
    internal string[] ClipIds = [];
    // clipId -> 曲线名（大小写不敏感）-> 键数据；只由 animation_catalog 的 assets 段填充。
    internal readonly Dictionary<string, Dictionary<string, AnimationCurve>> CurveData = new(StringComparer.Ordinal);
    internal readonly Dictionary<string, MontageInfo> Montages = new(StringComparer.Ordinal);
    public IReadOnlyList<string> Clips { get { CheckAccess(); return Array.AsReadOnly(ClipIds); } }
    public AnimationState State { get { CheckAccess(); return Feedback; } }
    public MotionMatchingState Motion { get { CheckAccess(); return MotionFeedback; } }
    public FootPlacementState FootPlacement { get { CheckAccess(); return FootPlacementFeedback; } }
    public RootOffsetState RootOffset { get { CheckAccess(); return RootOffsetFeedback; } }
    // 只播放 clip；warp 的 target/窗口完全由 Montage 资产声明，这里不再接受 warpTargets。
    public void Play(string clip, bool loop = true, float rate = 1, bool inPlace = true)
    {
        CheckRequest();
        if (!ClipIds.Contains(clip, StringComparer.Ordinal)) throw new ArgumentException($"Unknown animation clip: {clip}");
        if (!float.IsFinite(rate) || rate <= 0 || rate > 4) throw new ArgumentOutOfRangeException(nameof(rate));
        Pending = new(clip, loop, rate, inPlace);
    }

    // 任意时刻曲线采样：秒、clip-local、键间线性、区间外 clamp 到首/末值、单键常量。
    // 纯读取，不推进时钟/姿态/notify；曲线键来自一次性 catalog，不逐帧跨桥。
    public float CurveValue(string clipId, string curveName, float time)
    {
        CheckAccess();
        if (!float.IsFinite(time)) throw new ArgumentOutOfRangeException(nameof(time));
        if (string.IsNullOrEmpty(clipId))
            throw new ArgumentException("Animation clip id is required.", nameof(clipId));
        if (string.IsNullOrEmpty(curveName))
            throw new ArgumentException("Animation curve name is required.", nameof(curveName));
        if (!CurveData.TryGetValue(clipId, out var curves))
            throw new ArgumentException($"Unknown animation clip: {clipId}", nameof(clipId));
        if (!curves.TryGetValue(curveName, out var curve))
            throw new ArgumentException($"Unknown animation curve '{curveName}' for clip '{clipId}'.", nameof(curveName));
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
        if (assets.TryGetProperty("montages", out var montages) && montages.ValueKind == JsonValueKind.Array)
            foreach (var montage in montages.EnumerateArray())
            {
                var info = ReadMontage(montage);
                animation.Montages[info.Id] = info;
            }
    }

    private static MontageInfo ReadMontage(JsonElement value)
    {
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
            value.GetProperty("slot").GetString()!, sections, notifies, windows);
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
                        notify.GetProperty("interrupted").GetBoolean()));
            var montageId = "";
            var montageLength = 0.0f;
            var montageRange = default(Vector2);
            if (value.TryGetProperty("montage", out var montage) && montage.ValueKind == JsonValueKind.Object)
            {
                montageId = montage.GetProperty("id").GetString()!;
                montageLength = montage.GetProperty("length").GetSingle();
                var range = montage.GetProperty("range");
                montageRange = new(range[0].GetSingle(), range[1].GetSingle());
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
                notifies.ToArray(), montageId, montageLength, montageRange);
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
                else
                    commands.Add(new { component = mesh.Id, clip = request.Clip, loop = request.Loop,
                        rate = request.Rate, inPlace = request.InPlace });
                mesh.Animation.Pending = null;
            }
        return commands.ToArray();
    }
}
