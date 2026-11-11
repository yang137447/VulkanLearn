using System.Numerics;

namespace VulkanLearn;

// 通用地面步态动画蓝图：只做图宿主与 AnimationPlayback 边界。
// 状态选择、方向起步、停步和 Fall/Land 全部由图定义表达；角色蓝图只提交输入。
public class LocomotionAnimationBlueprint : AnimationBlueprint
{
    [Expose(InstanceEditable = true)] public float WalkSpeed { get; set; } = 200;
    [Expose(InstanceEditable = true)] public float JogSpeed { get; set; } = 450;
    [Expose(InstanceEditable = true)] public float StopSpeed { get; set; } = 10;
    [Expose(InstanceEditable = true)] public float WalkToJogEnterSpeed { get; set; } = 300;
    [Expose(InstanceEditable = true)] public float JogToWalkExitSpeed { get; set; } = 260;
    [Expose(InstanceEditable = true)] public float ForwardBoundaryDegrees { get; set; } = 45;
    [Expose(InstanceEditable = true)] public float BackwardBoundaryDegrees { get; set; } = 135;
    [Expose(InstanceEditable = true)] public bool UseUeStateConditions { get; set; } = true;
    [Expose(InstanceEditable = true)] public float MinimumMoveSpeed { get; set; } = 1;
    [Expose(InstanceEditable = true)] public float JogMaxSpeed { get; set; } = 300;
    [Expose(InstanceEditable = true)] public float JogInputMagnitude { get; set; } = 0.5f;
    [Expose(InstanceEditable = true)] public float WalkInputMagnitude { get; set; } = 0.01f;
    [Expose(InstanceEditable = true)] public float BackwardDotThreshold { get; set; } = -0.5f;
    [Expose(InstanceEditable = true)] public float JogStopSpeed { get; set; } = 200;
    // UE 的 Stop→Idle 是自动规则：整段停步片段结束才回 Idle。0 表示忠实播放完整
    // 片段；正值只用于需要更强响应的实验场景。
    [Expose(InstanceEditable = true)] public float StopTransitionMaxSeconds { get; set; }
    // 与 UE Echo_AnimBP 的 AnimStateTransitionNode 对齐：Idle→Walk/Jog 0 秒硬切。
    [Expose(InstanceEditable = true)] public float StartBlendSeconds { get; set; }
    [Expose(InstanceEditable = true)] public float WalkToJogBlendSeconds { get; set; } = 0.2f;
    [Expose(InstanceEditable = true)] public float GaitChangeBlendSeconds { get; set; } = 0.3f;
    [Expose(InstanceEditable = true)] public float StopBlendSeconds { get; set; } = 0.3f;
    [Expose(InstanceEditable = true)] public float StartToCycleBlendSeconds { get; set; } = 0.5f;
    [Expose(InstanceEditable = true)] public float StopToIdleBlendSeconds { get; set; } = 0.5f;
    [Expose(InstanceEditable = true)] public float FallBlendSeconds { get; set; } = 0.4f;
    [Expose(InstanceEditable = true)] public float LandBlendSeconds { get; set; } = 0.1f;
    [Expose(InstanceEditable = true)] public float LandToLoopBlendSeconds { get; set; }
    [Expose(InstanceEditable = true)] public bool InertializationOnStop { get; set; } = true;
    [Expose(InstanceEditable = true)] public bool InertializationOnWalkToJog { get; set; } = true;
    // T3D 里 Start*→Cycles 是 0.5 秒 Crossfade，但起步片段末帧静止会让脚"停一下再起步"；
    // 这里默认改走惯性化（源速度取片段出帧速度），设 false 可回到交叉淡入。
    [Expose(InstanceEditable = true)] public bool InertializationOnStartToCycle { get; set; } = true;
    // 片段默认原地播放；带根位移的资产需要调用方显式关闭。
    [Expose(InstanceEditable = true)] public bool InPlace { get; set; } = true;

    private readonly Dictionary<string, float> curveValues = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, float> startPositions = new(StringComparer.Ordinal);
    private LocomotionInput? pendingInput;
    private LocomotionDecision decision;
    private AnimationGraphDecision graphDecision;
    private AnimationGraphRuntime? graphRuntime;
    private IReadOnlyList<string> requiredCurves = Array.Empty<string>();
    private string speedCurveName = "";
    private string walkRotationCurveName = "";
    private string jogRotationCurveName = "";
    private LocomotionState lastGroundedState = LocomotionState.Idle;
    private LocomotionDirection lastDirection = LocomotionDirection.Forward;
    private string actionMontageId = "";
    private bool actionActive;
    private bool actionSeen;
    private bool restoreBaseOnNextTick;
    private AnimationState actionBaseState;
    private float lastPlanarSpeed;

