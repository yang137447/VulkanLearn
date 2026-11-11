namespace VulkanLearn;

// 可复用的地面/空中 locomotion 图模板。片段名、时间、曲线名和阈值全部由资源侧
// LocomotionGraphConfig 提供；runtime 不包含 Echo 的片段字符串。
public sealed class LocomotionGraphConfig
{
    public string GraphName { get; set; } = "Locomotion";
    public LocomotionClipSet Clips { get; set; } = new();
    public LocomotionSettings Settings { get; set; } = new();
    public string SyncGroup { get; set; } = "Locomotion";
    public string SpeedCurveName { get; set; } = "MoveData_Speed";
    public string WalkRotationCurveName { get; set; } = "MoveData_WalkRotationDelta";
    public string JogRotationCurveName { get; set; } = "MoveData_JogRotationDelta";
    // 默认值对齐 UE Echo_AnimBP；资源侧可以逐项覆盖。
    public float StartBlendSeconds { get; set; }
    public float WalkToJogBlendSeconds { get; set; } = 0.2f;
    public float GaitChangeBlendSeconds { get; set; } = 0.3f;
    public float StopBlendSeconds { get; set; } = 0.3f;
    public float StartToCycleBlendSeconds { get; set; } = 0.5f;
    public float StopToIdleBlendSeconds { get; set; } = 0.5f;
    public float FallBlendSeconds { get; set; } = 0.4f;
    public float LandBlendSeconds { get; set; } = 0.1f;
    public float LandToLoopBlendSeconds { get; set; }
    // UE Main States 的 Land 有两条出口：片段播完（自动规则）与
    // GetRelevantAnimTime > 0.05 && Speed > 100 的高速提前退出。
    // speed <= 0 表示关闭提前退出，只保留片段完成。
    public float LandEarlyExitSeconds { get; set; } = 0.05f;
    public float LandEarlyExitSpeed { get; set; } = 100;
    public float StopTransitionMaxSeconds { get; set; }
    public bool InertializationOnStop { get; set; } = true;
    public bool InertializationOnWalkToJog { get; set; } = true;
    // T3D 里 Start*→Cycles 是 0.5 秒 Crossfade（UE 只有 LogicType=TLT_Inertialization
    // 才走惯性化），但那个交叉淡入本身有缺陷：一次性起步片段播完后停在末帧，
    // 交叉淡入的前几帧源姿态是静止的，脚会"停一下再起步"。
    // 这里默认改成惯性化：源速度取自片段自身的出帧速度（InertializationFromCompletedOneShot），
    // 0.5 秒窗口内只做 offset 衰减，末帧速度连续地接到循环片段。显式设为 false 可回到交叉淡入。
    public bool InertializationOnStartToCycle { get; set; } = true;
}

public static class LocomotionAnimationGraph
{
    private const string IdleState = "Idle";
    private const string WalkState = "Walk";
    private const string JogState = "Jog";
    private const string AirborneState = "Airborne";
    private const string LandState = "Land";
    private const string IdleLoopState = "IdleLoop";
    private const string WalkStopState = "WalkStop";
    private const string JogStopState = "JogStop";
    private const string CyclesState = "Cycles";
    private const string StartForwardState = "StartForward";
    private const string StartLeft90State = "StartLeft90";
    private const string StartLeft180State = "StartLeft180";
    private const string StartRight90State = "StartRight90";
    private const string StartRight180State = "StartRight180";
    private const string ForwardBranch = "Forward";
    private const string Left90Branch = "Left90";
    private const string Left180Branch = "Left180";
    private const string Right90Branch = "Right90";
    private const string Right180Branch = "Right180";

    public static AnimationGraphDefinition Build(LocomotionGraphConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);
        ArgumentNullException.ThrowIfNull(config.Clips);
        ArgumentNullException.ThrowIfNull(config.Settings);
        config.Settings.Validate();
        if (!float.IsFinite(config.LandEarlyExitSeconds) || config.LandEarlyExitSeconds < 0 ||
            config.LandEarlyExitSeconds > 10)
            throw new ArgumentOutOfRangeException(nameof(config), "Land early exit time must be in [0,10].");
        if (!float.IsFinite(config.LandEarlyExitSpeed) || config.LandEarlyExitSpeed < 0)
            throw new ArgumentOutOfRangeException(nameof(config), "Land early exit speed must be non-negative.");
        var clips = config.Clips;
        var settings = config.Settings;

