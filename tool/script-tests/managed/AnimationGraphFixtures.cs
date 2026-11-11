using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

// 这些夹具只验证 C# 图层：不加载骨骼、不提交真实动画请求。
public sealed class AnimationGraphFakeCharacterBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var runtime = new AnimationGraphRuntime(FakeGraph());
        var first = runtime.Update(GraphInput(0, ""));
        var second = runtime.Update(GraphInput(100, first.Clip));
        var third = runtime.Update(GraphInput(0, second.Clip));
        Transform.Position = new(ClipCode(first.Clip), ClipCode(second.Clip), ClipCode(third.Clip));
        Transform.Rotation = new(second.BlendSeconds, second.Changed ? 1 : 0, 0);
    }

    private static AnimationGraphDefinition FakeGraph() => new()
    {
        Name = "FakeCharacter",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root",
                EntryState = "Rest",
                States = new[]
                {
                    new AnimationGraphState { Name = "Rest", Clip = "Fake_Rest", Loop = true },
                    new AnimationGraphState { Name = "Move", Clip = "Fake_Move", Loop = true }
                },
                Transitions = new[]
                {
                    Transition("*", "Move", AnimationGraphCondition.Speed(20), 0, 0.1f, "Move"),
                    Transition("*", "Rest",
                        AnimationGraphCondition.Speed(20, AnimationGraphComparison.LessThan), 10,
                        0.1f, "Rest")
                }
            }
        }
    };

    private static AnimationGraphTransition Transition(string from, string to,
        AnimationGraphCondition condition, int priority, float blend, string tag) => new()
    {
        From = from, To = to, Condition = condition, Priority = priority, BlendSeconds = blend, Tag = tag
    };

    private static AnimationGraphInput GraphInput(float speed, string clip) =>
        new(true, false, Vector3.Zero, Vector3.Zero, 0, speed, speed, speed, 0, 1f / 60,
            clip, 0, 1, false, true, 0);

    private static int ClipCode(string clip) => clip switch
    {
        "Fake_Rest" => 1,
        "Fake_Move" => 2,
        _ => 0
    };
}

public sealed class AnimationGraphValidationBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        int mask = 0;
        mask |= Catch(1, DuplicateState());
        mask |= Catch(2, UnknownTarget());
        mask |= Catch(4, NestedCycle());
        mask |= Catch(8, UnreachableState());
        mask |= Catch(16, EmptyClip());
        mask |= Catch(32, InvalidThreshold());
        mask |= Catch(64, BudgetExceeded());
        // 合法图必须通过；否则 rejection 夹具会把“全都拒绝”误判成正确。
        _ = ValidGraph().Validate();
        Transform.Position = new(mask, 0, 0);
    }

    private static int Catch(int bit, AnimationGraphDefinition graph)
    {
        try
        {
            _ = graph.Validate();
            return 0;
        }
        catch (Exception)
        {
            return bit;
        }
    }

    private static AnimationGraphDefinition ValidGraph() => new()
    {
        Name = "Valid",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState { Name = "A", Clip = "Clip_A" },
                    new AnimationGraphState { Name = "B", Clip = "Clip_B" }
                },
                Transitions = new[] { SimpleTransition("A", "B") }
            }
        }
    };

    private static AnimationGraphDefinition DuplicateState() => new()
    {
        Name = "Duplicate", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState { Name = "A", Clip = "Clip_A" },
                    new AnimationGraphState { Name = "A", Clip = "Clip_B" }
                }
            }
        }
    };

    private static AnimationGraphDefinition UnknownTarget() => new()
    {
        Name = "UnknownTarget", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[] { new AnimationGraphState { Name = "A", Clip = "Clip_A" } },
                Transitions = new[] { SimpleTransition("A", "Missing") }
            }
        }
    };

    private static AnimationGraphDefinition NestedCycle() => new()
    {
        Name = "NestedCycle", RootStateMachine = "M0",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "M0", EntryState = "Sub",
                States = new[] { new AnimationGraphState { Name = "Sub", SubStateMachine = "M1" } }
            },
            new AnimationGraphStateMachine
            {
                Name = "M1", EntryState = "Sub",
                States = new[] { new AnimationGraphState { Name = "Sub", SubStateMachine = "M0" } }
            }
        }
    };

    private static AnimationGraphDefinition UnreachableState() => new()
    {
        Name = "Unreachable", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState { Name = "A", Clip = "Clip_A" },
                    new AnimationGraphState { Name = "B", Clip = "Clip_B" }
                }
            }
        }
    };

    private static AnimationGraphDefinition EmptyClip() => new()
    {
        Name = "EmptyClip", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[] { new AnimationGraphState { Name = "A", Clip = "" } }
            }
        }
    };

    private static AnimationGraphDefinition InvalidThreshold() => new()
    {
        Name = "InvalidThreshold", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[] { new AnimationGraphState { Name = "A", Clip = "Clip_A" } },
                Transitions = new[]
                {
                    new AnimationGraphTransition
                    {
                        From = "A", To = "A",
                        Condition = AnimationGraphCondition.Speed(float.NaN)
                    }
                }
            }
        }
    };

    private static AnimationGraphDefinition BudgetExceeded()
    {
        var machines = new AnimationGraphStateMachine[AnimationGraphDefinition.MaximumStateMachines + 1];
        for (int index = 0; index < machines.Length; ++index)
            machines[index] = new AnimationGraphStateMachine
            {
                Name = "M" + index,
                EntryState = "A",
                States = new[] { new AnimationGraphState { Name = "A", Clip = "Clip_A" } }
            };
        return new AnimationGraphDefinition
        {
            Name = "Budget", RootStateMachine = "M0", StateMachines = machines
        };
    }

    private static AnimationGraphTransition SimpleTransition(string from, string to) => new()
    {
        From = from, To = to, Condition = AnimationGraphCondition.Always
    };
}

