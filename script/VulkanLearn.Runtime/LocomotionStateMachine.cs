using System.Numerics;

namespace VulkanLearn;

// Valley Echo 只有三种地面步态；空中/落地是独立的一次性转移，不塞进这个枚举。
public enum LocomotionState
{
    Idle = 0,
    Walk = 1,
    Jog = 2
}

public enum LocomotionMode
{
    Grounded = 0,
    Airborne = 1
}

// 输入方向相对角色朝向的五个分桶；片段映射由 LocomotionClipSet 负责。
public enum LocomotionDirection
{
    Forward = 0,
    Left90 = 1,
    Left180 = 2,
    Right90 = 3,
    Right180 = 4
}

// 每个 Update 最多输出一个转移；None 表示继续当前片段，不重新提交播放请求。
public enum LocomotionTransition
{
    None = 0,
    StartWalk = 1,
    StartJog = 2,
    StopWalk = 3,
    StopJog = 4,
    Fall = 5,
    Land = 6
}

// 世界方向与速度由调用方提供：状态机不读输入、不换算相机空间、不持有播放器。
public readonly record struct LocomotionInput(
    Vector3 DesiredDirection,
    float DesiredSpeed,
    float PlanarSpeed,
    float FacingYawDegrees,
    bool Grounded,
    // UE Echo 的 DetermineLocomotionState 读原始 InputVector、角色当前 MaxSpeed 与
    // Velocity 方向；负值表示由旧调用方从 DesiredDirection/DesiredSpeed 推导。
    float InputMagnitude = -1,
    float MaxSpeed = -1,
    Vector3 VelocityDirection = default,
    bool Sitting = false,
    // 可选 MoveData 曲线。NaN/负值 = 未提供，必须回退旧输入路径而不是当成 0。
    // SpeedCurve 是片段作者的名义移动速度，参与 Jog/Walk 阈值（只抬高、不压低
    // 现有 DesiredSpeed/MaxSpeed）；实际 PlanarSpeed 与倒走门控仍优先。
    float SpeedCurve = float.NaN,
    // Echo 的 RotationDelta 曲线是 [0,1] 的“剩余转向比例”。VL 用它在方向分桶前
    // 缩放带符号相对 yaw；缺失时仍直接使用当前相对 yaw。
    float WalkRotationDeltaCurve = float.NaN,
    float JogRotationDeltaCurve = float.NaN);

// DesiredSpeed 是调用方的原始意图，TargetSpeed 是当前步态应使用的名义速度。
// TargetYawDegrees 使用 VL 角色约定：yaw=0 面向 -Z，范围归一化到 [-180,180)。
public readonly record struct LocomotionDecision(
    LocomotionState State,
    LocomotionMode Mode,
    LocomotionTransition Transition,
    LocomotionDirection Direction,
    float DesiredSpeed,
    float TargetSpeed,
    float TargetYawDegrees);

// 配置在构造时一次性校验；阈值与滞回带不散落在条件表达式里。
public sealed class LocomotionSettings
{
    public float WalkSpeed { get; init; } = 200;
    public float JogSpeed { get; init; } = 450;
    public float StopSpeed { get; init; } = 10;
    public float WalkToJogEnterSpeed { get; init; } = 300;
    public float JogToWalkExitSpeed { get; init; } = 260;
    // UE Echo 的 DetermineLocomotionState：实际速度 >=1、InputVector 幅度与 MaxSpeed
    // 共同决定 Jog/Walk；速度方向与输入方向点积 < -0.5 时保持 Idle。
    public bool UseUeStateConditions { get; init; } = true;
    public float MinimumMoveSpeed { get; init; } = 1;
    public float JogMaxSpeed { get; init; } = 300;
    public float JogInputMagnitude { get; init; } = 0.5f;
    public float WalkInputMagnitude { get; init; } = 0.01f;
    public float BackwardDotThreshold { get; init; } = -0.5f;
    // Idle 内层：Speed > 200 播 Jog Stop，Speed > 1 播 Walk Stop。
    public float JogStopSpeed { get; init; } = 200;
    // 输入方向长度平方低于该值视为没有方向；避免零向量 atan2 抖动。
    public float MinimumDirectionLengthSquared { get; init; } = 1e-6f;
    // |相对 yaw| <= ForwardBoundary 走正向起步；>= BackwardBoundary 走 180 片段。
    public float ForwardBoundaryDegrees { get; init; } = 45;
    public float BackwardBoundaryDegrees { get; init; } = 135;