        var idleMachine = BuildIdleMachine(config);
        var walkMachine = BuildGaitMachine(config, WalkState, clips.Walk, clips.WalkStarts,
            config.WalkRotationCurveName, "StartWalk");
        var jogMachine = BuildGaitMachine(config, JogState, clips.Jog, clips.JogStarts,
            config.JogRotationCurveName, "StartJog");

        var idleCondition = BuildIdleCondition(config);
        var jogCondition = BuildJogCondition(config, idleCondition);
        var walkCondition = BuildWalkCondition(config, idleCondition, jogCondition);
        var jogExitCondition = BuildJogExitCondition(config, idleCondition, walkCondition);
        var ground = AnimationGraphCondition.Grounded;

        var rootTransitions = new List<AnimationGraphTransition>();
        AddAnyToAirborne(rootTransitions, config);

        rootTransitions.Add(Transition("Idle", WalkState,
            AnimationGraphCondition.All(ground, walkCondition), 30, config.StartBlendSeconds, tag: ""));
        rootTransitions.Add(Transition("Idle", JogState,
            AnimationGraphCondition.All(ground, jogCondition), 20, config.StartBlendSeconds, tag: ""));
        rootTransitions.Add(Transition(WalkState, JogState,
            AnimationGraphCondition.All(ground, jogCondition), 20, config.WalkToJogBlendSeconds,
            inertialization: config.InertializationOnWalkToJog, syncGroup: config.SyncGroup,
            syncPhaseFromSource: true));
        rootTransitions.Add(Transition(WalkState, IdleState,
            AnimationGraphCondition.All(ground, idleCondition), 10, config.StopBlendSeconds,
            inertialization: true));
        rootTransitions.Add(Transition(JogState, WalkState,
            AnimationGraphCondition.All(ground, jogExitCondition), 20, config.GaitChangeBlendSeconds,
            syncGroup: config.SyncGroup, syncPhaseFromSource: true));
        rootTransitions.Add(Transition(JogState, IdleState,
            AnimationGraphCondition.All(ground, idleCondition), 10, config.StopBlendSeconds,
            inertialization: true));
        rootTransitions.Add(Transition(AirborneState, LandState, AnimationGraphCondition.Grounded,
            0, config.LandBlendSeconds, tag: "Land"));
        AddLandingTransition(rootTransitions, IdleState, idleCondition, config);
        AddLandingTransition(rootTransitions, JogState, jogCondition, config);
        AddLandingTransition(rootTransitions, WalkState, walkCondition, config);

        var rootStates = new[]
        {
            new AnimationGraphState
            {
                Name = IdleState, SubStateMachine = idleMachine.Name, Mode = AnimationGraphMode.Grounded
            },
            new AnimationGraphState
            {
                Name = WalkState, SubStateMachine = walkMachine.Name, Mode = AnimationGraphMode.Grounded
            },
            new AnimationGraphState
            {
                Name = JogState, SubStateMachine = jogMachine.Name, Mode = AnimationGraphMode.Grounded
            },
            new AnimationGraphState
            {
                // 与 UE Echo_AnimBP 的 InAir 一致：Jump_Idle_Fall 的 bLoopAnimation=False，
                // 播放一次后保持末帧直到落地；资产里没有独立的"起跳"片段。
                Name = AirborneState, Clip = clips.Fall, Loop = false,
                SyncGroup = "", Mode = AnimationGraphMode.Airborne
            },
            new AnimationGraphState
            {
                Name = LandState, Clip = clips.Land, Loop = false,
                SyncGroup = "", Mode = AnimationGraphMode.Grounded
            }
        };

        var rootMachine = new AnimationGraphStateMachine
        {
            Name = config.GraphName,
            EntryState = IdleState,
            States = rootStates,
            Transitions = rootTransitions
        };

