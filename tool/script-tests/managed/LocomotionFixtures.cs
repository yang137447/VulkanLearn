using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

// 纯决策夹具：只把状态机的离散输出写进 Transform，C++ 测试按帧读取。
// Scenario=0 用 Expose 输入单帧；1/2 跑固定序列，避免测试侧需要逐帧改属性。
public sealed class LocomotionProbeBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public int Scenario { get; set; }
    [Expose(InstanceEditable = true)] public float DesiredX { get; set; }
    [Expose(InstanceEditable = true)] public float DesiredZ { get; set; }
    [Expose(InstanceEditable = true)] public float DesiredSpeed { get; set; }
    [Expose(InstanceEditable = true)] public float PlanarSpeed { get; set; }
    [Expose(InstanceEditable = true)] public float FacingYaw { get; set; }
    [Expose(InstanceEditable = true)] public bool Grounded { get; set; } = true;
    [Expose(InstanceEditable = true)] public float InputMagnitude { get; set; } = -1;
    [Expose(InstanceEditable = true)] public float MaxSpeed { get; set; } = -1;
    [Expose(InstanceEditable = true)] public float VelocityX { get; set; }
    [Expose(InstanceEditable = true)] public float VelocityZ { get; set; }
    [Expose(InstanceEditable = true)] public float SpeedCurve { get; set; } = -1;
    [Expose(InstanceEditable = true)] public float WalkRotationDeltaCurve { get; set; } = -1;
    [Expose(InstanceEditable = true)] public float JogRotationDeltaCurve { get; set; } = -1;

    private readonly LocomotionStateMachine machine = new();
    private int frame;

    protected override void Tick(float dt)
    {
        LocomotionInput input = Scenario switch
        {
            1 => GaitSequence(frame),
            2 => HysteresisSequence(frame),
            3 => StopThresholdSequence(frame),
            _ => new(new Vector3(DesiredX, 0, DesiredZ), DesiredSpeed, PlanarSpeed, FacingYaw, Grounded,
                InputMagnitude, MaxSpeed, new Vector3(VelocityX, 0, VelocityZ), false,
                SpeedCurve, WalkRotationDeltaCurve, JogRotationDeltaCurve)
        };
        frame++;
        var decision = machine.Update(input);

        // 枚举数值在 LocomotionStateMachine.cs 中显式固定；C++ 测试按同一组常量读取。
        Transform.Position = new((float)decision.State, (float)decision.Mode, (float)decision.Transition);
        Transform.Rotation = new((float)decision.Direction, decision.TargetSpeed, decision.TargetYawDegrees);
    }

    private static LocomotionInput GaitSequence(int frame) => frame switch
    {
        < 2 => new(new Vector3(0, 0, 0), 0, 0, 0, true),
        < 4 => new(new Vector3(0, 0, -1), 200, 200, 0, true),
        < 6 => new(new Vector3(0, 0, -1), 450, 450, 0, true),
        < 8 => new(new Vector3(0, 0, -1), 200, 200, 0, true),
        // 输入归零但仍有滑行速度：应补播 Stop 片段。
        < 10 => new(new Vector3(0, 0, 0), 0, 200, 0, true),
        10 => new(new Vector3(0, 0, -1), 200, 200, 0, false),
        _ => new(new Vector3(0, 0, -1), 200, 200, 0, true)
    };

    private static LocomotionInput HysteresisSequence(int frame) => frame switch
    {
        < 2 => new(new Vector3(0, 0, 0), 0, 0, 0, true),
        < 4 => new(new Vector3(0, 0, -1), 450, 450, 0, true),
        // UE 的 Jog 条件看当前 MaxSpeed（Sprint 上限），不是实际速度；
        // 实际速度降到 280 但 MaxSpeed 仍是 450 时保持 Jog。
        < 6 => new(new Vector3(0, 0, -1), 280, 280, 0, true, 1, 450),
        // 松开 Sprint 后 MaxSpeed 回到 200，才回 Walk。
        < 8 => new(new Vector3(0, 0, -1), 250, 250, 0, true, 1, 200),
        _ => new(new Vector3(0, 0, -1), 250, 250, 0, true)
    };

    private static LocomotionInput StopThresholdSequence(int frame) => frame switch
    {
        < 2 => new(new Vector3(0, 0, -1), 200, 200, 0, true),
        // Speed > 200 时 Idle 内层应选 Jog Stop，而不是按来源状态固定选片段。
        _ => new(new Vector3(0, 0, 0), 0, 250, 0, true, 0, 200)
    };
}

