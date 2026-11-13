namespace VulkanLearn;

public enum AnimationGraphMode
{
    Grounded = 0,
    Airborne = 1
}

// BlendSpace1D 的轴：当前只实现角色平面速度轴（UE BS_MM_WalkRun 的 Speed）。
public static class AnimationGraphBlendAxis
{
    public const string Speed = "Speed";
}

// 每个 sample 是一个片段坐标；阈值必须严格递增，顺序即混合邻接顺序。
public readonly record struct AnimationGraphBlendSample(string Clip, float Threshold);

// 方向分支是图级数据：runtime 只按顺序匹配第一个包含相对 yaw 的区间。
public sealed class AnimationGraphDirectionBranch
{
    public string Name { get; init; } = "";
    public float Minimum { get; init; } = -180;
    public float Maximum { get; init; } = 180;

    internal bool Contains(float yaw) => yaw >= Minimum && yaw <= Maximum;

    internal void Validate()
    {
        if (string.IsNullOrWhiteSpace(Name))
            throw new InvalidOperationException("Animation graph direction branch name is required.");
        if (!float.IsFinite(Minimum) || !float.IsFinite(Maximum) ||
            Minimum < -180 || Maximum > 180 || Minimum > Maximum)
            throw new InvalidOperationException($"Direction branch '{Name}' must be ordered within [-180,180].");
    }
}

public sealed class AnimationGraphState
{
    // 原生两侧混合是“基准 sample + 一个邻居”的成对模型，因此只支持 2..3 个 sample。
    public const int MaximumBlendSamples = 3;

    public string Name { get; init; } = "";
    // Clip、SubStateMachine 与 BlendSamples 必须三选一。
    public string Clip { get; init; } = "";
    public bool Loop { get; init; } = true;
    public float Rate { get; init; } = 1;
    public string SyncGroup { get; init; } = "";
    // -1 = 由宿主从 Sequence Player 读取 StartPosition；>=0 = 图显式覆盖。
    public float StartPosition { get; init; } = -1;
    public AnimationGraphMode Mode { get; init; } = AnimationGraphMode.Grounded;
    public string SubStateMachine { get; init; } = "";
    // 非空表示 BlendSpace1D；runtime 按轴值在相邻 sample 间输出片段对与权重。
    public string BlendAxis { get; init; } = AnimationGraphBlendAxis.Speed;
    public IReadOnlyList<AnimationGraphBlendSample> BlendSamples { get; init; } =
        Array.Empty<AnimationGraphBlendSample>();
    // 供宿主把叶子状态映射回语义（Echo 的 Forward/Left90...）；runtime 不解释名称。
    public string DirectionBranch { get; init; } = "";
    // 一次性起步/停步片段被新外层状态打断时，runtime 允许直接进入目标叶子。
    public bool Interruptible { get; init; }

