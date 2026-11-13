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
        mask |= Catch(128, UnknownAlias());
        mask |= Catch(256, BlendTooFewSamples());
        mask |= Catch(512, BlendUnsortedThresholds());
        mask |= Catch(1024, BlendDuplicateClip());
        mask |= Catch(2048, BlendUnknownAxis());
        mask |= Catch(32768, MaxTransitionsOutOfRange());
        mask |= Catch(65536, AdditiveOnConduit());
        mask |= Catch(131072, AdditiveAlphaWithoutClip());
        mask |= Catch(262144, CachedPoseWithClip());
        mask |= Catch(1048576, LayerOnConduit());
        mask |= Catch(2097152, LayerAlphaWithoutClip());
        mask |= Catch(4194304, LayerMaskDuplicate());
        mask |= Catch(8388608, SlotMaskWithoutSlot());
        mask |= Catch(4096, ConduitWithClip());
        mask |= Catch(8192, EntryRuleOnContentState());
        mask |= Catch(16384, EntryConduitWithoutFlag());
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

    private static AnimationGraphDefinition UnknownAlias() => new()
    {
        Name = "UnknownAlias", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[] { new AnimationGraphState { Name = "A", Clip = "Clip_A" } },
                Aliases = new[]
                {
                    new AnimationGraphStateAlias { Name = "Bad", Sources = new[] { "Missing" } }
                }
            }
        }
    };

    private static AnimationGraphDefinition BlendTooFewSamples() => BlendGraph(
        new[] { new AnimationGraphBlendSample("Clip_A", 230) });

    private static AnimationGraphDefinition BlendUnsortedThresholds() => BlendGraph(
        new[]
        {
            new AnimationGraphBlendSample("Clip_A", 500),
            new AnimationGraphBlendSample("Clip_B", 230)
        });

    private static AnimationGraphDefinition BlendDuplicateClip() => BlendGraph(
        new[]
        {
            new AnimationGraphBlendSample("Clip_A", 230),
            new AnimationGraphBlendSample("Clip_A", 500)
        });

    private static AnimationGraphDefinition BlendUnknownAxis() => new()
    {
        Name = "BlendUnknownAxis", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState
                    {
                        Name = "A",
                        BlendAxis = "Direction",
                        BlendSamples = new[]
                        {
                            new AnimationGraphBlendSample("Clip_A", 230),
                            new AnimationGraphBlendSample("Clip_B", 500)
                        }
                    }
                }
            }
        }
    };

    private static AnimationGraphDefinition MaxTransitionsOutOfRange() => new()
    {
        Name = "MaxTransitionsOutOfRange", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                MaxTransitionsPerFrame = 0,
                States = new[] { new AnimationGraphState { Name = "A", Clip = "Clip_A" } }
            }
        }
    };

    private static AnimationGraphDefinition AdditiveOnConduit() => new()
    {
        Name = "AdditiveOnConduit", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState
                    {
                        Name = "A", Conduit = true, AdditiveClip = "MM_Land"
                    }
                }
            }
        }
    };

    private static AnimationGraphDefinition LayerOnConduit() => new()
    {
        Name = "LayerOnConduit", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState
                    {
                        Name = "A", Conduit = true, LayerClip = "Clip_B"
                    }
                }
            }
        }
    };

    private static AnimationGraphDefinition LayerAlphaWithoutClip() => new()
    {
        Name = "LayerAlphaWithoutClip", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState
                    {
                        Name = "A", Clip = "Clip_A", LayerAlpha = 0.5f
                    }
                }
            }
        }
    };

    private static AnimationGraphDefinition LayerMaskDuplicate() => new()
    {
        Name = "LayerMaskDuplicate", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState
                    {
                        Name = "A", Clip = "Clip_A", LayerClip = "Clip_B",
                        LayerMask = new[] { "hip", "hip" }
                    }
                }
            }
        }
    };

    private static AnimationGraphDefinition SlotMaskWithoutSlot() => new()
    {
        Name = "SlotMaskWithoutSlot", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState
                    {
                        Name = "A", Clip = "Clip_A",
                        SlotMask = new[] { "hip" }
                    }
                }
            }
        }
    };

    private static AnimationGraphDefinition AdditiveAlphaWithoutClip() => new()
    {
        Name = "AdditiveAlphaWithoutClip", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState
                    {
                        Name = "A", Clip = "Clip_A", AdditiveAlpha = 0.5f
                    }
                }
            }
        }
    };

    private static AnimationGraphDefinition CachedPoseWithClip() => new()
    {
        Name = "CachedPoseWithClip", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState
                    {
                        Name = "A", Clip = "Clip_A", UseCachedPose = "Locomotion"
                    }
                }
            }
        }
    };

    private static AnimationGraphDefinition BlendGraph(AnimationGraphBlendSample[] samples) => new()
    {
        Name = "Blend", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[] { new AnimationGraphState { Name = "A", BlendSamples = samples } }
            }
        }
    };

    // Conduit 是“无姿态过路状态”，不能同时声明 clip。
    private static AnimationGraphDefinition ConduitWithClip() => new()
    {
        Name = "ConduitWithClip", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[] { new AnimationGraphState { Name = "A", Clip = "Clip_A", Conduit = true } }
            }
        }
    };

    private static AnimationGraphDefinition EntryRuleOnContentState() => new()
    {
        Name = "EntryRuleOnContentState", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState
                    {
                        Name = "A", Clip = "Clip_A",
                        EntryRule = AnimationGraphCondition.Always
                    }
                }
            }
        }
    };

    private static AnimationGraphDefinition EntryConduitWithoutFlag() => new()
    {
        Name = "EntryConduitWithoutFlag", RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "K",
                States = new[]
                {
                    new AnimationGraphState { Name = "K", Conduit = true },
                    new AnimationGraphState { Name = "C", Clip = "Clip_C" }
                },
                Transitions = new[] { SimpleTransition("K", "C") }
            }
        }
    };
}

