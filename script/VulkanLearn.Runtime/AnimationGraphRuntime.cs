using System.Numerics;

namespace VulkanLearn;

// runtime 只读取冻结输入；曲线和 StartPosition 通过显式表提供，避免逐帧反射 catalog。
public readonly record struct AnimationGraphInput(
    bool Grounded,
    bool Sitting,
    Vector3 DesiredDirection,
    Vector3 VelocityDirection,
    float FacingYawDegrees,
    float DesiredSpeed,
    float PlanarSpeed,
    float MaxSpeed,
    float InputMagnitude,
    float DeltaTime,
    string Clip,
    float ClipTime,
    float ClipDuration,
    bool ClipCompleted,
    bool ClipPlaying,
    float SyncPhase,
    string SyncMarker = "",
    float SyncMarkerAlpha = 0,
    IReadOnlyDictionary<string, float>? Curves = null,
    IReadOnlyDictionary<string, float>? StartPositions = null,
    float VerticalSpeed = 0)
{
    internal void Validate()
    {
        if (!float.IsFinite(DesiredDirection.X) || !float.IsFinite(DesiredDirection.Y) ||
            !float.IsFinite(DesiredDirection.Z) || !float.IsFinite(VelocityDirection.X) ||
            !float.IsFinite(VelocityDirection.Y) || !float.IsFinite(VelocityDirection.Z))
            throw new ArgumentOutOfRangeException(nameof(DesiredDirection), "Graph directions must be finite.");
        ValidateNonNegative(DesiredSpeed, nameof(DesiredSpeed));
        ValidateNonNegative(PlanarSpeed, nameof(PlanarSpeed));
        ValidateNonNegative(MaxSpeed, nameof(MaxSpeed));
        ValidateNonNegative(InputMagnitude, nameof(InputMagnitude));
        if (InputMagnitude > 1)
            throw new ArgumentOutOfRangeException(nameof(InputMagnitude), "Input magnitude must be in [0,1].");
        if (!float.IsFinite(FacingYawDegrees) || !float.IsFinite(DeltaTime) || DeltaTime < 0)
            throw new ArgumentOutOfRangeException(nameof(DeltaTime));
        if (!float.IsFinite(ClipTime) || ClipTime < 0 || !float.IsFinite(ClipDuration) || ClipDuration < 0)
            throw new ArgumentOutOfRangeException(nameof(ClipTime));
        if (!float.IsFinite(SyncPhase) || SyncPhase < -1 || SyncPhase > 1)
            throw new ArgumentOutOfRangeException(nameof(SyncPhase));
        if (!float.IsFinite(SyncMarkerAlpha) || SyncMarkerAlpha < 0 || SyncMarkerAlpha > 1)
            throw new ArgumentOutOfRangeException(nameof(SyncMarkerAlpha));
        if (!float.IsFinite(VerticalSpeed))
            throw new ArgumentOutOfRangeException(nameof(VerticalSpeed));
    }

    internal bool TryCurve(string name, out float value)
    {
        value = 0;
        if (Curves is null || name.Length == 0 || !Curves.TryGetValue(name, out value)) return false;
        if (!float.IsFinite(value)) { value = 0; return false; }
        return true;
    }

    internal float StartPosition(string clip) =>
        StartPositions is not null && clip.Length > 0 && StartPositions.TryGetValue(clip, out var value) &&
        float.IsFinite(value) && value >= 0 ? value : 0;

    private static void ValidateNonNegative(float value, string name)
    {
        if (!float.IsFinite(value) || value < 0)
            throw new ArgumentOutOfRangeException(name, $"{name} must be finite and non-negative.");
    }
}

// 每帧最多每个状态机迁移一次；outer 先于 inner 求值，外层迁移时内层按 entry 规则重入。
public readonly record struct AnimationGraphDecision(
    string State,
    string LeafState,
    string StatePath,
    AnimationGraphMode Mode,
    string Transition,
    string Clip,
    bool Loop,
    string DirectionBranch,
    float BlendSeconds,
    bool Inertialization,
    string SyncGroup,
    float SyncPhase,
    float StartPosition,
    bool Crossfade,
    bool Changed,
    float StateTime,
    bool Completed,
    bool InPlace = true,
    string BlendClip = "",
    float BlendWeight = 0,
    IReadOnlyList<string>? ConduitPath = null);