    internal void Validate()
    {
        if (string.IsNullOrWhiteSpace(Name))
            throw new InvalidOperationException("Animation graph state name is required.");
        if (!Enum.IsDefined(Mode))
            throw new InvalidOperationException($"Animation graph state '{Name}' has an invalid mode.");
        bool hasClip = !string.IsNullOrWhiteSpace(Clip);
        bool hasMachine = !string.IsNullOrWhiteSpace(SubStateMachine);
        bool hasBlend = BlendSamples is { Count: > 0 };
        int declarations = (hasClip ? 1 : 0) + (hasMachine ? 1 : 0) + (hasBlend ? 1 : 0);
        if (declarations != 1)
            throw new InvalidOperationException(
                $"Animation graph state '{Name}' must declare exactly one of " +
                "Clip, SubStateMachine or BlendSamples.");
        if (hasBlend)
        {
            ValidateBlendSamples();
            if (!float.IsFinite(Rate) || Rate <= 0 || Rate > 4)
                throw new InvalidOperationException($"Animation graph state '{Name}' rate must be in (0,4].");
            if (!float.IsFinite(StartPosition) || StartPosition < -1)
                throw new InvalidOperationException($"Animation graph state '{Name}' start position is invalid.");
        }
        else if (hasClip)
        {
            if (Clip.Trim().Length != Clip.Length)
                throw new InvalidOperationException($"Animation graph state '{Name}' clip has surrounding whitespace.");
            if (!float.IsFinite(Rate) || Rate <= 0 || Rate > 4)
                throw new InvalidOperationException($"Animation graph state '{Name}' rate must be in (0,4].");
            if (!float.IsFinite(StartPosition) || StartPosition < -1)
                throw new InvalidOperationException($"Animation graph state '{Name}' start position is invalid.");
            if (SyncGroup is null || SyncGroup.Length > 128 || SyncGroup.Contains('\0'))
                throw new InvalidOperationException($"Animation graph state '{Name}' sync group is invalid.");
        }
        else if (!float.IsFinite(Rate) || Rate != 1)
        {
            throw new InvalidOperationException(
                $"Animation graph state machine state '{Name}' cannot override clip rate.");
        }
        if (DirectionBranch is null || DirectionBranch.Length > 64 || DirectionBranch.Contains('\0'))
            throw new InvalidOperationException($"Animation graph state '{Name}' direction branch is invalid.");
    }

    private void ValidateBlendSamples()
    {
        if (BlendAxis != AnimationGraphBlendAxis.Speed)
            throw new InvalidOperationException(
                $"Animation graph state '{Name}' uses unsupported blend axis '{BlendAxis}'.");
        if (BlendSamples is null || BlendSamples.Count < 2 ||
            BlendSamples.Count > MaximumBlendSamples)
            throw new InvalidOperationException(
                $"Animation graph state '{Name}' blend space requires 2..{MaximumBlendSamples} samples.");
        float previous = float.NegativeInfinity;
        var clips = new HashSet<string>(StringComparer.Ordinal);
        foreach (var sample in BlendSamples)
        {
            if (string.IsNullOrWhiteSpace(sample.Clip) || sample.Clip.Trim().Length != sample.Clip.Length)
                throw new InvalidOperationException(
                    $"Animation graph state '{Name}' has an invalid blend sample clip.");
            if (!float.IsFinite(sample.Threshold) || sample.Threshold <= previous)
                throw new InvalidOperationException(
                    $"Animation graph state '{Name}' blend sample thresholds must be finite and increasing.");
            previous = sample.Threshold;
            if (!clips.Add(sample.Clip))
                throw new InvalidOperationException(
                    $"Animation graph state '{Name}' has duplicate blend sample clip '{sample.Clip}'.");
        }
    }
}

public sealed class AnimationGraphTransition
{
    // 诊断与宿主映射用；runtime 不理解 tag 的业务含义。
    public string Tag { get; init; } = "";
    // From 为 "*" 表示任意状态；状态机迁移不影响该规则。
    public string From { get; init; } = "";
    public string To { get; init; } = "";
    public AnimationGraphCondition Condition { get; init; } = AnimationGraphCondition.Always;
    // 数值越小越先检查；同优先级按声明顺序稳定决胜。
    public int Priority { get; init; }
    public float BlendSeconds { get; init; }
    public bool Inertialization { get; init; }
    public string SyncGroup { get; init; } = "";
    public bool SyncPhaseFromSource { get; init; }
    // false 表示硬切；true 且 BlendSeconds>0 表示交叉淡入。
    public bool Crossfade { get; init; } = true;