// 曲线驱动步态的纯逻辑探针：按真实 dt 推进一段速度和转向曲线，验证不同刷新率
// 下不会因曲线采样时刻不同而产生反复 Walk↔Jog 抖动。
public sealed class LocomotionCurveRateProbeBlueprint : Blueprint
{
    private readonly LocomotionStateMachine machine = new();
    private float elapsed;
    private float lastChangeTime = -1;
    private LocomotionState previous = LocomotionState.Idle;

    protected override void Tick(float dt)
    {
        elapsed += dt;
        bool moving = elapsed >= 0.2f;
        float progress = Math.Clamp((elapsed - 0.2f) / 0.8f, 0, 1);
        float curve = moving ? 1 - progress : float.NaN;
        var input = new LocomotionInput(new Vector3(0, 0, -1), moving ? 200 : 0,
            moving ? 200 : 0, 0, true, 1, -1, default, false,
            moving ? 200 + 250 * progress : float.NaN, curve, curve);
        var decision = machine.Update(input);
        if (decision.State != previous)
        {
            previous = decision.State;
            lastChangeTime = elapsed;
        }
        Transform.Position = new((float)decision.State, (float)decision.Transition, lastChangeTime);
        Transform.Rotation = new((float)decision.Direction, decision.TargetSpeed, elapsed);
    }
}

public sealed class LocomotionCurveValidationBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var machine = new LocomotionStateMachine();
        int caught = 0;
        try
        {
            machine.Update(new(new Vector3(0, 0, -1), 200, 200, 0, true,
                SpeedCurve: float.PositiveInfinity));
        }
        catch (ArgumentOutOfRangeException) { caught++; }
        try
        {
            machine.Update(new(new Vector3(0, 0, -1), 200, 200, 0, true,
                WalkRotationDeltaCurve: 1.1f));
        }
        catch (ArgumentOutOfRangeException) { caught++; }
        Transform.Position = new(caught, 0, 0);
    }
}

public sealed class LocomotionValidationBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public float WalkSpeed { get; set; } = 200;
    [Expose(InstanceEditable = true)] public float JogSpeed { get; set; } = 450;
    [Expose(InstanceEditable = true)] public float WalkToJogEnterSpeed { get; set; } = 300;
    [Expose(InstanceEditable = true)] public float JogToWalkExitSpeed { get; set; } = 260;

    protected override void BeginPlay()
    {
        try
        {
            _ = new LocomotionStateMachine(new LocomotionSettings
            {
                WalkSpeed = WalkSpeed,
                JogSpeed = JogSpeed,
                WalkToJogEnterSpeed = WalkToJogEnterSpeed,
                JogToWalkExitSpeed = JogToWalkExitSpeed
            });
        }
        catch (ArgumentOutOfRangeException)
        {
            Transform.Position = new Vector3(1, 0, 0);
        }
    }
}