// State Alias + VerticalSpeed 的纯 C# 探针：A/B 都能通过共享 alias 进入 C。
public sealed class AnimationGraphAliasProbeBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var graph = BuildGraph();
        var fromA = new AnimationGraphRuntime(graph);
        var a0 = fromA.Update(MakeInput(0, 10, ""));
        var a1 = fromA.Update(MakeInput(0, 10, a0.Clip));

        var fromB = new AnimationGraphRuntime(graph);
        var b0 = fromB.Update(MakeInput(20, 0, ""));
        var b1 = fromB.Update(MakeInput(20, 0, b0.Clip));
        var b2 = fromB.Update(MakeInput(20, 10, b1.Clip));

        Transform.Position = new(
            a1.State == "C" ? 1 : 0,
            b2.State == "C" ? 1 : 0,
            b1.State == "B" ? 1 : 0);
    }

    private static AnimationGraphDefinition BuildGraph() => new()
    {
        Name = "AliasProbe",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root",
                EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState { Name = "A", Clip = "Clip_A" },
                    new AnimationGraphState { Name = "B", Clip = "Clip_B" },
                    new AnimationGraphState { Name = "C", Clip = "Clip_C" }
                },
                Aliases = new[]
                {
                    new AnimationGraphStateAlias { Name = "Airborne", Sources = new[] { "A", "B" } }
                },
                Transitions = new[]
                {
                    new AnimationGraphTransition
                    {
                        From = "A", To = "B", Condition = AnimationGraphCondition.Speed(10),
                        Priority = 10
                    },
                    new AnimationGraphTransition
                    {
                        From = "Airborne", To = "C",
                        Condition = AnimationGraphCondition.VerticalSpeed(0,
                            AnimationGraphComparison.GreaterThan),
                        Priority = 0
                    }
                }
            }
        }
    };

    private static AnimationGraphInput MakeInput(float speed, float vertical, string clip) =>
        new(true, false, Vector3.Zero, Vector3.Zero, 0, speed, speed, speed, 0,
            1f / 60, clip, 0, 1, false, true, 0,
            VerticalSpeed: vertical);
}