    internal void Validate()
    {
        RequireFinitePositive(WalkSpeed, nameof(WalkSpeed));
        RequireFinitePositive(JogSpeed, nameof(JogSpeed));
        if (JogSpeed <= WalkSpeed)
            throw new ArgumentOutOfRangeException(nameof(JogSpeed), "Jog speed must be greater than walk speed.");
        if (!float.IsFinite(StopSpeed) || StopSpeed < 0 || StopSpeed >= WalkSpeed)
            throw new ArgumentOutOfRangeException(nameof(StopSpeed));
        if (!float.IsFinite(WalkToJogEnterSpeed) || WalkToJogEnterSpeed <= 0)
            throw new ArgumentOutOfRangeException(nameof(WalkToJogEnterSpeed));
        if (WalkToJogEnterSpeed <= WalkSpeed || WalkToJogEnterSpeed > JogSpeed)
            throw new ArgumentOutOfRangeException(nameof(WalkToJogEnterSpeed),
                "Walk-to-jog enter speed must sit above walk speed and at or below jog speed.");
        if (!float.IsFinite(JogToWalkExitSpeed) || JogToWalkExitSpeed <= 0 ||
            JogToWalkExitSpeed >= WalkToJogEnterSpeed)
            throw new ArgumentOutOfRangeException(nameof(JogToWalkExitSpeed),
                "Jog-to-walk exit speed must be lower than the walk-to-jog enter speed.");
        if (JogToWalkExitSpeed < StopSpeed || JogToWalkExitSpeed >= JogSpeed)
            throw new ArgumentOutOfRangeException(nameof(JogToWalkExitSpeed),
                "Jog-to-walk exit speed must sit at or above stop speed and below jog speed.");
        if (!float.IsFinite(MinimumDirectionLengthSquared) || MinimumDirectionLengthSquared <= 0)
            throw new ArgumentOutOfRangeException(nameof(MinimumDirectionLengthSquared));
        if (!float.IsFinite(ForwardBoundaryDegrees) || ForwardBoundaryDegrees <= 0 ||
            ForwardBoundaryDegrees >= 90)
            throw new ArgumentOutOfRangeException(nameof(ForwardBoundaryDegrees));
        if (!float.IsFinite(BackwardBoundaryDegrees) ||
            BackwardBoundaryDegrees <= ForwardBoundaryDegrees || BackwardBoundaryDegrees >= 180)
            throw new ArgumentOutOfRangeException(nameof(BackwardBoundaryDegrees));
        if (!float.IsFinite(MinimumMoveSpeed) || MinimumMoveSpeed < 0)
            throw new ArgumentOutOfRangeException(nameof(MinimumMoveSpeed));
        if (!float.IsFinite(JogMaxSpeed) || JogMaxSpeed <= 0)
            throw new ArgumentOutOfRangeException(nameof(JogMaxSpeed));
        if (!float.IsFinite(JogInputMagnitude) || JogInputMagnitude < 0 || JogInputMagnitude > 1)
            throw new ArgumentOutOfRangeException(nameof(JogInputMagnitude));
        if (!float.IsFinite(WalkInputMagnitude) || WalkInputMagnitude < 0 ||
            WalkInputMagnitude > JogInputMagnitude)
            throw new ArgumentOutOfRangeException(nameof(WalkInputMagnitude));
        if (!float.IsFinite(BackwardDotThreshold) || BackwardDotThreshold < -1 || BackwardDotThreshold >= 0)
            throw new ArgumentOutOfRangeException(nameof(BackwardDotThreshold));
        if (!float.IsFinite(JogStopSpeed) || JogStopSpeed <= MinimumMoveSpeed)
            throw new ArgumentOutOfRangeException(nameof(JogStopSpeed));
    }