public sealed class AnimationGraphRuntime
{
    private const float RadiansToDegrees = 180f / MathF.PI;
    private sealed class ActiveMachine
    {
        public AnimationGraphStateMachine Definition = null!;
        public string State = "";
        public float StateTime;
        public string EntryFromState = "";
        public bool JustEntered;
    }

    // Conduit 链的最终内容状态转移；Conduits 按经过顺序记录，用于诊断与测试。
    private sealed class ResolvedTransition
    {
        public AnimationGraphTransition Transition = null!;
        public List<string> Conduits = null!;
    }

    // 与 UE visited 集合等价的防环上限；超限按“没有可用转移”处理。
    private const int MaximumConduitChain = 16;

    private readonly AnimationGraphDefinition definition;
    private readonly Dictionary<string, AnimationGraphStateMachine> machines;
    private readonly List<ActiveMachine> stack = [];
    // UE BlendSpace 的 TargetWeightInterpolationSpeedPerSec = 5：轴值突变时权重按
    // 5/s 逼近目标，而不是当帧跳变。键是状态路径，离开状态即丢弃。
    private readonly Dictionary<string, float> blendWeights = new(StringComparer.Ordinal);
    private string activeBlendPath = "";
    private readonly List<string> conduitPath = [];
    private bool started;
    private string lastDirectionBranch = "";

    public AnimationGraphRuntime(AnimationGraphDefinition definition)
    {
        ArgumentNullException.ThrowIfNull(definition);
        this.definition = definition.Validate();
        machines = new Dictionary<string, AnimationGraphStateMachine>(StringComparer.Ordinal);
        foreach (var machine in definition.StateMachines) machines.Add(machine.Name, machine);
        var root = machines[definition.RootStateMachine];
        stack.Add(new ActiveMachine
        {
            Definition = root,
            State = root.EntryState,
            JustEntered = true
        });
        lastDirectionBranch = definition.DirectionBranches.Count > 0
            ? definition.DirectionBranches[0].Name : "";
        if (root.EntryState.Length > 0)
        {
            var entry = FindState(root, root.EntryState);
            if (entry.DirectionBranch.Length > 0) lastDirectionBranch = entry.DirectionBranch;
        }
        RebuildNested(stack[0]);
    }

    public AnimationGraphDefinition Definition => definition;