// BlendSpace1D 决策探针：官方 BS_MM_WalkRun 的三段 sample
// （0 = Walk_InPlace、230 = Walk_Fwd、500 = Run_Fwd）、区间括号选择，
// 以及 UE TargetWeightInterpolationSpeedPerSec=5 的权重逼近速度。
public sealed class AnimationGraphBlendSpaceProbeBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var graph = BuildGraph();
        // 首帧只走 entry transition，第二帧才做 Idle→WalkRun 的常规迁移，
        // 因此每个探针都先以 0 速度入场，再在同一 runtime 上给出目标速度。
        // 最低端点用刚过阈值的速度，权重仍在 1e-3 以内；0 速度不会离开 Idle。
        var low = Evaluate(graph, 0.001f);
        var below = Evaluate(graph, 115);
        var mid = Evaluate(graph, 365);
        var walk = Evaluate(graph, 230);
        var high = Evaluate(graph, 500);
        var above = Evaluate(graph, 900);

        var bracket = Evaluate(BuildThreeSampleGraph(), 420);

        // 230 边界两侧必须是同一个 A（Walk）且权重连续：230 为 0，230.2 仍≈0。
        var edge = new AnimationGraphRuntime(graph);
        edge.Update(Input(0));
        edge.Update(Input(230));
        var afterEdge = edge.Update(Input(230.2f));

        // 从 300 直接跳到 500：300 的稳态权重是 smoothstep(70/270)=0.1668，
        // 0.1 s 后按 5/s 最多走 0.5，因此预期 0.6668。
        var smoothed = new AnimationGraphRuntime(graph);
        smoothed.Update(Input(0));
        var steady = smoothed.Update(Input(300));
        var halfway = smoothed.Update(Input(500, 0.1f));

        Transform.Position = new(
            low.Clip == "MM_Walk_Fwd" && low.BlendClip == "MM_Walk_InPlace" &&
                MathF.Abs(low.BlendWeight - 1.0f) < 1e-3f ? 1 : 0,
            below.Clip == "MM_Walk_Fwd" && below.BlendClip == "MM_Walk_InPlace" &&
                MathF.Abs(below.BlendWeight - 0.5f) < 1e-3f ? 1 : 0,
            mid.Clip == "MM_Walk_Fwd" && mid.BlendClip == "MM_Run_Fwd" &&
                MathF.Abs(mid.BlendWeight - 0.5f) < 1e-3f ? 1 : 0);
        Transform.Rotation = new(
            walk.Clip == "MM_Walk_Fwd" && walk.BlendClip == "MM_Walk_InPlace" &&
                MathF.Abs(walk.BlendWeight) < 1e-3f &&
                afterEdge.Clip == "MM_Walk_Fwd" && afterEdge.BlendClip == "MM_Run_Fwd" &&
                MathF.Abs(afterEdge.BlendWeight) < 1e-3f ? 1 : 0,
            high.Clip == "MM_Walk_Fwd" && high.BlendClip == "MM_Run_Fwd" &&
                MathF.Abs(high.BlendWeight - 1.0f) < 1e-3f ? 1 : 0,
            above.Clip == "MM_Walk_Fwd" && above.BlendClip == "MM_Run_Fwd" &&
                MathF.Abs(above.BlendWeight - 1.0f) < 1e-3f ? 1 : 0);
        Transform.Scale = new(
            bracket.Clip == "MM_Mid" && bracket.BlendClip == "MM_Run_Fwd" &&
                MathF.Abs(bracket.BlendWeight - 0.104f) < 1e-3f ? 1 : 0,
            steady.Clip == "MM_Walk_Fwd" && steady.BlendClip == "MM_Run_Fwd" &&
                MathF.Abs(steady.BlendWeight - 0.1668f) < 1e-3f ? 1 : 0,
            halfway.Clip == "MM_Walk_Fwd" && halfway.BlendClip == "MM_Run_Fwd" &&
                MathF.Abs(halfway.BlendWeight - 0.6668f) < 1e-3f ? 1 : 0);
    }

    private static AnimationGraphDefinition BuildGraph() => Build(
        new[]
        {
            new AnimationGraphBlendSample("MM_Walk_InPlace", 0),
            new AnimationGraphBlendSample("MM_Walk_Fwd", 230),
            new AnimationGraphBlendSample("MM_Run_Fwd", 500)
        });

    private static AnimationGraphDefinition BuildThreeSampleGraph() => Build(
        new[]
        {
            new AnimationGraphBlendSample("MM_Walk_Fwd", 230),
            new AnimationGraphBlendSample("MM_Mid", 400),
            new AnimationGraphBlendSample("MM_Run_Fwd", 500)
        });

    private static AnimationGraphDefinition Build(AnimationGraphBlendSample[] samples) => new()
    {
        Name = "BlendSpaceProbe",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root",
                EntryState = "Idle",
                States = new[]
                {
                    new AnimationGraphState { Name = "Idle", Clip = "MM_Idle" },
                    new AnimationGraphState { Name = "Walk / Run", BlendSamples = samples }
                },
                Transitions = new[]
                {
                    new AnimationGraphTransition
                    {
                        From = "Idle",
                        To = "Walk / Run",
                        Condition = AnimationGraphCondition.Speed(0, AnimationGraphComparison.GreaterThan)
                    }
                }
            }
        }
    };

    private static AnimationGraphDecision Evaluate(AnimationGraphDefinition graph, float speed)
    {
        var runtime = new AnimationGraphRuntime(graph);
        runtime.Update(Input(0));
        return runtime.Update(Input(speed));
    }

    private static AnimationGraphInput Input(float speed, float deltaTime = 1f / 60) =>
        new(true, false, Vector3.Zero, Vector3.Zero, 0, speed, speed, speed, 0,
            deltaTime, "", 0, 1, false, true, 0);
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