    private static void RequireFinitePositive(float value, string name)
    {
        if (!float.IsFinite(value) || value <= 0)
            throw new ArgumentOutOfRangeException(name);
    }
}

// 纯决策组件：输入世界方向/速度/接地，输出步态与一次性转移。
// 不持有播放时钟、不调用 AnimationPlayback、不依赖任何具体 Blueprint。
public sealed class LocomotionStateMachine
{
    private const float RadiansToDegrees = 180f / MathF.PI;

    private LocomotionState state;
    private LocomotionMode mode;
    private LocomotionDirection direction;

    public LocomotionStateMachine(LocomotionSettings? settings = null)
    {
        Settings = settings ?? new LocomotionSettings();
        Settings.Validate();
        Reset();
    }

    public LocomotionSettings Settings { get; }
    public LocomotionState State => state;
    public LocomotionMode Mode => mode;
    public LocomotionDirection Direction => direction;

    // 传送、重生或 Play 重新构造实例后复位；不保留上一段动作的滞回状态。
    public void Reset(LocomotionState initialState = LocomotionState.Idle,
        LocomotionMode initialMode = LocomotionMode.Grounded)
    {
        if (!Enum.IsDefined(initialState)) throw new ArgumentOutOfRangeException(nameof(initialState));
        if (!Enum.IsDefined(initialMode)) throw new ArgumentOutOfRangeException(nameof(initialMode));
        state = initialState;
        mode = initialMode;
        direction = LocomotionDirection.Forward;
    }

    public LocomotionDecision Update(in LocomotionInput input)
    {
        ValidateInput(input);

        bool hasDirection = input.DesiredDirection.LengthSquared() > Settings.MinimumDirectionLengthSquared;
        float targetYaw = NormalizeDegrees(input.FacingYawDegrees);
        // 方向分桶需要知道本次将进入的步态，以便选择对应 Walk/Jog 转向曲线。
        // 空中仍沿用当前地面步态，不能把 Fall 片段误当成 Jog 条件。
        var selectedState = input.Grounded ? SelectState(input, state) : state;
        if (hasDirection)
        {
            // VL 角色 yaw=0 面向 -Z；与 GaspCharacter 的 atan2(-x,-z) 使用同一约定。
            targetYaw = NormalizeDegrees(
                MathF.Atan2(-input.DesiredDirection.X, -input.DesiredDirection.Z) * RadiansToDegrees);
            direction = SelectDirection(input,
                NormalizeDegrees(targetYaw - input.FacingYawDegrees), selectedState);
        }

        var transition = LocomotionTransition.None;
        if (!input.Grounded)
        {
            if (mode == LocomotionMode.Grounded)
            {
                mode = LocomotionMode.Airborne;
                transition = LocomotionTransition.Fall;
            }
        }
        else if (mode == LocomotionMode.Airborne)
        {
            // 落地只发一次 Land；同一帧按输入选出落地后的步态，不再叠加 Start/Stop。
            mode = LocomotionMode.Grounded;
            state = selectedState;
            transition = LocomotionTransition.Land;
        }
        else
        {
            if (selectedState != state)
            {
                transition = TransitionFor(state, selectedState, input.PlanarSpeed);
                state = selectedState;
            }
        }

        float targetSpeed = mode == LocomotionMode.Airborne
            ? input.DesiredSpeed
            : state switch
            {
                LocomotionState.Idle => 0,
                LocomotionState.Walk => Settings.WalkSpeed,
                LocomotionState.Jog => Settings.JogSpeed,
                _ => throw new ArgumentOutOfRangeException(nameof(state))
            };
        return new(state, mode, transition, direction, input.DesiredSpeed, targetSpeed, targetYaw);
    }