    public AnimationGraphDecision Update(in AnimationGraphInput input)
    {
        input.Validate();
        AdvanceTime(input.DeltaTime);
        conduitPath.Clear();

        var root = stack[0];
        ResolvedTransition? outerTransition = null;
        ResolvedTransition? innerTransition = null;
        bool changed = !started;
        bool wasInterruptibleLeaf = FindState(stack[^1].Definition, stack[^1].State).Interruptible;

        if (!started)
        {
            outerTransition = SelectTransition(root, root.Definition.EntryTransitions, input);
            root.JustEntered = false;
            if (outerTransition is not null)
            {
                EnterState(root, outerTransition.Transition.To, "");
                AppendConduits(outerTransition.Conduits);
                changed = true;
            }
            else
            {
                outerTransition = SelectTransition(root, root.Definition.Transitions, input);
                if (outerTransition is not null)
                {
                    string previous = root.State;
                    EnterState(root, outerTransition.Transition.To, previous);
                    AppendConduits(outerTransition.Conduits);
                    changed = true;
                }
            }
            ResolveConduitEntry(root, input);
            started = true;
        }
        else
        {
            outerTransition = SelectTransition(root, root.Definition.Transitions, input);
            if (outerTransition is not null)
            {
                string previous = root.State;
                EnterState(root, outerTransition.Transition.To, previous);
                AppendConduits(outerTransition.Conduits);
                changed = true;
            }
        }

        // 外层迁移会重建内层 entry 栈；同一帧继续检查内层 entry，保证 Idle→Walk 直接选方向起步。
        int level = 1;
        while (level < stack.Count)
        {
            var machine = stack[level];
            bool justEntered = machine.JustEntered;
            if (justEntered) ResolveConduitEntry(machine, input);
            var transitions = justEntered ? machine.Definition.EntryTransitions : machine.Definition.Transitions;
            machine.JustEntered = false;
            var transition = SelectTransition(machine, transitions, input);
            if (transition is null)
            {
                level++;
                continue;
            }
            string previous = machine.State;
            EnterState(machine, transition.Transition.To, previous);
            AppendConduits(transition.Conduits);
            innerTransition = transition;
            changed = true;
            level++;
        }

        var leafMachine = stack[^1];
        var leafState = FindState(leafMachine.Definition, leafMachine.State);
        string clip = leafState.Clip;
        string blendClip = "";
        float blendWeight = 0;
        string statePath = BuildStatePath();
        // 发生任何状态迁移或路径变化都从目标权重起步；重新进入同一路径也不能
        // 继承上一段播放的残留权重。
        if (changed || !string.Equals(activeBlendPath, statePath, StringComparison.Ordinal))
        {
            blendWeights.Clear();
            activeBlendPath = statePath;
        }
        if (leafState.BlendSamples.Count > 0)
        {
            var sample = ResolveBlendSample(leafState, input, statePath, input.DeltaTime);
            clip = sample.Clip;
            blendClip = sample.BlendClip;
            blendWeight = sample.Weight;
        }
        else blendWeights.Remove(statePath);
        float stateTime = ResolveStateTime(leafMachine, leafState, input);
        bool completed = clip.Length > 0 && input.Clip == clip && input.ClipCompleted;
        string direction = ResolveDirection(leafState, leafMachine.Definition, input);

        // 外层迁移拥有整个内层状态机的交叉淡入；内层迁移只在没有外层迁移时接管 blends。
        var transitionForOutput = (outerTransition ?? innerTransition)?.Transition;
        string tag = innerTransition is not null && innerTransition.Transition.Tag.Length > 0
            ? innerTransition.Transition.Tag
            : outerTransition?.Transition.Tag ?? "";

        float blend = transitionForOutput?.BlendSeconds ?? 0;
        bool inertialization = transitionForOutput?.Inertialization ?? false;
        string syncGroup = transitionForOutput?.SyncGroup ?? "";
        // 起步片段被外层迁移打断时，UE 不会把旧片段的相位带到目标循环；图定义里
        // outer transition 仍可声明 SyncPhaseFromSource，由这里的 interruptible 立即否决。
        bool interruptedStart = outerTransition is not null && wasInterruptibleLeaf && innerTransition is null;
        float syncPhase = transitionForOutput is not null && transitionForOutput.SyncPhaseFromSource &&
            !interruptedStart ? input.SyncPhase : -1;
        bool crossfade = transitionForOutput?.Crossfade == true && blend > 0;
        float startPosition = leafState.StartPosition >= 0
            ? leafState.StartPosition : input.StartPosition(clip);

        return new AnimationGraphDecision(
            root.State,
            leafMachine.State,
            statePath,
            FindState(root.Definition, root.State).Mode,
            tag,
            clip,
            leafState.Loop,
            direction,
            blend,
            inertialization,
            syncGroup,
            syncPhase,
            startPosition,
            crossfade,
            changed,
            stateTime,
            completed,
            true,
            blendClip,
            blendWeight,
            conduitPath.Count > 0 ? conduitPath.ToArray() : null);
    }

    // 只读诊断入口；不会推进状态机。
    public string CurrentDirectionBranch => lastDirectionBranch;

    private void AdvanceTime(float deltaTime)
    {
        if (stack.Count == 0) return;
        stack[^1].StateTime += deltaTime;
    }

    private bool IsClipReady(AnimationGraphState state, in AnimationGraphInput input) =>
        state.Clip.Length == 0 || input.Clip == state.Clip || input.Clip.Length == 0;

    private float ResolveStateTime(ActiveMachine machine, AnimationGraphState state, in AnimationGraphInput input) =>
        IsClipReady(state, input) && input.Clip == state.Clip ? input.ClipTime : machine.StateTime;

    private bool IsCompleted(ActiveMachine machine, AnimationGraphState state, in AnimationGraphInput input) =>
        state.Clip.Length > 0 && IsClipReady(state, input) && input.Clip == state.Clip && input.ClipCompleted;

    private ResolvedTransition? SelectTransition(ActiveMachine machine,
        IReadOnlyList<AnimationGraphTransition> transitions, in AnimationGraphInput input) =>
        SelectTransition(machine, machine.State, transitions, input,
            new HashSet<string>(StringComparer.Ordinal), 0);