// State Machine 设置探针：MaxTransitionsPerFrame 预算与 SkipFirstUpdateTransition 的
// “首帧转移不混合”语义（UE AnimNode_StateMachine 类默认值 3 / true）。
public sealed class AnimationGraphPoseGraphProbeBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var runtime = new AnimationGraphRuntime(BuildGraph());
        runtime.Update(Input(0));
        var cached = runtime.Update(Input(10));

        Transform.Position = new(
            Flag(cached.UseCachedPose == "Locomotion"),
            Flag(cached.AdditiveClip == "MM_Land" && MathF.Abs(cached.AdditiveAlpha - 0.5f) < 1e-4f),
            Flag(cached.Slot == "DefaultSlot"));
        Transform.Rotation = new(
            Flag(cached.Clip.Length == 0),
            Flag(cached.SaveCachedPose == "Locomotion"),
            Flag(cached.LeafState == "Cached"));
        Transform.Scale = new(
            Flag(cached.Changed),
            Flag(cached.Completed == false),
            Flag(!cached.AdditiveLoop));
    }

    private static AnimationGraphDefinition BuildGraph() => new()
    {
        Name = "PoseGraphProbe",
        RootStateMachine = "Root",
        SaveCachedPose = "Locomotion",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState { Name = "A", Clip = "MM_Idle" },
                    new AnimationGraphState
                    {
                        Name = "Cached",
                        UseCachedPose = "Locomotion",
                        AdditiveClip = "MM_Land",
                        AdditiveAlpha = 0.5f,
                        Slot = "DefaultSlot"
                    }
                },
                Transitions = new[]
                {
                    new AnimationGraphTransition
                    {
                        From = "A", To = "Cached",
                        Condition = AnimationGraphCondition.Speed(5,
                            AnimationGraphComparison.GreaterThan)
                    }
                }
            }
        }
    };

    private static AnimationGraphInput Input(float speed) =>
        new(true, false, Vector3.Zero, Vector3.Zero, 0, speed, speed, speed, 0, 1f / 60,
            "", 0, 1, false, true, 0);

    private static float Flag(bool ok) => ok ? 1 : 2;
}

public sealed class AnimationGraphSettingsProbeBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var one = new AnimationGraphRuntime(BuildChain(1, skipFirstUpdate: false));
        var first = one.Update(Input());
        var second = one.Update(Input());

        var three = new AnimationGraphRuntime(BuildChain(3, skipFirstUpdate: false));
        var threeFirst = three.Update(Input());

        var two = new AnimationGraphRuntime(BuildChain(2, skipFirstUpdate: false));
        var twoFirst = two.Update(Input());
        var twoSecond = two.Update(Input());

        var skip = new AnimationGraphRuntime(BuildChain(1, skipFirstUpdate: true));
        var skipFirst = skip.Update(Input());

        var nested = new AnimationGraphRuntime(BuildNested(skipFirstUpdate: true));
        var nestedFirst = nested.Update(Input());

        Transform.Position = new(
            Flag(first.State == "B"),
            Flag(second.State == "C"),
            Flag(threeFirst.State == "C" && MathF.Abs(threeFirst.BlendSeconds - 0.4f) < 1e-4f));
        Transform.Rotation = new(
            Flag(MathF.Abs(skipFirst.BlendSeconds) < 1e-4f),
            Flag(MathF.Abs(first.BlendSeconds - 0.4f) < 1e-4f),
            Flag(twoFirst.State == "C" && twoSecond.State == "C"));
        Transform.Scale = new(
            Flag(skipFirst.State == "B" && MathF.Abs(skipFirst.BlendSeconds) < 1e-4f),
            Flag(first.State == "B" && MathF.Abs(first.BlendSeconds - 0.4f) < 1e-4f),
            Flag(nestedFirst.LeafState == "Y" && MathF.Abs(nestedFirst.BlendSeconds) < 1e-4f));
    }

    private static AnimationGraphDefinition BuildChain(int maxTransitions, bool skipFirstUpdate) => new()
    {
        Name = "SettingsProbe",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                MaxTransitionsPerFrame = maxTransitions,
                SkipFirstUpdateTransition = skipFirstUpdate,
                States = new[]
                {
                    new AnimationGraphState { Name = "A", Clip = "Clip_A" },
                    new AnimationGraphState { Name = "B", Clip = "Clip_B" },
                    new AnimationGraphState { Name = "C", Clip = "Clip_C" }
                },
                Transitions = new[]
                {
                    new AnimationGraphTransition
                    {
                        From = "A", To = "B", Condition = AnimationGraphCondition.Always,
                        BlendSeconds = 0.4f
                    },
                    new AnimationGraphTransition
                    {
                        From = "B", To = "C", Condition = AnimationGraphCondition.Always,
                        BlendSeconds = 0.4f
                    }
                }
            }
        }
    };

    // 刚进入的内层状态机同样受 SkipFirstUpdateTransition 约束。
    private static AnimationGraphDefinition BuildNested(bool skipFirstUpdate) => new()
    {
        Name = "SettingsNestedProbe",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "L",
                States = new[] { new AnimationGraphState { Name = "L", SubStateMachine = "Inner" } }
            },
            new AnimationGraphStateMachine
            {
                Name = "Inner", EntryState = "X",
                SkipFirstUpdateTransition = skipFirstUpdate,
                States = new[]
                {
                    new AnimationGraphState { Name = "X", Clip = "Clip_X" },
                    new AnimationGraphState { Name = "Y", Clip = "Clip_Y" }
                },
                Transitions = new[]
                {
                    new AnimationGraphTransition
                    {
                        From = "X", To = "Y", Condition = AnimationGraphCondition.Always,
                        BlendSeconds = 0.4f
                    }
                }
            }
        }
    };

    private static AnimationGraphInput Input() =>
        new(true, false, Vector3.Zero, Vector3.Zero, 0, 100, 100, 100, 0, 1f / 60,
            "", 0, 1, false, true, 0);

    private static float Flag(bool ok) => ok ? 1 : 2;
}