// Echo 资源图的直接探针：确认资源侧图能独立构建并选出 Valley 片段。
public sealed class AnimationGraphEchoProbeBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var config = ValleyEcho.EchoLocomotionGraph.CreateConfig();
        // UE 的 Idle Loop 播 Idle_Long；改成短版 Idle 会是静默的手感退化。
        if (config.Clips.Idle != "Idle_Long")
            throw new InvalidOperationException("Echo idle loop must use Idle_Long.");
        config.Settings = new LocomotionSettings();
        var graph = LocomotionAnimationGraph.Build(config);
        var runtime = new AnimationGraphRuntime(graph);
        var decision = runtime.Update(new AnimationGraphInput(
            true, false, new Vector3(0, 0, -1), new Vector3(0, 0, -1), 0,
            200, 200, 200, 1, 1f / 60, "", 0, 1, false, true, 0));
        // 180° 起步片段的 UE StartPosition=0.20；硬切路径必须把它带到决策里。
        var starts = new Dictionary<string, float>(StringComparer.Ordinal)
        {
            ["Idle_to_Walk_Fwd_Left_180"] = 0.2f
        };
        // 相对 yaw=170°，落在 Left180 分桶（正负 180° 会被归一化到 -180，避免用边界值）。
        var left180 = new Vector3(-0.17364818f, 0, 0.98480775f);
        var turned = new AnimationGraphRuntime(graph).Update(new AnimationGraphInput(
            true, false, left180, left180, 0,
            200, 200, 200, 1, 1f / 60, "", 0, 1, false, true, 0, StartPositions: starts));
        Transform.Position = new(
            decision.Clip == "Idle_to_Walk_Fwd" ? 1 : 0,
            // UE Echo 的 Locomotion Idle→Walk 是 0 秒硬切；非零会改变起步手感。
            MathF.Abs(decision.BlendSeconds) < 1e-5f ? 1 : 0,
            decision.Transition == "StartWalk" ? 1 : 0);
        Transform.Rotation = new(
            decision.DirectionBranch == "Forward" ? 1 : 0,
            decision.StatePath == "Walk/StartForward" ? 1 : 0,
            MathF.Abs(turned.StartPosition - 0.2f) < 1e-5f ? 1 : 0);
    }
}