    public LocomotionClipSet Clips { get; } = new();
    public AnimationGraphDefinition Graph { get; private set; } = null!;
    public AnimationGraphRuntime GraphRuntime => graphRuntime ??
        throw new InvalidOperationException("Animation graph is not initialized.");
    public AnimationGraphDecision GraphDecision => graphDecision;
    public LocomotionDecision Decision => decision;
    public bool ActionActive => actionActive;
    public string ActiveAction => actionMontageId;
    public float LastPlanarSpeed => lastPlanarSpeed;

    // 当前片段在“授权速度 × 播放速率”下的地面速度（cm/s）。起步/停步片段用
    // MoveData_Speed 曲线描述加减速，循环片段是常量；缺曲线返回 0，调用方回退
    // 到步态名义速度。角色移动层用它做 speed warping，避免脚速与位移脱节。
    public float AnimationGroundSpeed()
    {
        var state = PlaybackState;
        if (string.IsNullOrEmpty(state.Clip) || speedCurveName.Length == 0 ||
            !Animation.HasCurve(state.Clip, speedCurveName)) return 0;
        float authored = Animation.CurveValue(state.Clip, speedCurveName, state.Time);
        if (!float.IsFinite(authored) || authored <= 0) return 0;
        float rate = float.IsFinite(state.Rate) && state.Rate > 0 ? state.Rate : 1;
        return authored * rate;
    }

    protected override void OnAnimationCreate()
    {
        ValidateTimings();
        // Expose 配置只在创建时读取；运行中修改不会重建状态机或片段映射。
        ConfigureClips(Clips);
        var config = CreateGraphConfig();
        speedCurveName = config.SpeedCurveName;
        walkRotationCurveName = config.WalkRotationCurveName;
        jogRotationCurveName = config.JogRotationCurveName;
        Graph = LocomotionAnimationGraph.Build(config);
        graphRuntime = new AnimationGraphRuntime(Graph);
        requiredCurves = Graph.CollectRequiredCurves();
        CacheStartPositions();
    }

    // 资源侧子类在这里换入自己的片段/曲线/时间；默认保留旧 LocomotionClipSet 映射。
    protected virtual LocomotionGraphConfig CreateGraphConfig()
    {
        var config = new LocomotionGraphConfig { Clips = Clips };
        ApplyBlueprintSettings(config);
        return config;
    }

    protected void ApplyBlueprintSettings(LocomotionGraphConfig config)
    {
        config.Settings = new LocomotionSettings
        {
            WalkSpeed = WalkSpeed,
            JogSpeed = JogSpeed,
            StopSpeed = StopSpeed,
            WalkToJogEnterSpeed = WalkToJogEnterSpeed,
            JogToWalkExitSpeed = JogToWalkExitSpeed,
            ForwardBoundaryDegrees = ForwardBoundaryDegrees,
            BackwardBoundaryDegrees = BackwardBoundaryDegrees,
            UseUeStateConditions = UseUeStateConditions,
            MinimumMoveSpeed = MinimumMoveSpeed,
            JogMaxSpeed = JogMaxSpeed,
            JogInputMagnitude = JogInputMagnitude,
            WalkInputMagnitude = WalkInputMagnitude,
            BackwardDotThreshold = BackwardDotThreshold,
            JogStopSpeed = JogStopSpeed
        };
        config.StartBlendSeconds = StartBlendSeconds;
        config.WalkToJogBlendSeconds = WalkToJogBlendSeconds;
        config.GaitChangeBlendSeconds = GaitChangeBlendSeconds;
        config.StopBlendSeconds = StopBlendSeconds;
        config.StartToCycleBlendSeconds = StartToCycleBlendSeconds;
        config.StopToIdleBlendSeconds = StopToIdleBlendSeconds;
        config.FallBlendSeconds = FallBlendSeconds;
        config.LandBlendSeconds = LandBlendSeconds;
        config.LandToLoopBlendSeconds = LandToLoopBlendSeconds;
        config.StopTransitionMaxSeconds = StopTransitionMaxSeconds;
        config.InertializationOnStop = InertializationOnStop;
        config.InertializationOnWalkToJog = InertializationOnWalkToJog;
        config.InertializationOnStartToCycle = InertializationOnStartToCycle;
    }