    internal void Validate(string machineName)
    {
        if (Condition is null)
            throw new InvalidOperationException($"Animation graph transition in '{machineName}' has no condition.");
        if (string.IsNullOrWhiteSpace(To))
            throw new InvalidOperationException($"Animation graph transition in '{machineName}' has no target state.");
        if (string.IsNullOrWhiteSpace(From))
            throw new InvalidOperationException($"Animation graph transition to '{To}' has no source state.");
        if (!float.IsFinite(BlendSeconds) || BlendSeconds < 0 || BlendSeconds > 10)
            throw new InvalidOperationException($"Animation graph transition '{From}->{To}' blend is invalid.");
        if (SyncGroup is null || SyncGroup.Length > 128 || SyncGroup.Contains('\0'))
            throw new InvalidOperationException($"Animation graph transition '{From}->{To}' sync group is invalid.");
        if (SyncPhaseFromSource && SyncGroup.Length == 0)
            throw new InvalidOperationException(
                $"Animation graph transition '{From}->{To}' requires a sync group for source phase.");
        if (Tag is null || Tag.Length > 64 || Tag.Contains('\0'))
            throw new InvalidOperationException($"Animation graph transition '{From}->{To}' tag is invalid.");
        Condition.Validate();
    }
}

// UE State Alias：把多条来源状态的转移收敛到一个共享出口。VL 只把它作为
// 转移来源集合解释，不生成可进入的状态，也不改变状态机当前状态身份。
public sealed class AnimationGraphStateAlias
{
    public string Name { get; init; } = "";
    // Global 等价于 UE 的 Global Alias：当前和以后新增的状态都属于来源集合。
    public bool Global { get; init; }
    public IReadOnlyList<string> Sources { get; init; } = Array.Empty<string>();

    internal bool Contains(string state) =>
        Global || Sources.Contains(state, StringComparer.Ordinal);

    internal void Validate()
    {
        if (string.IsNullOrWhiteSpace(Name) || Name.Trim().Length != Name.Length)
            throw new InvalidOperationException("Animation graph state alias name is required.");
        if (Sources is null)
            throw new InvalidOperationException($"Animation graph state alias '{Name}' sources cannot be null.");
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var source in Sources)
        {
            if (string.IsNullOrWhiteSpace(source) || source.Trim().Length != source.Length)
                throw new InvalidOperationException(
                    $"Animation graph state alias '{Name}' contains an invalid source state.");
            if (!seen.Add(source))
                throw new InvalidOperationException(
                    $"Animation graph state alias '{Name}' contains duplicate source '{source}'.");
        }
    }
}

public sealed class AnimationGraphStateMachine
{
    public const int MaximumAliasesPerMachine = 32;

    public string Name { get; init; } = "";
    public string EntryState { get; init; } = "";
    // 内层步态用它选择方向曲线；空串表示直接使用相对 yaw。
    public string DirectionCurve { get; init; } = "";
    public IReadOnlyList<AnimationGraphState> States { get; init; } = Array.Empty<AnimationGraphState>();
    public IReadOnlyList<AnimationGraphStateAlias> Aliases { get; init; } =
        Array.Empty<AnimationGraphStateAlias>();
    // 进入状态机当帧只检查 EntryTransitions，不检查常规 Transitions。
    public IReadOnlyList<AnimationGraphTransition> EntryTransitions { get; init; } =
        Array.Empty<AnimationGraphTransition>();
    public IReadOnlyList<AnimationGraphTransition> Transitions { get; init; } =
        Array.Empty<AnimationGraphTransition>();