        return new AnimationGraphDefinition
        {
            Name = config.GraphName,
            RootStateMachine = rootMachine.Name,
            DirectionBranches = BuildDirectionBranches(settings),
            StateMachines = new[] { rootMachine, idleMachine, walkMachine, jogMachine }
        }.Validate();
    }

    // Idle 内层：Entry 先按 Speed 选停步，停步片段完成后再回 Idle Loop。
    private static AnimationGraphStateMachine BuildIdleMachine(LocomotionGraphConfig config)
    {
        var settings = config.Settings;
        var clipSet = config.Clips;
        float walkStopSpeed = settings.UseUeStateConditions ? settings.MinimumMoveSpeed : settings.StopSpeed;
        var entry = new List<AnimationGraphTransition>
        {
            Transition(IdleLoopState, JogStopState,
                AnimationGraphCondition.All(StopEntrySource(),
                    AnimationGraphCondition.Speed(settings.JogStopSpeed,
                        AnimationGraphComparison.GreaterThan)),
                0, 0, tag: "StopJog"),
            Transition(IdleLoopState, WalkStopState,
                AnimationGraphCondition.All(StopEntrySource(),
                    AnimationGraphCondition.Speed(walkStopSpeed, AnimationGraphComparison.GreaterThan)),
                10, 0, tag: "StopWalk")
        };
        var transitions = new List<AnimationGraphTransition>();
        AddStopTransition(transitions, JogStopState, config);
        AddStopTransition(transitions, WalkStopState, config);
        return new AnimationGraphStateMachine
        {
            Name = config.GraphName + ".Idle",
            EntryState = IdleLoopState,
            States = new[]
            {
                new AnimationGraphState { Name = IdleLoopState, Clip = clipSet.Idle, Loop = true },
                new AnimationGraphState { Name = WalkStopState, Clip = clipSet.StopWalk, Loop = false },
                new AnimationGraphState { Name = JogStopState, Clip = clipSet.StopJog, Loop = false }
            },
            EntryTransitions = entry,
            Transitions = transitions
        };
    }

    private static void AddStopTransition(List<AnimationGraphTransition> transitions, string from,
        LocomotionGraphConfig config)
    {
        AnimationGraphCondition condition;
        if (config.StopTransitionMaxSeconds > 0)
        {
            condition = AnimationGraphCondition.Any(
                AnimationGraphCondition.StateCompleted,
                AnimationGraphCondition.StateTime(config.StopTransitionMaxSeconds));
        }
        else condition = AnimationGraphCondition.StateCompleted;
        transitions.Add(Transition(from, IdleLoopState, condition, 0, config.StopToIdleBlendSeconds,
            inertialization: config.InertializationOnStop));
    }

    // 只有从 Walk/Jog 进入 Idle 才播停步；启动时就处于 Idle 或 Land→Idle 不重新播 Stop。
    private static AnimationGraphCondition StopEntrySource() =>
        AnimationGraphCondition.Any(
            AnimationGraphCondition.EntryFromState(WalkState),
            AnimationGraphCondition.EntryFromState(JogState));

    // Walk/Jog 内层结构相同，只有片段名、方向曲线和 tag 不同。
    private static AnimationGraphStateMachine BuildGaitMachine(LocomotionGraphConfig config,
        string ownerState, string cycleClip, string[] startClips, string directionCurve,
        string startTag)
    {
        var entry = new List<AnimationGraphTransition>
        {
            BuildDirectionEntry(startTag, directionCurve, StartLeft180State, Left180Branch),
            BuildDirectionEntry(startTag, directionCurve, StartLeft90State, Left90Branch),
            BuildDirectionEntry(startTag, directionCurve, StartRight90State, Right90Branch),
            BuildDirectionEntry(startTag, directionCurve, StartRight180State, Right180Branch),
            BuildDirectionEntry(startTag, directionCurve, StartForwardState, ForwardBranch)
        };
        var transitions = new List<AnimationGraphTransition>();
        foreach (var start in new[]
                 {
                     StartForwardState, StartLeft90State, StartLeft180State, StartRight90State,
                     StartRight180State
                 })
        {
            transitions.Add(Transition(start, CyclesState, AnimationGraphCondition.StateCompleted,
                0, config.StartToCycleBlendSeconds,
                inertialization: config.InertializationOnStartToCycle,
                syncGroup: config.SyncGroup, syncPhaseFromSource: true));
        }

        return new AnimationGraphStateMachine
        {
            Name = config.GraphName + "." + ownerState,
            EntryState = CyclesState,
            DirectionCurve = directionCurve,
            States = new[]
            {
                new AnimationGraphState
                {
                    Name = StartForwardState, Clip = startClips[(int)LocomotionDirection.Forward],
                    Loop = false, Interruptible = true, DirectionBranch = ForwardBranch
                },
                new AnimationGraphState
                {
                    Name = StartLeft90State, Clip = startClips[(int)LocomotionDirection.Left90],
                    Loop = false, Interruptible = true, DirectionBranch = Left90Branch
                },
                new AnimationGraphState
                {
                    Name = StartLeft180State, Clip = startClips[(int)LocomotionDirection.Left180],
                    Loop = false, Interruptible = true, DirectionBranch = Left180Branch
                },
                new AnimationGraphState
                {
                    Name = StartRight90State, Clip = startClips[(int)LocomotionDirection.Right90],
                    Loop = false, Interruptible = true, DirectionBranch = Right90Branch
                },
                new AnimationGraphState
                {
                    Name = StartRight180State, Clip = startClips[(int)LocomotionDirection.Right180],
                    Loop = false, Interruptible = true, DirectionBranch = Right180Branch
                },
                new AnimationGraphState { Name = CyclesState, Clip = cycleClip, Loop = true }
            },
            EntryTransitions = entry,
            Transitions = transitions
        };
    }

    private static AnimationGraphTransition BuildDirectionEntry(string startTag, string directionCurve,
        string target, string branch)
    {
        // 只有 Idle→Walk/Jog 才播方向起步；Walk↔Jog 或 Land→Walk 直接进 Cycles。
        var condition = AnimationGraphCondition.All(
            AnimationGraphCondition.EntryFromState(IdleState),
            AnimationGraphCondition.Direction(branch, directionCurve));
        return Transition(CyclesState, target, condition, EntryPriority(branch), 0, tag: startTag);
    }

    private static int EntryPriority(string branch) => branch switch
    {
        Left180Branch => 0,
        Left90Branch => 10,
        Right90Branch => 20,
        Right180Branch => 30,
        _ => 40
    };

    private static void AddAnyToAirborne(List<AnimationGraphTransition> transitions,
        LocomotionGraphConfig config)
    {
        // UE 的 On Ground → InAir 是 Any State 过渡。runtime 的 SelectTransition 会
        // 跳过 To == 当前状态的规则，因此 "*" 不会把 Airborne 重入成每帧重启 Fall。
        transitions.Add(Transition("*", AirborneState, AnimationGraphCondition.Airborne,
            0, config.FallBlendSeconds, tag: "Fall"));
    }

    private static void AddLandingTransition(List<AnimationGraphTransition> transitions, string target,
        AnimationGraphCondition gait, LocomotionGraphConfig config)
    {
        // 高速落地：UE 用 GetRelevantAnimTime(0) > 0.05 且 Speed > 100 提前离开 Land，
        // 不等 2s 的 Jump_Idle_Land 播完；优先级高于片段完成规则。
        if (config.LandEarlyExitSpeed > 0)
            transitions.Add(Transition(LandState, target,
                AnimationGraphCondition.All(AnimationGraphCondition.Grounded,
                    AnimationGraphCondition.StateTime(config.LandEarlyExitSeconds,
                        AnimationGraphComparison.GreaterThan),
                    AnimationGraphCondition.Speed(config.LandEarlyExitSpeed,
                        AnimationGraphComparison.GreaterThan),
                    gait),
                10, 0));
        transitions.Add(Transition(LandState, target,
            AnimationGraphCondition.All(AnimationGraphCondition.Grounded,
                AnimationGraphCondition.StateCompleted, gait),
            20, config.LandToLoopBlendSeconds));
    }

    private static AnimationGraphCondition BuildIdleCondition(LocomotionGraphConfig config)
    {
        var settings = config.Settings;
        if (settings.UseUeStateConditions)
        {
            return AnimationGraphCondition.Any(
                AnimationGraphCondition.Sitting,
                AnimationGraphCondition.Speed(settings.MinimumMoveSpeed, AnimationGraphComparison.LessThan),
                AnimationGraphCondition.InputMagnitude(settings.WalkInputMagnitude,
                    AnimationGraphComparison.LessThan),
                AnimationGraphCondition.BackwardDot(settings.BackwardDotThreshold));
        }
        return AnimationGraphCondition.Any(
            AnimationGraphCondition.DesiredSpeed(settings.StopSpeed, AnimationGraphComparison.AtMost),
            AnimationGraphCondition.Curve(config.SpeedCurveName, settings.StopSpeed,
                AnimationGraphComparison.AtMost));
    }

    private static AnimationGraphCondition BuildJogCondition(LocomotionGraphConfig config,
        AnimationGraphCondition idle)
    {
        var settings = config.Settings;
        if (settings.UseUeStateConditions)
        {
            var evidence = AnimationGraphCondition.Any(
                AnimationGraphCondition.MaxSpeed(settings.JogMaxSpeed),
                AnimationGraphCondition.Curve(config.SpeedCurveName, settings.JogMaxSpeed));
            return AnimationGraphCondition.All(
                AnimationGraphCondition.Not(idle),
                AnimationGraphCondition.InputMagnitude(settings.JogInputMagnitude),
                evidence);
        }
        return AnimationGraphCondition.All(
            AnimationGraphCondition.Not(idle),
            AnimationGraphCondition.Any(
                AnimationGraphCondition.DesiredSpeed(settings.WalkToJogEnterSpeed),
                AnimationGraphCondition.Curve(config.SpeedCurveName, settings.WalkToJogEnterSpeed)));
    }

    private static AnimationGraphCondition BuildWalkCondition(LocomotionGraphConfig config,
        AnimationGraphCondition idle, AnimationGraphCondition jog)
    {
        // 两种判定模式都是"既非 Idle 也非 Jog"的兜底；保留独立函数是为了让图定义
        // 读起来与 UE 的三态枚举一致。
        return AnimationGraphCondition.All(AnimationGraphCondition.Not(idle),
            AnimationGraphCondition.Not(jog));
    }

    // Jog→Walk 是滞回退出：非 UE 模式只在实际/曲线速度回到 Exit 以下才降档；
    // UE 模式的状态条件本身没有滞回，直接复用 Walk 条件。
    private static AnimationGraphCondition BuildJogExitCondition(LocomotionGraphConfig config,
        AnimationGraphCondition idle, AnimationGraphCondition walk)
    {
        var settings = config.Settings;
        if (settings.UseUeStateConditions) return walk;
        return AnimationGraphCondition.All(
            AnimationGraphCondition.Not(idle),
            AnimationGraphCondition.DesiredSpeed(settings.JogToWalkExitSpeed,
                AnimationGraphComparison.AtMost),
            AnimationGraphCondition.Not(AnimationGraphCondition.Curve(config.SpeedCurveName,
                settings.JogToWalkExitSpeed, AnimationGraphComparison.GreaterThan)));
    }

    private static AnimationGraphTransition Transition(string from, string to,
        AnimationGraphCondition condition, int priority, float blendSeconds,
        bool inertialization = false, string syncGroup = "", bool syncPhaseFromSource = false,
        string tag = "")
    {
        return new AnimationGraphTransition
        {
            From = from,
            To = to,
            Condition = condition,
            Priority = priority,
            BlendSeconds = blendSeconds,
            Inertialization = inertialization,
            SyncGroup = syncGroup,
            SyncPhaseFromSource = syncPhaseFromSource,
            Tag = tag
        };
    }

    private static AnimationGraphDirectionBranch[] BuildDirectionBranches(LocomotionSettings settings)
    {
        // 顺序就是匹配优先级：先 45° 正向、再 180° 边界，最后 90° 区间。
        return new[]
        {
            new AnimationGraphDirectionBranch
            {
                Name = ForwardBranch, Minimum = -settings.ForwardBoundaryDegrees,
                Maximum = settings.ForwardBoundaryDegrees
            },
            new AnimationGraphDirectionBranch
            {
                Name = Left180Branch, Minimum = settings.BackwardBoundaryDegrees, Maximum = 180
            },
            new AnimationGraphDirectionBranch
            {
                Name = Right180Branch, Minimum = -180,
                Maximum = -settings.BackwardBoundaryDegrees
            },
            new AnimationGraphDirectionBranch
            {
                Name = Left90Branch, Minimum = settings.ForwardBoundaryDegrees,
                Maximum = settings.BackwardBoundaryDegrees
            },
            new AnimationGraphDirectionBranch
            {
                Name = Right90Branch, Minimum = -settings.BackwardBoundaryDegrees,
                Maximum = -settings.ForwardBoundaryDegrees
            }
        };
    }
}