public sealed class LocomotionClipProbeBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var clips = new LocomotionClipSet();
        Require(clips.LoopClip(LocomotionState.Idle) == "Idle");
        Require(clips.LoopClip(LocomotionState.Walk) == "Walk_Fwd");
        Require(clips.LoopClip(LocomotionState.Jog) == "Jog_Fwd");
        Require(clips.TransitionClip(LocomotionTransition.StartWalk, LocomotionDirection.Forward) ==
            "Idle_to_Walk_Fwd");
        Require(clips.TransitionClip(LocomotionTransition.StartWalk, LocomotionDirection.Left90) ==
            "Idle_to_Walk_Fwd_Left_90");
        Require(clips.TransitionClip(LocomotionTransition.StartJog, LocomotionDirection.Right180) ==
            "Idle_to_Jog_Fwd_Right_180");
        Require(clips.TransitionClip(LocomotionTransition.StopJog, LocomotionDirection.Forward) ==
            "Jog_Fwd_to_Idle");
        Require(clips.TransitionClip(LocomotionTransition.None, LocomotionDirection.Forward) is null);
        // 全量固定 Valley 资产的实际大小写；曾出现 Idle_To_Walk_Fwd 与资产不一致，
        // 运行时会被 AnimationPlayback 判为未知 clip。
        string[] walkStarts =
        [
            "Idle_to_Walk_Fwd", "Idle_to_Walk_Fwd_Left_90", "Idle_to_Walk_Fwd_Left_180",
            "Idle_to_Walk_Fwd_Right_90", "Idle_to_Walk_Fwd_Right_180"
        ];
        string[] jogStarts =
        [
            "Idle_To_Jog_Fwd", "Idle_to_Jog_Fwd_Left_90", "Idle_to_Jog_Fwd_Left_180",
            "Idle_to_Jog_Fwd_Right_90", "Idle_to_Jog_Fwd_Right_180"
        ];
        for (int direction = 0; direction < walkStarts.Length; ++direction)
        {
            Require(clips.WalkStarts[direction] == walkStarts[direction]);
            Require(clips.JogStarts[direction] == jogStarts[direction]);
        }
        Require(clips.StopWalk == "Walk_Fwd_to_Idle");
        Require(clips.StopJog == "Jog_Fwd_to_Idle");
        Require(clips.Fall == "Jump_Idle_Fall");
        Require(clips.Land == "Jump_Idle_Land");

        // 起步→循环的相位映射：按同名 marker 的“距上次落脚时间”对齐，而不是
        // 归一化时间（后者会把左右脚对反，进入循环时出现抢脚）。
        AnimationSyncMarker[] start =
        [
            new("Foot_L", 0.73f), new("Foot_R", 1.23f), new("Foot_L", 1.74f)
        ];
        AnimationSyncMarker[] cycle =
        [
            new("Foot_L", 0.33f), new("Foot_R", 0.85f), new("Foot_L", 1.36f)
        ];
        Require(LocomotionAnimationBlueprint.TryResolveMarkerEntry(start, 2.0f, cycle, out var entry));
        Require(MathF.Abs(entry - (0.33f + 0.26f)) < 1e-4f);
        AnimationSyncMarker[] unrelated = [new("Hit", 0.5f)];
        Require(!LocomotionAnimationBlueprint.TryResolveMarkerEntry(start, 2.0f, unrelated, out _));
    }

    private static void Require(bool condition)
    {
        if (!condition) throw new InvalidOperationException("Locomotion clip set mapping mismatch.");
    }
}

// 角色蓝图只负责把最终物理状态交给动画蓝图；动画状态机不在角色类里。
public sealed class LocomotionAnimationDriverBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public float DesiredSpeed { get; set; } = 200;
    [Expose(InstanceEditable = true)] public bool Grounded { get; set; } = true;

    protected override void LateTick(float dt)
    {
        var animation = GetScriptComponent<LocomotionAnimationProbeBlueprint>("anim");
        animation.SetLocomotionInput(new(new Vector3(0, 0, -1), DesiredSpeed, DesiredSpeed, 0, Grounded));
    }
}

// 测试替身不访问 AnimationPlayback，只把 AnimBlueprint 的决策写回 owner Transform。
public sealed class LocomotionAnimationProbeBlueprint : LocomotionAnimationBlueprint
{
    protected override void ApplyDecision(in LocomotionDecision next)
    {
        Transform.Position = new((float)next.State, (float)next.Mode, (float)next.Transition);
        Transform.Rotation = new((float)next.Direction, next.TargetSpeed, next.TargetYawDegrees);
    }
}

// 假时钟动画蓝图：不提交 AnimationPlayback，用 PlayClip/PlaybackState 模拟原生
// 片段切换与完成反馈，专门验证起步片段被新步态打断、Fall 循环等时序规则。
public sealed class LocomotionClockProbeBlueprint : LocomotionAnimationBlueprint
{
    private string clip = "";
    private float time;
    private bool loop;
    private bool playing;
    private bool completed;
    private float lastBlend;