    private LocomotionState SelectState(in LocomotionInput input, LocomotionState current)
    {
        if (Settings.UseUeStateConditions)
        {
            float inputMagnitude = EffectiveInputMagnitude(input);
            float maxSpeed = EffectiveMaxSpeed(input);
            bool hasInput = input.DesiredDirection.LengthSquared() > Settings.MinimumDirectionLengthSquared &&
                inputMagnitude >= Settings.WalkInputMagnitude;
            bool moving = input.PlanarSpeed >= Settings.MinimumMoveSpeed;
            bool backward = false;
            if (input.DesiredDirection.LengthSquared() > Settings.MinimumDirectionLengthSquared &&
                input.VelocityDirection.LengthSquared() > 1e-6f)
            {
                var inputDirection = Vector3.Normalize(input.DesiredDirection);
                var velocityDirection = Vector3.Normalize(input.VelocityDirection);
                backward = Vector3.Dot(inputDirection, velocityDirection) < Settings.BackwardDotThreshold;
            }
            // 与 UE DetermineLocomotionState 一致：离地/坐下/倒走/无输入或未产生
            // 实际速度时保持 Idle；Jog 由角色当前 MaxSpeed（Sprint 上限）决定。
            if (input.Sitting || !moving || !hasInput || backward) return LocomotionState.Idle;
            // 曲线是额外证据而非替代：它可以把步态提升为 Jog，但不会把显式
            // MaxSpeed/DesiredSpeed 已允许的 Jog 降回 Walk，避免起步曲线从低位
            // 爬升时产生 Jog→Walk→Jog 的阈值抖动。
            if (input.SpeedCurve >= 0) maxSpeed = MathF.Max(maxSpeed, input.SpeedCurve);
            if (maxSpeed >= Settings.JogMaxSpeed && inputMagnitude >= Settings.JogInputMagnitude)
                return LocomotionState.Jog;
            return LocomotionState.Walk;
        }
        float desiredSpeed = input.SpeedCurve >= 0
            ? MathF.Max(input.DesiredSpeed, input.SpeedCurve) : input.DesiredSpeed;
        if (desiredSpeed <= Settings.StopSpeed) return LocomotionState.Idle;
        if (desiredSpeed >= Settings.WalkToJogEnterSpeed) return LocomotionState.Jog;
        if (desiredSpeed <= Settings.JogToWalkExitSpeed) return LocomotionState.Walk;
        // 滞回带内保持当前步态；从 Idle 进入时默认走 Walk。
        return current == LocomotionState.Jog ? LocomotionState.Jog : LocomotionState.Walk;
    }

    private LocomotionTransition TransitionFor(LocomotionState from, LocomotionState to, float planarSpeed)
    {
        if (to == LocomotionState.Idle)
        {
            // UE Idle 内层 Entry：Speed > 200 进 Jog Stop，Speed > 1 进 Walk Stop。
            float walkStopSpeed = Settings.UseUeStateConditions ? Settings.MinimumMoveSpeed : Settings.StopSpeed;
            if (planarSpeed <= walkStopSpeed) return LocomotionTransition.None;
            return planarSpeed > Settings.JogStopSpeed
                ? LocomotionTransition.StopJog
                : LocomotionTransition.StopWalk;
        }
        if (from == LocomotionState.Idle)
            return to == LocomotionState.Jog
                ? LocomotionTransition.StartJog
                : LocomotionTransition.StartWalk;
        // Walk <-> Jog 只切换循环片段，不重复播放 Idle 起步动作。
        return LocomotionTransition.None;
    }