    // 角色蓝图在 Tick/LateTick 提交世界方向与物理状态；覆盖写，不排队。
    public void SetLocomotionInput(in LocomotionInput input)
    {
        pendingInput = input;
        lastPlanarSpeed = input.PlanarSpeed;
    }

    // 动作层入口：激活期间保留 base graph 状态但不提交 base clip；原生 Montage
    // 完成/取消后回到动作前的 locomotion 反馈继续求值。targets 仍由原生按资产
    // 声明的 warp target 键集合校验，空字典表示该 Montage 没有 warp 窗口。
    public void PlayAction(AssetId montage,
        IReadOnlyDictionary<string, WarpTarget>? targets = null, bool loop = false,
        float rate = 1, bool inPlace = true, float startTime = 0)
    {
        var info = Animation.Montage(montage);
        PlayActionCore(info.Id, targets ?? new Dictionary<string, WarpTarget>(), loop, rate,
            inPlace, startTime);
    }

    public void PlayAction(AssetId montage, IReadOnlyDictionary<string, Vector3> targets,
        bool loop = false, float rate = 1, bool inPlace = true, float startTime = 0)
    {
        var info = Animation.Montage(montage);
        var converted = new Dictionary<string, WarpTarget>(StringComparer.Ordinal);
        foreach (var pair in targets) converted[pair.Key] = new WarpTarget(pair.Value);
        PlayActionCore(info.Id, converted, loop, rate, inPlace, startTime);
    }

    public void PlayAction(string montageId,
        IReadOnlyDictionary<string, WarpTarget>? targets = null, bool loop = false,
        float rate = 1, bool inPlace = true, float startTime = 0)
    {
        if (string.IsNullOrEmpty(montageId))
            throw new ArgumentException("Animation montage id is required.", nameof(montageId));
        PlayActionCore(montageId, targets ?? new Dictionary<string, WarpTarget>(), loop, rate,
            inPlace, startTime);
    }

    private void PlayActionCore(string montageId, IReadOnlyDictionary<string, WarpTarget> targets,
        bool loop, float rate, bool inPlace, float startTime)
    {
        if (loop)
            throw new ArgumentException(
                $"Animation montage '{montageId}' does not support looping playback.", nameof(loop));
        if (!float.IsFinite(rate) || rate <= 0 || rate > 4)
            throw new ArgumentOutOfRangeException(nameof(rate));
        if (!float.IsFinite(startTime) || startTime < 0)
            throw new ArgumentOutOfRangeException(nameof(startTime));
        if (!actionActive) actionBaseState = PlaybackState;
        PlayMontageAction(montageId, targets, loop, rate, inPlace, startTime);
        actionMontageId = montageId;
        actionActive = true;
        actionSeen = false;
        restoreBaseOnNextTick = false;
    }

    // 取消只撤销动作层；下一 LateTick 用动作前反馈重新提交 base 决策。
    public void CancelAction()
    {
        if (!actionActive) return;
        actionActive = false;
        actionSeen = false;
        actionMontageId = "";
        restoreBaseOnNextTick = true;
    }

    // 资源侧子类可覆写片段配置（例如 Idle_Long），不复制决策逻辑。
    protected virtual void ConfigureClips(LocomotionClipSet clips) { }

    // 播放提交与反馈读取是仅有的原生动画边界；拆成虚拟方法后，纯逻辑测试可以
    // 用假时钟验证“起步片段被新步态打断”等时序规则，而不必加载真实骨骼资产。
    // 片段一律 1 倍速播放（与 UE AnimBP 一致）：步频由片段自身决定，位移层再按
    // MoveData_Speed 适配，否则循环片段被按速度缩放后步频会和起步片段明显不同。
    protected virtual void PlayClip(string clip, bool loop) => Animation.Play(clip, loop, 1, InPlace);
    protected virtual void PlayTransition(string clip, bool loop, float blendSeconds, bool inertialization,
        float startTime, string syncGroup, float syncPhase) =>
        Animation.PlayTransition(clip, startTime, loop, blendSeconds, InPlace, 1, inertialization,
            syncGroup, syncPhase);
    protected virtual void PlayMontageAction(string montageId,
        IReadOnlyDictionary<string, WarpTarget> targets, bool loop, float rate,
        bool inPlace, float startTime) =>
        Animation.PlayMontage(montageId, targets, loop, rate, inPlace, startTime);
    protected virtual AnimationState PlaybackState => Animation.State;