// LayeredBoneBlend 决策探针：字段透传与 mask 顺序。
public sealed class AnimationGraphLayerProbeBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var runtime = new AnimationGraphRuntime(Build());
        var decision = runtime.Update(Input());
        Transform.Position = new(
            decision.LayerClip == "Clip_B" ? 1 : 0,
            MathF.Abs(decision.LayerAlpha - 0.25f) < 1e-4f ? 1 : 0,
            decision.LayerLoop ? 1 : 0);
        Transform.Rotation = new(
            decision.LayerMask is { Count: 2 } ? 1 : 0,
            decision.LayerMask is { Count: 2 } &&
                decision.LayerMask[0] == "hip" ? 1 : 0,
            decision.LayerMask is { Count: 2 } &&
                decision.LayerMask[1] == "spine_01" ? 1 : 0);
        Transform.Scale = new(
            decision.Slot == "DefaultSlot" ? 1 : 0,
            decision.SlotMask is { Count: 2 } &&
                decision.SlotMask[0] == "hip" ? 1 : 0,
            MathF.Abs(decision.SlotAlpha - 0.5f) < 1e-4f ? 1 : 0);
    }

    private static AnimationGraphDefinition Build() => new()
    {
        Name = "LayerProbe",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState
                    {
                        Name = "A", Clip = "Clip_A",
                        LayerClip = "Clip_B", LayerAlpha = 0.25f, LayerLoop = true,
                        LayerMask = new[] { "hip", "spine_01" },
                        Slot = "DefaultSlot",
                        SlotMask = new[] { "hip", "spine_01" },
                        SlotAlpha = 0.5f
                    }
                }
            }
        }
    };

    private static AnimationGraphInput Input() =>
        new(true, false, Vector3.Zero, Vector3.Zero, 0, 0, 0, 0, 0, 1f / 60,
            "", 0, 1, false, true, 0);
}

// BS_*.json 资产探针：图只写 BlendSpace 名字，宿主用 catalog 解析成 samples。
public sealed class AnimationGraphBlendSpaceAssetProbeBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var map = new Dictionary<string, AnimationBlendSpaceDefinition>
        {
            ["BS_WalkRun"] = new("BS_WalkRun", 5, "EaseInOut", new[]
            {
                new AnimationBlendSpaceSample("MM_Walk_InPlace", 0),
                new AnimationBlendSpaceSample("MM_Walk_Fwd", 230),
                new AnimationBlendSpaceSample("MM_Run_Fwd", 500)
            })
        };
        var resolved = BuildGraph().ResolveBlendSpaces(map).Validate();
        var low = Evaluate(resolved, 115);
        var mid = Evaluate(resolved, 365);
        var high = Evaluate(resolved, 500);

        bool unresolvedRejected = false;
        try { BuildGraph().Validate(); }
        catch (InvalidOperationException) { unresolvedRejected = true; }
        bool unknownRejected = false;
        try
        {
            BuildGraph().ResolveBlendSpaces(
                new Dictionary<string, AnimationBlendSpaceDefinition>());
        }
        catch (InvalidOperationException) { unknownRejected = true; }

        Transform.Position = new(
            Flag(low.Clip == "MM_Walk_Fwd" && low.BlendClip == "MM_Walk_InPlace" &&
                MathF.Abs(low.BlendWeight - 0.5f) < 1e-3f),
            Flag(mid.Clip == "MM_Walk_Fwd" && mid.BlendClip == "MM_Run_Fwd" &&
                MathF.Abs(mid.BlendWeight - 0.5f) < 1e-3f),
            Flag(high.Clip == "MM_Walk_Fwd" && high.BlendClip == "MM_Run_Fwd" &&
                MathF.Abs(high.BlendWeight - 1.0f) < 1e-3f));
        Transform.Rotation = new(Flag(unresolvedRejected), Flag(unknownRejected), 1);
        Transform.Scale = new(1, 1, 1);
    }

    private static AnimationGraphDefinition BuildGraph() => new()
    {
        Name = "BlendSpaceAssetProbe",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "Idle",
                States = new[]
                {
                    new AnimationGraphState { Name = "Idle", Clip = "MM_Idle" },
                    new AnimationGraphState { Name = "Walk / Run", BlendSpace = "BS_WalkRun" }
                },
                Transitions = new[]
                {
                    new AnimationGraphTransition
                    {
                        From = "Idle", To = "Walk / Run",
                        Condition = AnimationGraphCondition.Speed(0, AnimationGraphComparison.GreaterThan)
                    }
                }
            }
        }
    };

    private static AnimationGraphDecision Evaluate(AnimationGraphDefinition graph, float speed)
    {
        var runtime = new AnimationGraphRuntime(graph);
        runtime.Update(Input(0));
        return runtime.Update(Input(speed));
    }

    private static AnimationGraphInput Input(float speed) =>
        new(true, false, Vector3.Zero, Vector3.Zero, 0, speed, speed, speed, 0, 1f / 60,
            "", 0, 1, false, true, 0);

    private static float Flag(bool ok) => ok ? 1 : 2;
}