    // 按 priority（同优先级按声明顺序）尝试转移；目标若是 conduit，就在同一帧继续
    // 用该 conduit 的出边递归，直到内容状态。visited 防止 conduit 环无限展开。
    private ResolvedTransition? SelectTransition(ActiveMachine machine, string sourceState,
        IReadOnlyList<AnimationGraphTransition> transitions, in AnimationGraphInput input,
        HashSet<string> visitedConduits, int depth)
    {
        if (depth > MaximumConduitChain) return null;
        var context = BuildContext(machine, sourceState, input);
        var candidates = new List<(int Index, AnimationGraphTransition Transition)>();
        for (int index = 0; index < transitions.Count; ++index)
        {
            var transition = transitions[index];
            if (!machine.Definition.MatchesSource(transition.From, sourceState)) continue;
            if (transition.To == sourceState) continue;
            if (!context.Evaluate(transition.Condition)) continue;
            candidates.Add((index, transition));
        }
        candidates.Sort((left, right) => left.Transition.Priority != right.Transition.Priority
            ? left.Transition.Priority.CompareTo(right.Transition.Priority)
            : left.Index.CompareTo(right.Index));
        foreach (var candidate in candidates)
        {
            var target = FindState(machine.Definition, candidate.Transition.To);
            if (!target.Conduit)
                return new ResolvedTransition { Transition = candidate.Transition, Conduits = [] };
            var nested = ResolveConduit(machine, target, input, visitedConduits, depth + 1);
            if (nested is not null) return nested;
        }
        return null;
    }

    private ResolvedTransition? ResolveConduit(ActiveMachine machine, AnimationGraphState conduit,
        in AnimationGraphInput input, HashSet<string> visitedConduits, int depth)
    {
        if (depth > MaximumConduitChain || !visitedConduits.Add(conduit.Name)) return null;
        // UE 的 conduit 入口规则：false 时这条路径整体不可用，调用方继续尝试其它转移。
        if (conduit.EntryRule is not null && !BuildContext(machine, conduit.Name, input).Evaluate(conduit.EntryRule))
            return null;
        var resolved = SelectTransition(machine, conduit.Name, machine.Definition.Transitions,
            input, visitedConduits, depth + 1);
        resolved?.Conduits.Insert(0, conduit.Name);
        return resolved;
    }

    // EntryState 本身是 conduit 时（AllowConduitEntryStates 已在定义期校验），进入状态机
    // 当帧就跟随它的出边；没有可用路径则停在该 conduit，本帧不输出片段。
    private void ResolveConduitEntry(ActiveMachine machine, in AnimationGraphInput input)
    {
        var state = FindState(machine.Definition, machine.State);
        if (!state.Conduit || !machine.Definition.AllowConduitEntryStates) return;
        var resolved = ResolveConduit(machine, state, input,
            new HashSet<string>(StringComparer.Ordinal), 0);
        if (resolved is null) return;
        string previous = machine.State;
        EnterState(machine, resolved.Transition.To, previous);
        AppendConduits(resolved.Conduits);
    }

    private AnimationGraphEvaluationContext BuildContext(ActiveMachine machine, string stateName,
        in AnimationGraphInput input)
    {
        var state = FindState(machine.Definition, stateName);
        return new AnimationGraphEvaluationContext(definition, input,
            ResolveStateTime(machine, state, input), IsCompleted(machine, state, input),
            machine.EntryFromState, machine.Definition.DirectionCurve);
    }

    private void AppendConduits(IReadOnlyList<string> conduits)
    {
        for (int index = 0; index < conduits.Count; ++index) conduitPath.Add(conduits[index]);
    }

    private void EnterState(ActiveMachine machine, string stateName, string previousState)
    {
        machine.State = stateName;
        machine.StateTime = 0;
        machine.EntryFromState = previousState;
        machine.JustEntered = false;
        RebuildNested(machine);
    }

    private void RebuildNested(ActiveMachine parent)
    {
        int index = stack.IndexOf(parent);
        if (index < 0) throw new InvalidOperationException("Animation graph machine stack is inconsistent.");
        TrimStack(index + 1);
        var state = FindState(parent.Definition, parent.State);
        if (state.SubStateMachine.Length == 0) return;
        var nested = machines[state.SubStateMachine];
        stack.Add(new ActiveMachine
        {
            Definition = nested,
            State = nested.EntryState,
            EntryFromState = parent.EntryFromState,
            JustEntered = true
        });
    }

    private void TrimStack(int count)
    {
        if (stack.Count > count) stack.RemoveRange(count, stack.Count - count);
    }

    private string BuildStatePath()
    {
        var names = new string[stack.Count];
        for (int index = 0; index < stack.Count; ++index) names[index] = stack[index].State;
        return string.Join('/', names);
    }

    private static AnimationGraphState FindState(AnimationGraphStateMachine machine, string name)
    {
        foreach (var state in machine.States)
            if (state.Name == name) return state;
        throw new InvalidOperationException($"Animation graph state '{machine.Name}/{name}' is unknown.");
    }