    protected override void PlayClip(string next, bool nextLoop)
    {
        clip = next;
        time = 0;
        loop = nextLoop;
        playing = true;
        completed = false;
        lastBlend = 0;
    }

    protected override void PlayTransition(string next, bool nextLoop, float blendSeconds,
        bool inertialization, float startTime, string syncGroup, float syncPhase)
    {
        // 与 AnimationPlayback.PlayTransition 保持同一约束：相位必须配组，
        // 但组可以单独出现（过渡不携带相位）。旧实现误判成非法并禁用了组件。
        if (syncPhase >= 0 && syncGroup.Length == 0)
            throw new ArgumentException("Sync phase requires a sync group.", nameof(syncPhase));
        PlayClip(next, nextLoop);
        lastBlend = blendSeconds;
    }

    protected override AnimationState PlaybackState =>
        new(clip, time, Duration(clip), completed, playing && !(completed && !loop));

    protected override void OnAnimationLateTick(float deltaTime)
    {
        if (playing && !loop)
        {
            time += deltaTime;
            completed = time >= Duration(clip);
        }
        base.OnAnimationLateTick(deltaTime);
        Transform.Position = new(Code(clip), loop ? 1 : 0, lastBlend);
    }

    private static float Duration(string id) => id switch
    {
        "Idle_to_Walk_Fwd" => 2.0666666f,
        "Idle_To_Jog_Fwd" => 2.8f,
        "Walk_Fwd_to_Idle" => 5.6666665f,
        "Jog_Fwd_to_Idle" => 3.4666667f,
        "Jump_Idle_Fall" => 0.3f,
        "Jump_Idle_Land" => 2,
        _ => 6.6666665f
    };

    private static float Code(string id) => id switch
    {
        "Idle" => 1,
        "Walk_Fwd" => 2,
        "Jog_Fwd" => 3,
        "Idle_to_Walk_Fwd" => 4,
        "Idle_To_Jog_Fwd" => 5,
        "Walk_Fwd_to_Idle" => 6,
        "Jog_Fwd_to_Idle" => 7,
        "Jump_Idle_Fall" => 8,
        "Jump_Idle_Land" => 9,
        _ => 0
    };
}

// 前三帧保持 Walk 输入，第四帧松开且实际速度已归零；用于验证起步片段被打断。
public sealed class LocomotionStartReleaseDriverBlueprint : Blueprint
{
    private int frame;

    protected override void LateTick(float deltaTime)
    {
        float desired = frame < 3 ? 200 : 0;
        frame++;
        GetScriptComponent<LocomotionClockProbeBlueprint>("anim")
            .SetLocomotionInput(new(new Vector3(0, 0, -1), desired, desired, 0, true));
    }
}

// 起步片段播到一半切 Jog：外层 Walk→Jog 带同步组但不带相位（interruptedStart），
// 用于验证过渡请求不会因同步组参数被判非法而禁用动画组件。
public sealed class LocomotionStartToJogDriverBlueprint : Blueprint
{
    private int frame;

    protected override void LateTick(float deltaTime)
    {
        bool jog = frame++ >= 3;
        float speed = jog ? 450 : 200;
        GetScriptComponent<LocomotionClockProbeBlueprint>("anim").SetLocomotionInput(
            new LocomotionInput(new Vector3(0, 0, -1), speed, speed, 0, true, 1, speed));
    }
}

// 第一帧接地，随后持续空中；用于验证 Fall 片段播完后仍循环 Fall，而不是切回地面 Idle。
public sealed class LocomotionAirborneDriverBlueprint : Blueprint
{
    private int frame;

    protected override void LateTick(float deltaTime)
    {
        bool grounded = frame++ == 0;
        GetScriptComponent<LocomotionClockProbeBlueprint>("anim")
            .SetLocomotionInput(new(new Vector3(0, 0, -1), 200, 200, 0, grounded));
    }
}

