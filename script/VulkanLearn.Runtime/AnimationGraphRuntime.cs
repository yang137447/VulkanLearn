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
    IReadOnlyDictionary<string, float>? StartPositions = null)
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
    bool InPlace = true);

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

    private readonly AnimationGraphDefinition definition;
    private readonly Dictionary<string, AnimationGraphStateMachine> machines;
    private readonly List<ActiveMachine> stack = [];
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

        var root = stack[0];
        AnimationGraphTransition? outerTransition = null;
        AnimationGraphTransition? innerTransition = null;
        bool changed = !started;
        bool wasInterruptibleLeaf = FindState(stack[^1].Definition, stack[^1].State).Interruptible;

        if (!started)
        {
            outerTransition = SelectTransition(root, root.Definition.EntryTransitions, input);
            root.JustEntered = false;
            if (outerTransition is not null)
            {
                EnterState(root, outerTransition.To, "");
                changed = true;
            }
            else
            {
                outerTransition = SelectTransition(root, root.Definition.Transitions, input);
                if (outerTransition is not null)
                {
                    string previous = root.State;
                    EnterState(root, outerTransition.To, previous);
                    changed = true;
                }
            }
            started = true;
        }
        else
        {
            outerTransition = SelectTransition(root, root.Definition.Transitions, input);
            if (outerTransition is not null)
            {
                string previous = root.State;
                EnterState(root, outerTransition.To, previous);
                changed = true;
            }
        }

        // 外层迁移会重建内层 entry 栈；同一帧继续检查内层 entry，保证 Idle→Walk 直接选方向起步。
        int level = 1;
        while (level < stack.Count)
        {
            var machine = stack[level];
            var transitions = machine.JustEntered ? machine.Definition.EntryTransitions : machine.Definition.Transitions;
            machine.JustEntered = false;
            var transition = SelectTransition(machine, transitions, input);
            if (transition is null)
            {
                level++;
                continue;
            }
            string previous = machine.State;
            EnterState(machine, transition.To, previous);
            innerTransition = transition;
            changed = true;
            level++;
        }

        var leafMachine = stack[^1];
        var leafState = FindState(leafMachine.Definition, leafMachine.State);
        string clip = leafState.Clip;
        float stateTime = ResolveStateTime(leafMachine, leafState, input);
        bool completed = clip.Length > 0 && input.Clip == clip && input.ClipCompleted;
        string direction = ResolveDirection(leafState, leafMachine.Definition, input);

        // 外层迁移拥有整个内层状态机的交叉淡入；内层迁移只在没有外层迁移时接管 blends。
        var transitionForOutput = outerTransition ?? innerTransition;
        string tag = innerTransition is not null && innerTransition.Tag.Length > 0
            ? innerTransition.Tag
            : outerTransition?.Tag ?? "";

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
            BuildStatePath(),
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
            true);
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

    private AnimationGraphTransition? SelectTransition(ActiveMachine machine,
        IReadOnlyList<AnimationGraphTransition> transitions, in AnimationGraphInput input)
    {
        AnimationGraphTransition? best = null;
        foreach (var transition in transitions)
        {
            if (transition.From != "*" && transition.From != machine.State) continue;
            if (transition.To == machine.State) continue;
            var state = FindState(machine.Definition, machine.State);
            var context = new AnimationGraphEvaluationContext(definition, input,
                ResolveStateTime(machine, state, input), IsCompleted(machine, state, input),
                machine.EntryFromState, machine.Definition.DirectionCurve);
            if (!context.Evaluate(transition.Condition)) continue;
            if (best is null || transition.Priority < best.Priority) best = transition;
        }
        return best;
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