    // 轴值取本帧平面速度。权重先按 UE 的 BSIT_EaseInOut（smoothstep）成形，
    // 再按 5/s 向目标插值。A 固定为状态基准 sample（两个 sample 取 0，三个取 1），
    // B 只取基准左右相邻的 sample：这样跨内部 sample 边界时 A 的时钟不换片、
    // 相位不会重启；B 的权重按片段分别缓存，换片段对时从各自的历史值继续。
    private (string Clip, string BlendClip, float Weight) ResolveBlendSample(
        AnimationGraphState state, in AnimationGraphInput input, string statePath, float deltaTime)
    {
        var samples = state.BlendSamples;
        float axis = input.PlanarSpeed;
        int baseIndex = samples.Count >= 3 ? 1 : 0;
        string clip = samples[baseIndex].Clip;
        string blendClip;
        float target;
        if (baseIndex == 0)
        {
            blendClip = samples[1].Clip;
            target = SmoothstepAlpha(axis, samples[0].Threshold, samples[1].Threshold);
        }
        else if (axis <= samples[1].Threshold)
        {
            // 低段：A 是中间 sample，B 是首 sample；轴越低 B 权重越高。
            blendClip = samples[0].Clip;
            target = 1 - SmoothstepAlpha(axis, samples[0].Threshold, samples[1].Threshold);
        }
        else
        {
            blendClip = samples[2].Clip;
            target = SmoothstepAlpha(axis, samples[1].Threshold, samples[2].Threshold);
        }

        // B 片段是权重缓存键的一部分：换片段对不会继承上一对的端点权重。
        string key = statePath + "|" + blendClip;
        if (!blendWeights.TryGetValue(key, out float current) || !float.IsFinite(current))
        {
            blendWeights[key] = target;
            return (clip, blendClip, target);
        }
        float step = 5 * Math.Max(0, deltaTime);
        float weight = Math.Clamp(target, current - step, current + step);
        blendWeights[key] = weight;
        return (clip, blendClip, weight);
    }

    private static float SmoothstepAlpha(float axis, float from, float to)
    {
        float span = to - from;
        float alpha = span > 0 ? Math.Clamp((axis - from) / span, 0, 1) : 0;
        return alpha * alpha * (3 - 2 * alpha);
    }

    private string ResolveDirection(AnimationGraphState leaf, AnimationGraphStateMachine machine,
        in AnimationGraphInput input)
    {
        string branch = leaf.DirectionBranch;
        if (branch.Length > 0) return branch;
        string curve = machine.DirectionCurve;
        if (input.DesiredDirection.LengthSquared() <= 1e-6f) return lastDirectionBranch;
        float yaw = EffectiveRelativeYaw(input, curve);
        var resolved = definition.ResolveDirectionBranch(yaw);
        if (resolved is not null) lastDirectionBranch = resolved.Name;
        return lastDirectionBranch;
    }

    private static float EffectiveRelativeYaw(in AnimationGraphInput input, string curveName)
    {
        float target = MathF.Atan2(-input.DesiredDirection.X, -input.DesiredDirection.Z) * RadiansToDegrees;
        float relative = NormalizeDegrees(target - input.FacingYawDegrees);
        if (curveName.Length > 0 && input.TryCurve(curveName, out float scale) && scale >= 0 && scale <= 1)
            relative = NormalizeDegrees(relative * scale);
        return relative;
    }

    internal static float NormalizeDegrees(float degrees)
    {
        float normalized = degrees % 360f;
        if (normalized >= 180f) normalized -= 360f;
        if (normalized < -180f) normalized += 360f;
        return normalized;
    }
}