    protected override void OnAnimationLateTick(float deltaTime)
    {
        bool restoreBase = restoreBaseOnNextTick;
        restoreBaseOnNextTick = false;
        if (actionActive)
        {
            var live = PlaybackState;
            // 只在看到“新动作已真正开始”的反馈后进入完成判定。上一次动作留下的
            // Montage id + Completed 反馈会晚一帧到达，不能把它当成新动作已结束。
            if (!actionSeen && live.Montage == actionMontageId &&
                live.Playing && !live.Completed)
                actionSeen = true;
            if (actionSeen &&
                (string.IsNullOrEmpty(live.Montage) || live.Completed || !live.Playing))
            {
                // 原生完成：本帧继续跑 base 恢复，不能先返回，否则 base graph
                // 会晚一帧才重新提交，Montage 终帧会被误认为仍在动作层。
                actionActive = false;
                actionSeen = false;
                actionMontageId = "";
                restoreBase = true;
            }
            else return;
        }

        if (pendingInput is not { } input)
        {
            if (restoreBase)
            {
                if (graphDecision.Clip.Length > 0) ApplyDecision(decision);
                else if (!string.IsNullOrEmpty(actionBaseState.Clip))
                    PlayClip(actionBaseState.Clip, true);
            }
            return;
        }
        pendingInput = null;
        // 动作结束与取消都从动作前的 base 反馈求值；当前 PlaybackState 仍是
        // Montage clip/time，直接喂给图会把 slot 片段误当成 locomotion 状态。
        var playback = restoreBase ? actionBaseState : PlaybackState;
        BuildCurveValues(input, playback);
        string clip = playback.Clip ?? "";
        var graphInput = new AnimationGraphInput(
            input.Grounded,
            input.Sitting,
            input.DesiredDirection,
            input.VelocityDirection,
            input.FacingYawDegrees,
            input.DesiredSpeed,
            input.PlanarSpeed,
            EffectiveMaxSpeed(input),
            EffectiveInputMagnitude(input),
            deltaTime,
            clip,
            playback.Time,
            playback.Duration,
            playback.Completed,
            playback.Playing,
            playback.SyncPhase,
            playback.SyncMarker,
            playback.SyncMarkerAlpha,
            curveValues,
            startPositions);
        graphDecision = GraphRuntime.Update(graphInput);
        decision = ToLocomotionDecision(graphDecision, input);
        ApplyDecision(decision);
    }

    protected virtual void ApplyDecision(in LocomotionDecision next)
    {
        // 动作激活时 graph 可继续求值，但绝不提交 base clip；整身 Montage 是
        // 当前 DefaultSlot 的唯一姿态输出。
        if (actionActive) return;
        var playback = PlaybackState;
        // 非循环片段播完后原生 Playing=false，但状态仍应保持末帧；这时不能因为
        // "没在播放" 就重新 Play，否则 0.3s 的 Jump_Idle_Fall 会被反复重启。
        bool clipMatches = playback.Clip == graphDecision.Clip;
        if (!graphDecision.Changed && clipMatches) return;
        // 硬切也可能携带片段入口时间（UE 的 Walk/Jog Stop 与 180° 起步）。
        // 原生 Play 没有 startTime，只有 TransitionTo 能落在指定相位，所以
        // "有入口时间" 也走 transitionTo，并用 0 秒混合保持硬切语义。
        if (graphDecision.Crossfade || graphDecision.StartPosition > 0)
        {
            string group = graphDecision.Crossfade ? graphDecision.SyncGroup : "";
            float phase = graphDecision.Crossfade ? graphDecision.SyncPhase : -1;
            float startTime = graphDecision.StartPosition;
            // 起步片段 → 循环片段：归一化相位会把左右脚对反（源片段结束在左脚落地
            // 之后，目标的第 0 帧却在右脚之后）。用同名 foot marker 把“距上次落脚
            // 的时间”搬到目标片段，进入循环时脚相位才连续。
            if (graphDecision.Crossfade && phase >= 0 &&
                TryResolveMarkerEntryTime(playback.Clip ?? "", playback.Time,
                    graphDecision.Clip, out float markerTime))
            {
                startTime = markerTime;
                phase = -1;
            }
            if (group.Length > 0 && !Animation.HasSyncGroup(graphDecision.Clip, group))
            {
                group = "";
                phase = -1;
            }
            PlayTransition(graphDecision.Clip, graphDecision.Loop, graphDecision.BlendSeconds,
                graphDecision.Crossfade && graphDecision.Inertialization,
                startTime, group, phase);
        }
        else PlayClip(graphDecision.Clip, graphDecision.Loop);
    }