    internal void Validate(IReadOnlyDictionary<string, AnimationGraphStateMachine> machines)
    {
        if (string.IsNullOrWhiteSpace(Name))
            throw new InvalidOperationException("Animation graph state machine name is required.");
        if (States is null || Aliases is null || EntryTransitions is null || Transitions is null)
            throw new InvalidOperationException($"Animation graph state machine '{Name}' collections cannot be null.");
        if (States.Count == 0)
            throw new InvalidOperationException($"Animation graph state machine '{Name}' has no states.");
        if (States.Count > AnimationGraphDefinition.MaximumStatesPerMachine)
            throw new InvalidOperationException($"Animation graph state machine '{Name}' exceeds the state budget.");
        if (Transitions.Count > AnimationGraphDefinition.MaximumTransitionsPerMachine ||
            EntryTransitions.Count > AnimationGraphDefinition.MaximumTransitionsPerMachine)
            throw new InvalidOperationException($"Animation graph state machine '{Name}' exceeds the transition budget.");
        if (Aliases.Count > MaximumAliasesPerMachine)
            throw new InvalidOperationException(
                $"Animation graph state machine '{Name}' exceeds the state alias budget.");
        if (DirectionCurve is null || DirectionCurve.Trim().Length != DirectionCurve.Length ||
            DirectionCurve.Contains('\0'))
            throw new InvalidOperationException($"Animation graph state machine '{Name}' direction curve is invalid.");

        var byName = new Dictionary<string, AnimationGraphState>(StringComparer.Ordinal);
        foreach (var state in States)
        {
            if (state is null)
                throw new InvalidOperationException($"Animation graph state machine '{Name}' has a null state.");
            state.Validate();
            if (!byName.TryAdd(state.Name, state))
                throw new InvalidOperationException(
                    $"Animation graph state machine '{Name}' has duplicate state '{state.Name}'.");
            if (state.SubStateMachine.Length > 0)
            {
                if (state.SubStateMachine == Name)
                    throw new InvalidOperationException(
                        $"Animation graph state machine '{Name}' cannot nest itself.");
                if (!machines.ContainsKey(state.SubStateMachine))
                    throw new InvalidOperationException(
                        $"Animation graph state '{Name}/{state.Name}' references unknown machine " +
                        $"'{state.SubStateMachine}'.");
            }
        }
        var aliases = new Dictionary<string, AnimationGraphStateAlias>(StringComparer.Ordinal);
        foreach (var alias in Aliases)
        {
            if (alias is null)
                throw new InvalidOperationException(
                    $"Animation graph state machine '{Name}' has a null state alias.");
            alias.Validate();
            if (byName.ContainsKey(alias.Name))
                throw new InvalidOperationException(
                    $"Animation graph state machine '{Name}' state alias '{alias.Name}' collides with a state.");
            if (!aliases.TryAdd(alias.Name, alias))
                throw new InvalidOperationException(
                    $"Animation graph state machine '{Name}' has duplicate state alias '{alias.Name}'.");
            foreach (var source in alias.Sources)
                if (!byName.ContainsKey(source))
                    throw new InvalidOperationException(
                        $"Animation graph state alias '{Name}/{alias.Name}' has unknown source '{source}'.");
        }
        if (string.IsNullOrWhiteSpace(EntryState) || !byName.ContainsKey(EntryState))
            throw new InvalidOperationException(
                $"Animation graph state machine '{Name}' entry state '{EntryState}' is unknown.");
        ValidateTransitions(byName, aliases, EntryTransitions, "entry");
        ValidateTransitions(byName, aliases, Transitions, "transition");
        ValidateReachability(byName, aliases);
    }

    private void ValidateTransitions(IReadOnlyDictionary<string, AnimationGraphState> states,
        IReadOnlyDictionary<string, AnimationGraphStateAlias> aliases,
        IReadOnlyList<AnimationGraphTransition> transitions, string kind)
    {
        foreach (var transition in transitions)
        {
            if (transition is null)
                throw new InvalidOperationException($"Animation graph machine '{Name}' has a null {kind}.");
            transition.Validate(Name);
            if (transition.From != "*" && !states.ContainsKey(transition.From) &&
                !aliases.ContainsKey(transition.From))
                throw new InvalidOperationException(
                    $"Animation graph machine '{Name}' {kind} has unknown source '{transition.From}'.");
            if (!states.ContainsKey(transition.To))
                throw new InvalidOperationException(
                    $"Animation graph machine '{Name}' {kind} has unknown target '{transition.To}'.");
        }
    }