// 持续按住 Walk；用于验证 Start→Cycles 的 0.5s 过渡时间和 UE 的 0 秒 Idle→Walk 入口。
public sealed class LocomotionWalkHoldDriverBlueprint : Blueprint
{
    protected override void LateTick(float deltaTime)
    {
        GetScriptComponent<LocomotionClockProbeBlueprint>("anim")
            .SetLocomotionInput(new(new Vector3(0, 0, -1), 200, 200, 0, true, 1, 200));
    }
}

// 两帧 Walk 后松输入但保留 200 的实际速度；用于验证 Speed>1 进入 Walk Stop，
// 并由自动规则在片段结束后以 0.5s 淡入 Idle Loop。
public sealed class LocomotionStopDriverBlueprint : Blueprint
{
    private int frame;

    protected override void LateTick(float deltaTime)
    {
        bool walking = frame++ < 2;
        GetScriptComponent<LocomotionClockProbeBlueprint>("anim")
            .SetLocomotionInput(new(new Vector3(0, 0, walking ? -1 : 0),
                walking ? 200 : 0, walking ? 200 : 200, 0, true, walking ? 1 : 0, 200));
    }
}

// Action 层假时钟：保留 base clip 与 Montage 两条可观测状态，但不采样骨骼。
public sealed class LocomotionActionClockBlueprint : LocomotionAnimationBlueprint
{
    private string clip = "Idle";
    private string montage = "";
    private float clipTime;
    private float montageTime;
    private bool montagePlaying;
    private bool loop;
    private bool completed;
    private int playClipCalls;
    private int actionCalls;

    protected override void PlayClip(string next, bool nextLoop)
    {
        clip = next;
        clipTime = 0;
        montage = "";
        montagePlaying = false;
        loop = nextLoop;
        completed = false;
        playClipCalls++;
    }

    protected override void PlayTransition(string next, bool nextLoop, float blendSeconds,
        bool inertialization, float startTime, string syncGroup, float syncPhase) =>
        PlayClip(next, nextLoop);

    protected override void PlayMontageAction(string montageId,
        IReadOnlyDictionary<string, WarpTarget> targets, bool loop, float rate,
        bool inPlace, float startTime)
    {
        montage = montageId;
        montageTime = 0;
        montagePlaying = true;
        completed = false;
        actionCalls++;
    }

    protected override AnimationState PlaybackState => montagePlaying
        ? new(clip, montageTime, 1, completed, true, Montage: montage,
            MontageLength: 1, MontageSegment: 1, MontageSegmentId: "Loop")
        : new(clip, clipTime, Duration(clip), completed, true, Montage: montage,
            MontageLength: montage.Length > 0 ? 1 : 0);

    protected override void OnAnimationLateTick(float deltaTime)
    {
        if (montagePlaying)
        {
            montageTime += deltaTime;
            if (montageTime >= 1)
            {
                montageTime = 1;
                montagePlaying = false;
                completed = true;
            }
        }
        else
        {
            clipTime += deltaTime;
            if (!loop && clipTime >= Duration(clip))
            {
                clipTime = Duration(clip);
                completed = true;
            }
        }
        base.OnAnimationLateTick(deltaTime);
        Transform.Position = new(Code(clip), playClipCalls, ActionActive ? 1 : 0);
        Transform.Rotation = new(montageTime, actionCalls,
            montage == "AM_Action" ? 1 : 0);
    }

    private static float Code(string id) => id switch
    {
        "Idle" => 1,
        "Walk_Fwd" => 2,
        _ => 0
    };

    private static float Duration(string id) => id switch
    {
        "Idle" => 6.6666665f,
        "Idle_to_Walk_Fwd" => 2.0666666f,
        "Walk_Fwd" => 6.6666665f,
        _ => 1
    };
}

public sealed class LocomotionActionDriverBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public int TriggerFrame { get; set; } = 2;
    [Expose(InstanceEditable = true)] public int CancelFrame { get; set; } = -1;

    private int frame;

    protected override void LateTick(float dt)
    {
        var animation = GetScriptComponent<LocomotionActionClockBlueprint>("anim");
        if (frame == TriggerFrame) animation.PlayAction("AM_Action");
        if (frame == CancelFrame) animation.CancelAction();
        animation.SetLocomotionInput(new(new Vector3(0, 0, -1), 200, 200, 0, true));
        frame++;
    }
}
