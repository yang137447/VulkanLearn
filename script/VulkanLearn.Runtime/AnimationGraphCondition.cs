namespace VulkanLearn;

// 图条件的叶节点只读取本帧冻结输入；组合节点只做布尔归约，不持有状态。
public enum AnimationGraphConditionKind
{
    None = 0,
    Always = 1,
    Grounded = 2,
    Airborne = 3,
    Sitting = 4,
    Speed = 5,
    DesiredSpeed = 6,
    MaxSpeed = 7,
    InputMagnitude = 8,
    BackwardDot = 9,
    Direction = 10,
    RelativeYaw = 11,
    Curve = 12,
    StateTime = 13,
    StateCompleted = 14,
    SyncMarker = 15,
    EntryFromState = 16,
    VerticalSpeed = 17
}

public enum AnimationGraphConditionOperator
{
    Leaf = 0,
    All = 1,
    Any = 2,
    Not = 3
}

// 比较方向统一为“输入值 op 阈值”，阈值本身来自图定义而不是散落在 runtime。
public enum AnimationGraphComparison
{
    AtLeast = 0,
    AtMost = 1,
    GreaterThan = 2,
    LessThan = 3,
    Equal = 4,
    NotEqual = 5
}

// 条件树是不可变的作者数据；求值阶段只读，方便同一图被多个角色实例共享。
public sealed class AnimationGraphCondition
{
    public const int MaximumDepth = 8;
    public const int MaximumNodes = 128;

    public AnimationGraphConditionOperator Operator { get; init; } = AnimationGraphConditionOperator.Leaf;
    public AnimationGraphConditionKind Kind { get; init; } = AnimationGraphConditionKind.Always;
    public AnimationGraphComparison Comparison { get; init; } = AnimationGraphComparison.AtLeast;
    // 数值阈值；Direction/Curve/SyncMarker/EntryFromState 用 Name 选目标。
    public float Threshold { get; init; }
    public float Minimum { get; init; } = -180;
    public float Maximum { get; init; } = 180;
    public string Name { get; init; } = "";
    // Direction/RelativeYaw 可选的方向曲线名；缺失时使用未缩放的相对 yaw。
    public string DirectionCurve { get; init; } = "";
    public IReadOnlyList<AnimationGraphCondition> Children { get; init; } = Array.Empty<AnimationGraphCondition>();

    public static AnimationGraphCondition Always => new();
    public static AnimationGraphCondition Grounded => Leaf(AnimationGraphConditionKind.Grounded);
    public static AnimationGraphCondition Airborne => Leaf(AnimationGraphConditionKind.Airborne);
    public static AnimationGraphCondition Sitting => Leaf(AnimationGraphConditionKind.Sitting);
    public static AnimationGraphCondition StateCompleted => Leaf(AnimationGraphConditionKind.StateCompleted);

    public static AnimationGraphCondition Speed(float threshold, AnimationGraphComparison comparison = AnimationGraphComparison.AtLeast) =>
        Numeric(AnimationGraphConditionKind.Speed, threshold, comparison);
    public static AnimationGraphCondition DesiredSpeed(float threshold, AnimationGraphComparison comparison = AnimationGraphComparison.AtLeast) =>
        Numeric(AnimationGraphConditionKind.DesiredSpeed, threshold, comparison);
    public static AnimationGraphCondition MaxSpeed(float threshold, AnimationGraphComparison comparison = AnimationGraphComparison.AtLeast) =>
        Numeric(AnimationGraphConditionKind.MaxSpeed, threshold, comparison);
    public static AnimationGraphCondition VerticalSpeed(float threshold,
        AnimationGraphComparison comparison = AnimationGraphComparison.AtLeast) =>
        Numeric(AnimationGraphConditionKind.VerticalSpeed, threshold, comparison);
    public static AnimationGraphCondition InputMagnitude(float threshold, AnimationGraphComparison comparison = AnimationGraphComparison.AtLeast) =>
        Numeric(AnimationGraphConditionKind.InputMagnitude, threshold, comparison);
    public static AnimationGraphCondition BackwardDot(float threshold, AnimationGraphComparison comparison = AnimationGraphComparison.LessThan) =>
        Numeric(AnimationGraphConditionKind.BackwardDot, threshold, comparison);
    public static AnimationGraphCondition Curve(string name, float threshold,
        AnimationGraphComparison comparison = AnimationGraphComparison.AtLeast) =>
        new() { Kind = AnimationGraphConditionKind.Curve, Name = name, Threshold = threshold, Comparison = comparison };
    public static AnimationGraphCondition StateTime(float threshold,
        AnimationGraphComparison comparison = AnimationGraphComparison.AtLeast) =>
        Numeric(AnimationGraphConditionKind.StateTime, threshold, comparison);
    public static AnimationGraphCondition Direction(string branch, string curve = "") =>
        new() { Kind = AnimationGraphConditionKind.Direction, Name = branch, DirectionCurve = curve };
    public static AnimationGraphCondition RelativeYaw(float minimum, float maximum, string curve = "") =>
        new()
        {
            Kind = AnimationGraphConditionKind.RelativeYaw,
            Minimum = minimum,
            Maximum = maximum,
            DirectionCurve = curve
        };
    public static AnimationGraphCondition SyncMarker(string marker, float minimumAlpha = 0) =>
        new() { Kind = AnimationGraphConditionKind.SyncMarker, Name = marker, Threshold = minimumAlpha };
    public static AnimationGraphCondition EntryFromState(string state) =>
        new() { Kind = AnimationGraphConditionKind.EntryFromState, Name = state };