    // 用源/目标片段共有的 sync marker 名，把“距上一次落脚的时间”映射到目标片段。
    // 循环片段任一同名 marker 等价，取第一个；缺 marker 或源片段尚未穿过一次
    // marker 时返回 false，调用方回退到归一化相位。
    private bool TryResolveMarkerEntryTime(string sourceClip, float sourceTime, string targetClip,
        out float targetTime)
    {
        targetTime = 0;
        if (string.IsNullOrEmpty(sourceClip) || sourceClip == targetClip) return false;
        return TryResolveMarkerEntry(Animation.SyncMarkers(sourceClip), sourceTime,
            Animation.SyncMarkers(targetClip), out targetTime);
    }

    // 纯函数入口：给定源/目标 marker 表，把源片段“距上次同名标记的时间”搬到目标。
    public static bool TryResolveMarkerEntry(IReadOnlyList<AnimationSyncMarker> sourceMarkers,
        float sourceTime, IReadOnlyList<AnimationSyncMarker> targetMarkers, out float targetTime)
    {
        targetTime = 0;
        if (sourceMarkers.Count == 0 || targetMarkers.Count == 0) return false;
        int previous = -1;
        for (int index = 0; index < sourceMarkers.Count; ++index)
        {
            if (sourceMarkers[index].Time > sourceTime + 1e-4f) break;
            previous = index;
        }
        if (previous < 0) return false;
        string name = sourceMarkers[previous].Name;
        float elapsed = sourceTime - sourceMarkers[previous].Time;
        if (!float.IsFinite(elapsed) || elapsed < 0) return false;
        foreach (var marker in targetMarkers)
        {
            if (!string.Equals(marker.Name, name, StringComparison.Ordinal)) continue;
            targetTime = marker.Time + elapsed;
            return float.IsFinite(targetTime) && targetTime >= 0;
        }
        return false;
    }

    private void BuildCurveValues(in LocomotionInput input, in AnimationState playback)
    {
        curveValues.Clear();
        AddCurve(speedCurveName, input.SpeedCurve);
        AddCurve(walkRotationCurveName, input.WalkRotationDeltaCurve);
        AddCurve(jogRotationCurveName, input.JogRotationDeltaCurve);
        if (string.IsNullOrEmpty(playback.Clip) || !Animation.Clips.Contains(playback.Clip)) return;
        foreach (var name in requiredCurves)
        {
            if (name.Length == 0 || curveValues.ContainsKey(name) ||
                !Animation.HasCurve(playback.Clip, name)) continue;
            curveValues[name] = Animation.CurveValue(playback.Clip, name, playback.Time);
        }

        void AddCurve(string name, float value)
        {
            if (name.Length == 0 || value < 0 || !float.IsFinite(value)) return;
            curveValues[name] = value;
        }
    }

    private static float EffectiveInputMagnitude(in LocomotionInput input) =>
        input.InputMagnitude < 0 ? MathF.Min(1, input.DesiredDirection.Length()) : input.InputMagnitude;

    private static float EffectiveMaxSpeed(in LocomotionInput input) =>
        input.MaxSpeed < 0 ? input.DesiredSpeed : input.MaxSpeed;

    private void CacheStartPositions()
    {
        startPositions.Clear();
        foreach (var machine in Graph.StateMachines)
            foreach (var state in machine.States)
                if (state.Clip.Length > 0 && !startPositions.ContainsKey(state.Clip))
                    startPositions[state.Clip] = Animation.StartPosition(state.Clip);
    }