    private LocomotionDirection ClassifyDirection(float relativeYaw)
    {
        float magnitude = MathF.Abs(relativeYaw);
        if (magnitude <= Settings.ForwardBoundaryDegrees) return LocomotionDirection.Forward;
        if (magnitude >= Settings.BackwardBoundaryDegrees)
            // 180° 正好落在分界上时固定归入右侧，避免 atan2 的 ±180 抖动改变片段选择。
            return relativeYaw >= 0 ? LocomotionDirection.Left180 : LocomotionDirection.Right180;
        return relativeYaw > 0 ? LocomotionDirection.Left90 : LocomotionDirection.Right90;
    }

    private LocomotionDirection SelectDirection(
        in LocomotionInput input, float relativeYaw, LocomotionState gait)
    {
        float deltaCurve = gait == LocomotionState.Jog
            ? input.JogRotationDeltaCurve : input.WalkRotationDeltaCurve;
        if (deltaCurve >= 0) relativeYaw *= deltaCurve;
        return ClassifyDirection(NormalizeDegrees(relativeYaw));
    }

    private static float NormalizeDegrees(float degrees)
    {
        float normalized = degrees % 360f;
        if (normalized >= 180f) normalized -= 360f;
        if (normalized < -180f) normalized += 360f;
        return normalized;
    }

    private static void ValidateInput(in LocomotionInput input)
    {
        if (!float.IsFinite(input.DesiredDirection.X) || !float.IsFinite(input.DesiredDirection.Y) ||
            !float.IsFinite(input.DesiredDirection.Z))
            throw new ArgumentOutOfRangeException(nameof(input), "Desired direction must be finite.");
        if (!float.IsFinite(input.DesiredSpeed) || input.DesiredSpeed < 0)
            throw new ArgumentOutOfRangeException(nameof(input), "Desired speed must be finite and non-negative.");
        if (!float.IsFinite(input.PlanarSpeed) || input.PlanarSpeed < 0)
            throw new ArgumentOutOfRangeException(nameof(input), "Planar speed must be finite and non-negative.");
        if (!float.IsFinite(input.FacingYawDegrees))
            throw new ArgumentOutOfRangeException(nameof(input), "Facing yaw must be finite.");
        if (input.InputMagnitude >= 0 && (!float.IsFinite(input.InputMagnitude) || input.InputMagnitude > 1))
            throw new ArgumentOutOfRangeException(nameof(input), "Input magnitude must be finite in [0,1].");
        if (input.MaxSpeed >= 0 && !float.IsFinite(input.MaxSpeed))
            throw new ArgumentOutOfRangeException(nameof(input), "Max speed must be finite.");
        if (!float.IsFinite(input.VelocityDirection.X) || !float.IsFinite(input.VelocityDirection.Y) ||
            !float.IsFinite(input.VelocityDirection.Z))
            throw new ArgumentOutOfRangeException(nameof(input), "Velocity direction must be finite.");
        if (input.SpeedCurve >= 0 && !float.IsFinite(input.SpeedCurve))
            throw new ArgumentOutOfRangeException(nameof(input), "Speed curve must be finite or missing.");
        ValidateRotationDeltaCurve(input.WalkRotationDeltaCurve, "Walk rotation delta curve");
        ValidateRotationDeltaCurve(input.JogRotationDeltaCurve, "Jog rotation delta curve");
    }

    private static void ValidateRotationDeltaCurve(float value, string name)
    {
        if (value >= 0 && (!float.IsFinite(value) || value > 1))
            throw new ArgumentOutOfRangeException(nameof(value), $"{name} must be finite in [0,1] or missing.");
    }

    private static float EffectiveInputMagnitude(in LocomotionInput input) =>
        input.InputMagnitude < 0 ? MathF.Min(1, input.DesiredDirection.Length()) : input.InputMagnitude;

    private static float EffectiveMaxSpeed(in LocomotionInput input) =>
        input.MaxSpeed < 0 ? input.DesiredSpeed : input.MaxSpeed;
}