// 旧 LocomotionStateMachine 与新图模板的同一帧语义对照；只覆盖地面/空中一次性转移，
// 不伪造片段完成反馈，因此 Start/Stop 的内部完成仍由 clock 夹具验证。
public sealed class AnimationGraphParityBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var settings = new LocomotionSettings();
        var legacy = new LocomotionStateMachine(settings);
        var graph = LocomotionAnimationGraph.Build(new LocomotionGraphConfig { Settings = settings });
        var runtime = new AnimationGraphRuntime(graph);
        string clip = "";
        int mismatches = 0;
        int firstMismatch = -1;
        var frames = Sequence();
        for (int frame = 0; frame < frames.Length; ++frame)
        {
            var input = frames[frame];
            var expected = legacy.Update(input);
            var actual = runtime.Update(new AnimationGraphInput(
                input.Grounded, input.Sitting, input.DesiredDirection, input.VelocityDirection,
                input.FacingYawDegrees, input.DesiredSpeed, input.PlanarSpeed,
                input.MaxSpeed < 0 ? input.DesiredSpeed : input.MaxSpeed,
                input.InputMagnitude < 0 ? MathF.Min(1, input.DesiredDirection.Length()) : input.InputMagnitude,
                1f / 60, clip, 0, 1, false, true, 0));
            clip = actual.Clip;
            if (!Matches(expected, actual))
            {
                mismatches++;
                if (firstMismatch < 0) firstMismatch = frame;
            }
        }
        Transform.Position = new(mismatches, firstMismatch, 0);
    }

    private static bool Matches(LocomotionDecision expected, AnimationGraphDecision actual)
    {
        int mode = actual.Mode == AnimationGraphMode.Airborne ? 1 : 0;
        if ((int)expected.Mode != mode) return false;
        int transition = TransitionCode(actual.Transition);
        if ((int)expected.Transition != transition) return false;
        int direction = DirectionCode(actual.DirectionBranch);
        if ((int)expected.Direction != direction) return false;
        if (actual.State is "Airborne" or "Land") return true;
        int state = actual.State switch
        {
            "Idle" => 0,
            "Walk" => 1,
            "Jog" => 2,
            _ => -1
        };
        return (int)expected.State == state;
    }

    private static LocomotionInput[] Sequence() => new[]
    {
        new LocomotionInput(Vector3.Zero, 0, 0, 0, true),
        new LocomotionInput(new Vector3(0, 0, -1), 200, 200, 0, true),
        new LocomotionInput(new Vector3(0, 0, -1), 200, 200, 0, true),
        new LocomotionInput(new Vector3(0, 0, -1), 450, 450, 0, true, 1, 450),
        new LocomotionInput(new Vector3(0, 0, -1), 450, 450, 0, true, 1, 450),
        new LocomotionInput(new Vector3(0, 0, -1), 200, 200, 0, true, 1, 200),
        new LocomotionInput(Vector3.Zero, 0, 200, 0, true, 0, 200),
        new LocomotionInput(Vector3.Zero, 0, 0, 0, true, 0, 200),
        new LocomotionInput(new Vector3(0, 0, -1), 200, 200, 0, false, 1, 200),
        new LocomotionInput(new Vector3(0, 0, -1), 200, 200, 0, true, 1, 200),
        new LocomotionInput(new Vector3(0, 0, -1), 200, 200, 0, true, 1, 200)
    };

    private static int TransitionCode(string tag) => tag switch
    {
        "StartWalk" => 1,
        "StartJog" => 2,
        "StopWalk" => 3,
        "StopJog" => 4,
        "Fall" => 5,
        "Land" => 6,
        _ => 0
    };

    private static int DirectionCode(string branch) => branch switch
    {
        "Forward" => 0,
        "Left90" => 1,
        "Left180" => 2,
        "Right90" => 3,
        "Right180" => 4,
        _ => 0
    };
}

// 用假播放时钟跑完整起停/空中序列；C++ 侧在 30/60/120 Hz 下断言同一个终态。
public sealed class AnimationGraphRateProbeBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public int FrameRate { get; set; } = 60;

    private AnimationGraphRuntime runtime = null!;
    private string clip = "";
    private float clipTime;
    private float clipDuration;
    private bool loop;
    private float elapsed;

    protected override void BeginPlay()
    {
        if (FrameRate is not (30 or 60 or 120)) throw new ArgumentOutOfRangeException(nameof(FrameRate));
        runtime = new AnimationGraphRuntime(
            LocomotionAnimationGraph.Build(new LocomotionGraphConfig()));
    }

    protected override void Tick(float dt)
    {
        AdvancePlayback(dt);
        elapsed += dt;
        var decision = runtime.Update(InputAt(elapsed));
        if (decision.Changed || decision.Clip != clip)
        {
            clip = decision.Clip;
            clipTime = 0;
            clipDuration = Duration(clip);
            loop = decision.Loop;
        }
        Transform.Position = new(
            decision.State == "Idle" ? 0 : decision.State == "Walk" ? 1 :
            decision.State == "Jog" ? 2 : decision.State == "Airborne" ? 3 : 4,
            decision.Mode == AnimationGraphMode.Airborne ? 1 : 0,
            decision.Transition switch
            {
                "StartWalk" => 1, "StartJog" => 2, "StopWalk" => 3, "StopJog" => 4,
                "Fall" => 5, "Land" => 6, _ => 0
            });
        Transform.Rotation = new(clip == "Idle" ? 1 : 0, decision.Changed ? 1 : 0, elapsed);
    }

    private void AdvancePlayback(float dt)
    {
        if (clip.Length == 0 || clipDuration <= 0) return;
        clipTime += dt;
        if (loop) clipTime %= clipDuration;
    }

    private AnimationGraphInput InputAt(float time)
    {
        bool walk = time >= 0.2f && time < 1.0f;
        bool jog = time >= 1.0f && time < 1.6f;
        bool walkAgain = time >= 1.6f && time < 2.4f;
        bool decel = time >= 2.4f && time < 2.9f;
        bool airborne = time >= 3.2f && time < 3.6f;
        bool moving = walk || walkAgain || airborne;
        bool sprint = jog;
        var direction = moving ? new Vector3(0, 0, -1) : Vector3.Zero;
        float desired = sprint ? 450 : moving ? 200 : 0;
        float planar = moving ? desired : decel ? 200 : 0;
        float max = sprint ? 450 : 200;
        float input = moving || decel ? 1 : 0;
        bool grounded = !airborne;
        return new AnimationGraphInput(
            grounded, false, direction, direction, 0, desired, planar, max, input, 1f / FrameRate,
            clip, clipTime, clipDuration, !loop && clipTime >= clipDuration, true, 0);
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
}