    // 状态迁移本身允许环（Idle↔Walk 本来就必须成环）；这里只拒绝从 Entry 永远到不了的状态。
    private void ValidateReachability(IReadOnlyDictionary<string, AnimationGraphState> states,
        IReadOnlyDictionary<string, AnimationGraphStateAlias> aliases)
    {
        var reachable = new HashSet<string>(StringComparer.Ordinal) { EntryState };
        bool changed = true;
        while (changed)
        {
            changed = false;
            foreach (var transition in EntryTransitions.Concat(Transitions))
            {
                bool sourceReachable;
                if (transition.From == "*")
                    sourceReachable = reachable.Count > 0;
                else if (aliases.TryGetValue(transition.From, out var alias))
                    sourceReachable = alias.Global
                        ? reachable.Count > 0
                        : alias.Sources.Any(reachable.Contains);
                else
                    sourceReachable = reachable.Contains(transition.From);
                if (sourceReachable && reachable.Add(transition.To))
                    changed = true;
            }
        }
        foreach (var state in states.Values)
            if (!reachable.Contains(state.Name))
                throw new InvalidOperationException(
                    $"Animation graph state '{Name}/{state.Name}' is unreachable from entry '{EntryState}'.");
    }

    internal bool MatchesSource(string from, string currentState)
    {
        if (from == "*" || from == currentState) return true;
        foreach (var alias in Aliases)
            if (alias.Name == from && alias.Contains(currentState)) return true;
        return false;
    }
}

public sealed class AnimationGraphDefinition
{
    public const int MaximumStateMachines = 32;
    public const int MaximumStatesPerMachine = 128;
    public const int MaximumStates = 512;
    public const int MaximumTransitionsPerMachine = 256;
    public const int MaximumTransitions = 2048;
    public const int MaximumMachineDepth = 4;
    public const int MaximumDirectionBranches = 16;

    public string Name { get; init; } = "";
    public string Version { get; init; } = "vl.anim-graph.v1";
    public string RootStateMachine { get; init; } = "";
    public IReadOnlyList<AnimationGraphDirectionBranch> DirectionBranches { get; init; } =
        Array.Empty<AnimationGraphDirectionBranch>();
    public IReadOnlyList<AnimationGraphStateMachine> StateMachines { get; init; } =
        Array.Empty<AnimationGraphStateMachine>();

    public AnimationGraphDefinition Validate()
    {
        if (string.IsNullOrWhiteSpace(Name))
            throw new InvalidOperationException("Animation graph name is required.");
        if (Version != "vl.anim-graph.v1")
            throw new InvalidOperationException($"Unsupported animation graph version: {Version}.");
        if (StateMachines is null || DirectionBranches is null)
            throw new InvalidOperationException("Animation graph collections cannot be null.");
        if (StateMachines.Count == 0 || StateMachines.Count > MaximumStateMachines)
            throw new InvalidOperationException("Animation graph state machine budget exceeded.");
        if (DirectionBranches.Count > MaximumDirectionBranches)
            throw new InvalidOperationException("Animation graph direction branch budget exceeded.");

        var machines = new Dictionary<string, AnimationGraphStateMachine>(StringComparer.Ordinal);
        foreach (var machine in StateMachines)
        {
            if (machine is null)
                throw new InvalidOperationException("Animation graph contains a null state machine.");
            if (!machines.TryAdd(machine.Name, machine))
                throw new InvalidOperationException($"Duplicate animation graph state machine '{machine.Name}'.");
        }
        if (string.IsNullOrWhiteSpace(RootStateMachine) || !machines.ContainsKey(RootStateMachine))
            throw new InvalidOperationException("Animation graph root state machine is unknown.");

        var branchNames = new HashSet<string>(StringComparer.Ordinal);
        foreach (var branch in DirectionBranches)
        {
            if (branch is null)
                throw new InvalidOperationException("Animation graph contains a null direction branch.");
            branch.Validate();
            if (!branchNames.Add(branch.Name))
                throw new InvalidOperationException($"Duplicate direction branch '{branch.Name}'.");
        }

        int stateCount = 0;
        int transitionCount = 0;
        foreach (var machine in StateMachines)
        {
            machine.Validate(machines);
            stateCount += machine.States.Count;
            transitionCount += machine.Transitions.Count + machine.EntryTransitions.Count;
        }
        if (stateCount > MaximumStates)
            throw new InvalidOperationException("Animation graph state budget exceeded.");
        if (transitionCount > MaximumTransitions)
            throw new InvalidOperationException("Animation graph transition budget exceeded.");
        foreach (var machine in StateMachines)
            foreach (var state in machine.States)
                if (state.DirectionBranch.Length > 0 && !branchNames.Contains(state.DirectionBranch))
                    throw new InvalidOperationException(
                        $"Animation graph state '{machine.Name}/{state.Name}' uses unknown direction branch " +
                        $"'{state.DirectionBranch}'.");
        ValidateNestedMachines(machines);
        ValidateDirectionConditions(machines, branchNames);
        return this;
    }