internal readonly struct AnimationGraphEvaluationContext
{
    private readonly AnimationGraphDefinition definition;
    private readonly AnimationGraphInput input;
    private readonly float stateTime;
    private readonly bool clipCompleted;
    private readonly string entryFromState;
    private readonly string machineDirectionCurve;

    public AnimationGraphEvaluationContext(AnimationGraphDefinition definition, in AnimationGraphInput input,
        float stateTime, bool clipCompleted, string entryFromState, string machineDirectionCurve)
    {
        this.definition = definition;
        this.input = input;
        this.stateTime = stateTime;
        this.clipCompleted = clipCompleted;
        this.entryFromState = entryFromState;
        this.machineDirectionCurve = machineDirectionCurve;
    }

    public bool Evaluate(AnimationGraphCondition condition)
    {
        if (condition.Operator == AnimationGraphConditionOperator.Leaf)
            return EvaluateLeaf(condition);
        if (condition.Operator == AnimationGraphConditionOperator.Not)
            return !Evaluate(condition.Children[0]);
        if (condition.Operator == AnimationGraphConditionOperator.Any)
        {
            foreach (var child in condition.Children)
                if (Evaluate(child)) return true;
            return false;
        }
        foreach (var child in condition.Children)
            if (!Evaluate(child)) return false;
        return true;
    }

    private bool EvaluateLeaf(AnimationGraphCondition condition)
    {
        switch (condition.Kind)
        {
        case AnimationGraphConditionKind.Always: return true;
        case AnimationGraphConditionKind.Grounded: return input.Grounded;
        case AnimationGraphConditionKind.Airborne: return !input.Grounded;
        case AnimationGraphConditionKind.Sitting: return input.Sitting;
        case AnimationGraphConditionKind.Speed: return Compare(input.PlanarSpeed, condition);
        case AnimationGraphConditionKind.DesiredSpeed: return Compare(input.DesiredSpeed, condition);
        case AnimationGraphConditionKind.MaxSpeed: return Compare(input.MaxSpeed, condition);
        case AnimationGraphConditionKind.VerticalSpeed: return Compare(input.VerticalSpeed, condition);
        case AnimationGraphConditionKind.InputMagnitude: return Compare(input.InputMagnitude, condition);
        case AnimationGraphConditionKind.BackwardDot:
            return Compare(BackwardDot(input), condition);
        case AnimationGraphConditionKind.Direction:
            return ResolveDirectionBranch(condition.DirectionCurve).Name == condition.Name;
        case AnimationGraphConditionKind.RelativeYaw:
        {
            float yaw = ResolveRelativeYaw(condition.DirectionCurve);
            return yaw >= condition.Minimum && yaw <= condition.Maximum;
        }
        case AnimationGraphConditionKind.Curve:
            return input.TryCurve(condition.Name, out float curve) && Compare(curve, condition);
        case AnimationGraphConditionKind.StateTime: return Compare(stateTime, condition);
        case AnimationGraphConditionKind.StateCompleted: return clipCompleted;
        case AnimationGraphConditionKind.SyncMarker:
            return input.SyncMarker == condition.Name && input.SyncMarkerAlpha >= condition.Threshold;
        case AnimationGraphConditionKind.EntryFromState: return entryFromState == condition.Name;
        default: return false;
        }
    }

    private static float BackwardDot(in AnimationGraphInput input)
    {
        if (input.DesiredDirection.LengthSquared() <= 1e-6f ||
            input.VelocityDirection.LengthSquared() <= 1e-6f)
            return 1;
        return Vector3.Dot(Vector3.Normalize(input.DesiredDirection),
            Vector3.Normalize(input.VelocityDirection));
    }

    private float ResolveRelativeYaw(string curveName)
    {
        if (input.DesiredDirection.LengthSquared() <= 1e-6f) return 0;
        float target = MathF.Atan2(-input.DesiredDirection.X, -input.DesiredDirection.Z) *
            (180f / MathF.PI);
        float relative = AnimationGraphRuntime.NormalizeDegrees(target - input.FacingYawDegrees);
        string curve = curveName.Length > 0 ? curveName : machineDirectionCurve;
        if (curve.Length > 0 && input.TryCurve(curve, out float scale) && scale >= 0 && scale <= 1)
            relative = AnimationGraphRuntime.NormalizeDegrees(relative * scale);
        return relative;
    }

    private AnimationGraphDirectionBranch ResolveDirectionBranch(string curveName)
    {
        if (input.DesiredDirection.LengthSquared() <= 1e-6f)
            return definition.ResolveDirectionBranch(0) ?? EmptyBranch;
        return definition.ResolveDirectionBranch(ResolveRelativeYaw(curveName)) ?? EmptyBranch;
    }

    private static readonly AnimationGraphDirectionBranch EmptyBranch = new() { Name = "" };

    private static bool Compare(float value, AnimationGraphCondition condition) =>
        condition.Comparison switch
        {
            AnimationGraphComparison.AtLeast => value >= condition.Threshold,
            AnimationGraphComparison.AtMost => value <= condition.Threshold,
            AnimationGraphComparison.GreaterThan => value > condition.Threshold,
            AnimationGraphComparison.LessThan => value < condition.Threshold,
            AnimationGraphComparison.Equal => value == condition.Threshold,
            AnimationGraphComparison.NotEqual => value != condition.Threshold,
            _ => false
        };
}