    private LocomotionDecision ToLocomotionDecision(in AnimationGraphDecision graph, in LocomotionInput input)
    {
        LocomotionState state = graph.State switch
        {
            "Idle" => LocomotionState.Idle,
            "Walk" => LocomotionState.Walk,
            "Jog" => LocomotionState.Jog,
            // Airborne/Land 沿用最近的地面步态：UE 的 Falling/Sitting 分支同样不写
            // LocomotionState。这里不再跑第二套状态机。
            _ => lastGroundedState
        };
        if (graph.Mode == AnimationGraphMode.Grounded && graph.State is "Idle" or "Walk" or "Jog")
            lastGroundedState = state;
        LocomotionMode mode = graph.Mode == AnimationGraphMode.Airborne
            ? LocomotionMode.Airborne : LocomotionMode.Grounded;
        LocomotionTransition transition = graph.Transition switch
        {
            "StartWalk" => LocomotionTransition.StartWalk,
            "StartJog" => LocomotionTransition.StartJog,
            "StopWalk" => LocomotionTransition.StopWalk,
            "StopJog" => LocomotionTransition.StopJog,
            "Fall" => LocomotionTransition.Fall,
            "Land" => LocomotionTransition.Land,
            _ => LocomotionTransition.None
        };
        if (graph.DirectionBranch.Length > 0)
        {
            lastDirection = graph.DirectionBranch switch
            {
                "Forward" => LocomotionDirection.Forward,
                "Left90" => LocomotionDirection.Left90,
                "Left180" => LocomotionDirection.Left180,
                "Right90" => LocomotionDirection.Right90,
                "Right180" => LocomotionDirection.Right180,
                _ => lastDirection
            };
        }
        float targetSpeed = mode == LocomotionMode.Airborne
            ? input.DesiredSpeed
            : state switch
            {
                LocomotionState.Idle => 0,
                LocomotionState.Walk => WalkSpeed,
                LocomotionState.Jog => JogSpeed,
                _ => input.DesiredSpeed
            };
        return new LocomotionDecision(state, mode, transition, lastDirection, input.DesiredSpeed,
            targetSpeed, ComputeTargetYaw(input));
    }

    private static float ComputeTargetYaw(in LocomotionInput input)
    {
        float targetYaw = NormalizeDegrees(input.FacingYawDegrees);
        if (input.DesiredDirection.LengthSquared() > 1e-6f)
        {
            targetYaw = NormalizeDegrees(
                MathF.Atan2(-input.DesiredDirection.X, -input.DesiredDirection.Z) * (180f / MathF.PI));
        }
        return targetYaw;
    }

    private static float NormalizeDegrees(float degrees)
    {
        float normalized = degrees % 360f;
        if (normalized >= 180f) normalized -= 360f;
        if (normalized < -180f) normalized += 360f;
        return normalized;
    }

    protected override void OnAnimationEndPlay(EndPlayReason reason)
    {
        actionActive = false;
        actionSeen = false;
        actionMontageId = "";
        restoreBaseOnNextTick = false;
        base.OnAnimationEndPlay(reason);
    }

    private void ValidateTimings()
    {
        if (!float.IsFinite(StopTransitionMaxSeconds) || StopTransitionMaxSeconds < 0 ||
            StopTransitionMaxSeconds > 10)
            throw new ArgumentOutOfRangeException(nameof(StopTransitionMaxSeconds));
        ValidateBlend(StartBlendSeconds, nameof(StartBlendSeconds));
        ValidateBlend(WalkToJogBlendSeconds, nameof(WalkToJogBlendSeconds));
        ValidateBlend(GaitChangeBlendSeconds, nameof(GaitChangeBlendSeconds));
        ValidateBlend(StopBlendSeconds, nameof(StopBlendSeconds));
        ValidateBlend(StartToCycleBlendSeconds, nameof(StartToCycleBlendSeconds));
        ValidateBlend(StopToIdleBlendSeconds, nameof(StopToIdleBlendSeconds));
        ValidateBlend(FallBlendSeconds, nameof(FallBlendSeconds));
        ValidateBlend(LandBlendSeconds, nameof(LandBlendSeconds));
        ValidateBlend(LandToLoopBlendSeconds, nameof(LandToLoopBlendSeconds));
    }

    private static void ValidateBlend(float value, string name)
    {
        if (!float.IsFinite(value) || value < 0 || value > 10)
            throw new ArgumentOutOfRangeException(name);
    }
}