    public static AnimationGraphCondition All(params AnimationGraphCondition[] children) =>
        Composite(AnimationGraphConditionOperator.All, children);
    public static AnimationGraphCondition Any(params AnimationGraphCondition[] children) =>
        Composite(AnimationGraphConditionOperator.Any, children);
    public static AnimationGraphCondition Not(AnimationGraphCondition child) =>
        new() { Operator = AnimationGraphConditionOperator.Not, Children = new[] { child } };

    // 组合条件不能是空数组；空 All/Any 会让作者误以为自己写了一条兜底规则。
    public static AnimationGraphCondition Composite(AnimationGraphConditionOperator kind,
        params AnimationGraphCondition[] children)
    {
        if (kind is AnimationGraphConditionOperator.Leaf or AnimationGraphConditionOperator.Not)
            throw new ArgumentException("Composite conditions require All or Any.", nameof(kind));
        ArgumentNullException.ThrowIfNull(children);
        if (children.Length == 0)
            throw new ArgumentException("Composite condition requires at least one child.", nameof(children));
        return new() { Operator = kind, Children = (AnimationGraphCondition[])children.Clone() };
    }

    private static AnimationGraphCondition Leaf(AnimationGraphConditionKind kind) => new() { Kind = kind };

    private static AnimationGraphCondition Numeric(AnimationGraphConditionKind kind, float threshold,
        AnimationGraphComparison comparison) => new() { Kind = kind, Threshold = threshold, Comparison = comparison };

    public void Validate()
    {
        if (!Enum.IsDefined(Operator))
            throw new ArgumentOutOfRangeException(nameof(Operator));
        if (!Enum.IsDefined(Kind))
            throw new ArgumentOutOfRangeException(nameof(Kind));
        if (!Enum.IsDefined(Comparison))
            throw new ArgumentOutOfRangeException(nameof(Comparison));
        int nodes = 0;
        ValidateNode(0, ref nodes);
    }

    private void ValidateNode(int depth, ref int nodes)
    {
        if (depth > MaximumDepth)
            throw new InvalidOperationException("Animation graph condition exceeds the depth budget.");
        if (++nodes > MaximumNodes)
            throw new InvalidOperationException("Animation graph condition exceeds the node budget.");
        if (Operator == AnimationGraphConditionOperator.Leaf)
        {
            if (Children.Count != 0)
                throw new InvalidOperationException("Leaf animation graph condition cannot have children.");
            if (Kind == AnimationGraphConditionKind.None)
                throw new InvalidOperationException("Animation graph condition kind is required.");
            ValidateLeaf();
            return;
        }
        if (Operator == AnimationGraphConditionOperator.Not)
        {
            if (Children.Count != 1)
                throw new InvalidOperationException("Not condition requires exactly one child.");
        }
        else if (Children.Count == 0)
            throw new InvalidOperationException("Composite animation graph condition requires children.");
        foreach (var child in Children)
        {
            if (child is null)
                throw new InvalidOperationException("Animation graph condition child is null.");
            child.ValidateNode(depth + 1, ref nodes);
        }
    }

    private void ValidateLeaf()
    {
        switch (Kind)
        {
        case AnimationGraphConditionKind.Always:
        case AnimationGraphConditionKind.Grounded:
        case AnimationGraphConditionKind.Airborne:
        case AnimationGraphConditionKind.Sitting:
        case AnimationGraphConditionKind.StateCompleted:
            break;
        case AnimationGraphConditionKind.Speed:
        case AnimationGraphConditionKind.DesiredSpeed:
        case AnimationGraphConditionKind.MaxSpeed:
        case AnimationGraphConditionKind.VerticalSpeed:
        case AnimationGraphConditionKind.InputMagnitude:
        case AnimationGraphConditionKind.BackwardDot:
            RequireFinite(Threshold, "Threshold");
            break;
        case AnimationGraphConditionKind.Curve:
            RequireName(Name, "Curve name");
            RequireFinite(Threshold, "Threshold");
            break;
        case AnimationGraphConditionKind.StateTime:
            RequireFinite(Threshold, "Threshold");
            if (Threshold < 0)
                throw new InvalidOperationException("State time threshold cannot be negative.");
            break;
        case AnimationGraphConditionKind.Direction:
            RequireName(Name, "Direction branch");
            break;
        case AnimationGraphConditionKind.RelativeYaw:
            RequireFinite(Minimum, "Minimum");
            RequireFinite(Maximum, "Maximum");
            if (Minimum > Maximum || Minimum < -180 || Maximum > 180)
                throw new InvalidOperationException("Relative yaw range must be ordered within [-180,180].");
            break;
        case AnimationGraphConditionKind.SyncMarker:
            RequireName(Name, "Sync marker");
            RequireFinite(Threshold, "Threshold");
            break;
        case AnimationGraphConditionKind.EntryFromState:
            RequireName(Name, "Entry source state");
            break;
        default:
            throw new InvalidOperationException($"Unsupported animation graph condition kind: {Kind}.");
        }
        if ((Kind is AnimationGraphConditionKind.Direction or AnimationGraphConditionKind.RelativeYaw) &&
            (DirectionCurve is null || DirectionCurve.Trim().Length != DirectionCurve.Length))
            throw new InvalidOperationException("Direction curve name cannot have surrounding whitespace.");
    }

    private static void RequireFinite(float value, string name)
    {
        if (!float.IsFinite(value))
            throw new InvalidOperationException($"Animation graph condition {name} must be finite.");
    }

    private static void RequireName(string value, string name)
    {
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException($"Animation graph condition {name} is required.");
    }
}