// Conduit 语义探针：同帧落地、入口规则回落、链、环、最终 blend 与入口 conduit。
public sealed class AnimationGraphConduitProbeBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var chain = new AnimationGraphRuntime(BuildChainGraph(blockedRule: false));
        chain.Update(Input(0));
        var landed = chain.Update(Input(10));

        var blocked = new AnimationGraphRuntime(BuildChainGraph(blockedRule: true));
        blocked.Update(Input(0));
        var fellBack = blocked.Update(Input(10));

        var chained = new AnimationGraphRuntime(BuildChain2Graph());
        chained.Update(Input(0));
        var chainLanded = chained.Update(Input(10));

        var cycle = new AnimationGraphRuntime(BuildCycleGraph());
        cycle.Update(Input(0));
        var stuck = cycle.Update(Input(10));

        var entry = new AnimationGraphRuntime(BuildEntryConduitGraph(hasPath: true));
        var entryFirst = entry.Update(Input(10));

        var unresolved = new AnimationGraphRuntime(BuildEntryConduitGraph(hasPath: false));
        var unresolvedFirst = unresolved.Update(Input(10));

        Transform.Position = new(
            landed.State == "C" && PathIs(landed, "K") ? 1 : 0,
            fellBack.State == "B" ? 1 : 0,
            chainLanded.State == "C" && PathIs(chainLanded, "K1", "K2") ? 1 : 0);
        Transform.Rotation = new(
            stuck.State == "A" ? 1 : 0,
            MathF.Abs(landed.BlendSeconds - 0.35f) < 1e-4f ? 1 : 0,
            entryFirst.State == "C" ? 1 : 0);
        Transform.Scale = new(
            Flag(unresolvedFirst.State == "K" && unresolvedFirst.Clip.Length == 0),
            Flag(landed.ConduitPath is { Count: 1 }),
            Flag(entryFirst.Changed));
    }

    // A -> K(conduit) -> C；K 的入口规则失败时回落到 A -> B。
    private static AnimationGraphDefinition BuildChainGraph(bool blockedRule) => new()
    {
        Name = blockedRule ? "ConduitBlocked" : "ConduitChain",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState { Name = "A", Clip = "Clip_A" },
                    new AnimationGraphState
                    {
                        Name = "K",
                        Conduit = true,
                        EntryRule = blockedRule
                            ? AnimationGraphCondition.Speed(1000, AnimationGraphComparison.GreaterThan)
                            : AnimationGraphCondition.Always
                    },
                    new AnimationGraphState { Name = "C", Clip = "Clip_C" },
                    new AnimationGraphState { Name = "B", Clip = "Clip_B" }
                },
                Transitions = new[]
                {
                    Transition("A", "K", AnimationGraphCondition.Speed(5, AnimationGraphComparison.GreaterThan),
                        0, 0.9f),
                    Transition("K", "C", AnimationGraphCondition.Always, 0, 0.35f),
                    Transition("A", "B", AnimationGraphCondition.Speed(5, AnimationGraphComparison.GreaterThan),
                        10, 0.1f)
                }
            }
        }
    };

    private static AnimationGraphDefinition BuildChain2Graph() => new()
    {
        Name = "ConduitChain2",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState { Name = "A", Clip = "Clip_A" },
                    new AnimationGraphState { Name = "K1", Conduit = true },
                    new AnimationGraphState { Name = "K2", Conduit = true },
                    new AnimationGraphState { Name = "C", Clip = "Clip_C" }
                },
                Transitions = new[]
                {
                    Transition("A", "K1", AnimationGraphCondition.Speed(5, AnimationGraphComparison.GreaterThan),
                        0, 0),
                    Transition("K1", "K2", AnimationGraphCondition.Always, 0, 0),
                    Transition("K2", "C", AnimationGraphCondition.Always, 0, 0)
                }
            }
        }
    };

    // K1 与 K2 成环且没有内容出口：visited 必须终止求值，源状态保持 A。
    private static AnimationGraphDefinition BuildCycleGraph() => new()
    {
        Name = "ConduitCycle",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "A",
                States = new[]
                {
                    new AnimationGraphState { Name = "A", Clip = "Clip_A" },
                    new AnimationGraphState { Name = "K1", Conduit = true },
                    new AnimationGraphState { Name = "K2", Conduit = true }
                },
                Transitions = new[]
                {
                    Transition("A", "K1", AnimationGraphCondition.Always, 0, 0),
                    Transition("K1", "K2", AnimationGraphCondition.Always, 0, 0),
                    Transition("K2", "K1", AnimationGraphCondition.Always, 0, 0)
                }
            }
        }
    };

    // EntryState 就是 conduit：允许跟随且无可用出边时停在该 conduit，不输出片段。
    private static AnimationGraphDefinition BuildEntryConduitGraph(bool hasPath) => new()
    {
        Name = hasPath ? "ConduitEntryPath" : "ConduitEntryStuck",
        RootStateMachine = "Root",
        StateMachines = new[]
        {
            new AnimationGraphStateMachine
            {
                Name = "Root", EntryState = "K", AllowConduitEntryStates = true,
                States = new[]
                {
                    new AnimationGraphState { Name = "K", Conduit = true },
                    new AnimationGraphState { Name = "C", Clip = "Clip_C" }
                },
                Transitions = new[]
                {
                    Transition("K", "C", hasPath
                        ? AnimationGraphCondition.Always
                        : AnimationGraphCondition.Speed(1000, AnimationGraphComparison.GreaterThan), 0, 0)
                }
            }
        }
    };

    private static AnimationGraphTransition Transition(string from, string to,
        AnimationGraphCondition condition, int priority, float blend) => new()
    {
        From = from, To = to, Condition = condition, Priority = priority, BlendSeconds = blend
    };

    private static AnimationGraphInput Input(float speed) =>
        new(true, false, Vector3.Zero, Vector3.Zero, 0, speed, speed, speed, 0, 1f / 60,
            "", 0, 1, false, true, 0);

    private static bool PathIs(in AnimationGraphDecision decision, params string[] names)
    {
        var path = decision.ConduitPath;
        if (path is null || path.Count != names.Length) return false;
        for (int index = 0; index < names.Length; ++index)
            if (!string.Equals(path[index], names[index], StringComparison.Ordinal)) return false;
        return true;
    }

    // 失败值不能是 0：宿主会拒绝零缩放，0 会让整条状态输出失败。
    private static float Flag(bool ok) => ok ? 1 : 2;
}