    internal AnimationGraphDirectionBranch? ResolveDirectionBranch(float yaw)
    {
        foreach (var branch in DirectionBranches)
            if (branch.Contains(yaw)) return branch;
        return null;
    }

    // 宿主据此只采样图真正需要的曲线；缺失值由 AnimationGraphInput 视为 false 条件。
    public IReadOnlyList<string> CollectRequiredCurves()
    {
        var curves = new List<string>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var machine in StateMachines)
        {
            AddCurve(machine.DirectionCurve);
            foreach (var transition in machine.EntryTransitions.Concat(machine.Transitions))
                CollectCondition(transition.Condition);
        }
        return curves;

        void CollectCondition(AnimationGraphCondition condition)
        {
            if (condition.Operator == AnimationGraphConditionOperator.Leaf)
            {
                if (condition.Kind == AnimationGraphConditionKind.Curve) AddCurve(condition.Name);
                else if (condition.Kind is AnimationGraphConditionKind.Direction
                         or AnimationGraphConditionKind.RelativeYaw) AddCurve(condition.DirectionCurve);
                return;
            }
            foreach (var child in condition.Children) CollectCondition(child);
        }

        void AddCurve(string name)
        {
            if (!string.IsNullOrEmpty(name) && seen.Add(name)) curves.Add(name);
        }
    }

    // 嵌套引用必须是 DAG；状态迁移环合法，但状态机互相嵌套会形成无限求值路径。
    private void ValidateNestedMachines(IReadOnlyDictionary<string, AnimationGraphStateMachine> machines)
    {
        var visiting = new HashSet<string>(StringComparer.Ordinal);
        var visited = new HashSet<string>(StringComparer.Ordinal);
        foreach (var machine in machines.Keys)
            Visit(machine, 0);

        void Visit(string name, int depth)
        {
            if (visited.Contains(name)) return;
            if (depth > MaximumMachineDepth)
                throw new InvalidOperationException(
                    $"Animation graph nested state machine depth exceeds {MaximumMachineDepth}.");
            if (!visiting.Add(name))
                throw new InvalidOperationException($"Animation graph nested state machine cycle at '{name}'.");
            foreach (var state in machines[name].States)
                if (state.SubStateMachine.Length > 0) Visit(state.SubStateMachine, depth + 1);
            visiting.Remove(name);
            visited.Add(name);
        }
    }

    private static void ValidateDirectionConditions(
        IReadOnlyDictionary<string, AnimationGraphStateMachine> machines, HashSet<string> branchNames)
    {
        foreach (var machine in machines.Values)
        {
            foreach (var transition in machine.EntryTransitions.Concat(machine.Transitions))
                ValidateCondition(transition.Condition);
        }

        void ValidateCondition(AnimationGraphCondition condition)
        {
            if (condition.Operator == AnimationGraphConditionOperator.Leaf)
            {
                if (condition.Kind == AnimationGraphConditionKind.Direction &&
                    !branchNames.Contains(condition.Name))
                    throw new InvalidOperationException(
                        $"Animation graph condition references unknown direction branch '{condition.Name}'.");
                return;
            }
            foreach (var child in condition.Children) ValidateCondition(child);
        }
    }
}
