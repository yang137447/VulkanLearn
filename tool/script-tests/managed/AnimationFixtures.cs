using VulkanLearn;
using System.Numerics;

namespace ScriptFixtures;

public sealed class AnimationProbe : Blueprint
{
    [Expose(InstanceEditable = true)] public string Mode { get; set; } = "";
    private MeshComponent? removed;
    private int frame;
    protected override void OnCreate()
    {
        if (Mode == "create") GetComponent<MeshComponent>("visual").Animation.Play("walk");
    }
    protected override void BeginPlay()
    {
        if (Mode == "machine")
        {
            var machine = new StateMachine<DayOfWeek>(DayOfWeek.Monday);
            machine.Add(DayOfWeek.Monday, DayOfWeek.Tuesday, () => true);
            machine.Add(DayOfWeek.Monday, DayOfWeek.Wednesday, () => true, 100);
            machine.Add(DayOfWeek.Monday, DayOfWeek.Thursday, () => true, 100);
            machine.Add(DayOfWeek.Wednesday, DayOfWeek.Friday, () => true, 1000);
            machine.Update();
            Transform.Position = new((int)machine.Current, 0, 0);
            return;
        }
        GetComponent<MeshComponent>("visual").Animation.Play("walk", loop: Mode != "oneshot", rate: 2);
    }
    protected override void Tick(float dt)
    {
        var animation = GetComponent<MeshComponent>("visual").Animation;
        if (Mode == "invalid") animation.Play("missing");
        if (Mode == "throw")
        {
            animation.Play("jump", loop: false);
            throw new InvalidOperationException("Reject this callback's animation command");
        }
        if (Mode == "remove")
        {
            if (frame++ == 0)
            {
                removed = GetComponent<MeshComponent>("extra");
                RemoveComponent(removed.Handle);
            }
            else removed!.Animation.Play("walk");
        }
        if (Mode == "feedback")
            Transform.Position = new(animation.State.Time, animation.State.Completed ? 1 : 0, 0);
    }
}

// P3 固定片段回归夹具；正式 GASP 已改由 MM 选帧，这里单独保留旧接口/Jolt 历史断言。
public sealed class LegacyLocomotionFixture : Blueprint
{
    private AnimationPlayback animation = null!;
    private string current = "";
    private bool landing;
    protected override void OnCreate() => animation = GetComponent<MeshComponent>("visual").Animation;
    protected override void BeginPlay() => Play("M_Neutral_Stand_Idle_Loop", true);
    protected override void Tick(float dt)
    {
        var move = Input.GetVector2("Move");
        if (move.LengthSquared() > 1) move = Vector2.Normalize(move);
        var direction = new Vector3(move.X, 0, -move.Y);
        Character!.Move(direction * (Input.IsPressed("Sprint") ? 450 : 200));
        if (direction.LengthSquared() > 0.0001f)
            Transform.Rotation = new(0, MathF.Atan2(-direction.X, -direction.Z) * 180 / MathF.PI, 0);
        if (Input.WasPressed("Jump")) Character.Jump();
    }
    protected override void LateTick(float dt)
    {
        var velocity = Character!.LinearVelocity;
        string clip;
        bool loop = true;
        if (!Character.IsGrounded)
        {
            landing = true;
            clip = velocity.Y > 0 ? "M_Neutral_Jump_F_Start_Stand_Lfoot" : "M_Neutral_Jump_Loop_Fall";
            loop = velocity.Y <= 0;
        }
        else if (landing)
        {
            clip = "M_Neutral_Jump_F_Land_Stand_Light_Lfoot"; loop = false;
            if (current == clip && animation.State.Completed) landing = false;
        }
        else
        {
            float speed = new Vector2(velocity.X, velocity.Z).Length();
            clip = speed < 2 ? "M_Neutral_Stand_Idle_Loop" :
                speed > 325 ? "M_Neutral_Run_Loop_F" : "M_Neutral_Walk_Loop_F";
        }
        if (clip != current) Play(clip, loop);
    }
    private void Play(string clip, bool loop)
    {
        current = clip;
        animation.Play(clip, loop);
    }
}

public sealed class MotionMatchingProbe : Blueprint
{
    [Expose(InstanceEditable = true)] public string Mode { get; set; } = "";
    private bool idleSubmitted;
    private static readonly AssetId Database = new("Maps/SC_gasp/MotionMatching/DB_gasp.json");
    private static readonly string[] Ground =
    [
        "M_Neutral_Stand_Idle_Loop", "M_Neutral_Walk_Loop_F",
        "M_Neutral_Walk_Start_F_Lfoot", "M_Neutral_Walk_Stop_F_Lfoot",
        "M_Neutral_Stand_Turn_180_L", "M_Neutral_Stand_Turn_180_R"
    ];
    protected override void BeginPlay()
    {
        GetComponent<MeshComponent>("visual").Animation.Match(Database, Ground);
    }
    protected override void Tick(float dt)
    {
        var animation = GetComponent<MeshComponent>("visual").Animation;
        // 生命周期夹具先保留原移动查询，再通过真实物理停止，为锁足冻结建立静止前提。
        bool stopped = Mode == "move-then-idle" && Time.TimeSincePlay >= 1;
        Character?.Move(stopped ? Vector3.Zero : new Vector3(0, 0, -200));
        if (stopped && !idleSubmitted)
        {
            animation.Match(Database, ["M_Neutral_Stand_Idle_Loop"], footPlacement: true);
            idleSubmitted = true;
        }
        if (Mode == "empty") animation.Match(Database, []);
        if (Mode == "filtered") animation.Match(Database, ["M_Neutral_Walk_Loop_F"]);
        if (Mode == "throw")
        {
            animation.Match(Database, []);
            throw new InvalidOperationException("Reject this callback's candidate filter");
        }
        if (Mode == "teleport") { Transform.Position += new Vector3(3000, 0, 0); Mode = ""; }
    }
}

// catalog + Montage 提交面夹具；真实 Hurdle sidecar/曲线与 Montage 夹具由原生测试宿主注入。
public sealed class MontageProbe : Blueprint
{
    [Expose(InstanceEditable = true)] public string Mode { get; set; } = "";
    private const string MontageId = "AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot";
    private static readonly string[] TargetNames = ["FrontLedge", "BackLedge", "BackFloor"];
    private AnimationPlayback animation = null!;
    private int notifyCount;
    private float firstNotifyTime = -1;
    private int firstNotifyPhase;

    protected override void OnCreate() => animation = GetComponent<MeshComponent>("visual").Animation;
    protected override void BeginPlay()
    {
        if (Mode is "montage" or "feedback" or "notify") StartMontage();
        else if (Mode == "montage-start") StartMontage(0.877f);
    }

    protected override void Tick(float dt)
    {
        var montage = animation.Montage(MontageId);
        switch (Mode)
        {
            case "curve":
                // golden：UE 参考 0.598017→34.098671、0.831335→90.908951；线性 sidecar 容差 0.5 cm。
                Transform.Position = new(
                    animation.CurveValue(montage.ClipId, "distance_from_ledge", 0.598017f),
                    animation.CurveValue(montage.ClipId, "DISTANCE_FROM_LEDGE", 0.831335f),
                    animation.CurveValue(montage.ClipId, "distance_from_ledge", 999f));
                break;
            case "curve-clamp":
                Transform.Position = new(
                    animation.CurveValue(montage.ClipId, "distance_from_ledge", -100f),
                    animation.CurveValue(montage.ClipId, "disableleftfootik", 1234f),
                    0);
                break;
            case "curve-errors":
            {
                var caught = 0;
                try { animation.CurveValue("missing", "distance_from_ledge", 0); }
                catch (ArgumentException) { caught++; }
                try { animation.CurveValue(montage.ClipId, "missing_curve", 0); }
                catch (ArgumentException) { caught++; }
                try { animation.CurveValue(montage.ClipId, "distance_from_ledge", float.NaN); }
                catch (ArgumentOutOfRangeException) { caught++; }
                try { animation.CurveValue(montage.ClipId, "distance_from_ledge", float.PositiveInfinity); }
                catch (ArgumentOutOfRangeException) { caught++; }
                Transform.Position = new(caught, 0, 0);
                break;
            }
            case "montage-info":
            {
                var unknown = 0;
                try { animation.Montage("AM_missing"); }
                catch (ArgumentException) { unknown++; }
                Transform.Position = new(montage.Sections.Count, montage.Notifies.Count,
                    montage.Windows.Count + unknown * 100);
                break;
            }
            case "montage-reject":
            {
                var caught = 0;
                caught += Reject(() => animation.PlayMontage("AM_missing", Targets()));
                caught += Reject(() => animation.PlayMontage(MontageId, Targets("FrontLedge", "BackLedge")));
                caught += Reject(() => animation.PlayMontage(MontageId,
                    Targets("FrontLedge", "BackLedge", "BackFloor", "Extra")));
                // 多 section 与 Static provider 已由原生执行层支持，不再属于拒绝项。
                caught += Reject(() => animation.PlayMontage("AM_Test_NonLinear", Targets()));
                caught += Reject(() => animation.PlayMontage(MontageId, Targets(), loop: true));
                caught += Reject(() => animation.PlayMontage(MontageId, Targets(), rate: 0));
                Transform.Position = new(caught, 0, 0);
                break;
            }
            case "montage-rotation":
            {
                // 新增 WarpTarget 重载：位置 + 目标世界旋转。Hurdle 的两个 FrontLedge
                // 窗口 warpRotation=true，原生应接受 Slerp 并进入 Montage 播放。
                var caught = 0;
                var targets = new Dictionary<string, WarpTarget>(StringComparer.Ordinal);
                var rotation = Quaternion.CreateFromAxisAngle(Vector3.UnitY, MathF.PI / 2);
                foreach (var name in TargetNames)
                    targets[name] = new WarpTarget(new Vector3(0, 100, 200), rotation);
                try { animation.PlayMontage(MontageId, targets); }
                catch (ArgumentException) { caught++; }
                Transform.Position = new(caught, 0, 0);
                break;
            }
            case "montage-start":
            {
                // 入口时间在 C# 入口只允许有限非负；落在 section 0 之外由原生按资产拒绝。
                var caught = 0;
                caught += RejectRange(() => animation.PlayMontage(MontageId, Targets(), startTime: -1));
                caught += RejectRange(() => animation.PlayMontage(MontageId, Targets(), startTime: float.NaN));
                caught += RejectRange(() =>
                    animation.PlayMontage(MontageId, Targets(), startTime: float.PositiveInfinity));
                var state = animation.State;
                Transform.Position = new(caught, state.MontageRange.X, state.Time);
                break;
            }
            case "feedback":
            {
                var state = animation.State;
                Transform.Position = new(string.IsNullOrEmpty(state.Montage) ? 0 : 1,
                    state.MontageLength, state.MontageRange.Y);
                break;
            }
            case "notify":
            {
                foreach (var notify in animation.State.Notifies ?? [])
                {
                    if (notifyCount++ == 0)
                    {
                        firstNotifyTime = notify.Time;
                        firstNotifyPhase = notify.Phase == "begin" ? 1 :
                            notify.Phase == "instant" ? 2 : notify.Phase == "end" ? 3 : 0;
                    }
                    Log.Info($"notify:{notify.Name}:{notify.Phase}:{notify.Kind}:{notify.Interrupted}");
                }
                Transform.Position = new(notifyCount, firstNotifyTime, firstNotifyPhase);
                break;
            }
        }
    }

    private static Dictionary<string, Vector3> Targets(params string[] names)
    {
        var result = new Dictionary<string, Vector3>(StringComparer.Ordinal);
        foreach (var name in names.Length == 0 ? TargetNames : names)
            result[name] = new Vector3(0, 100, 200);
        return result;
    }

    private static int Reject(Action action)
    {
        try { action(); return 0; }
        catch (ArgumentException error) { Log.Info("montage-reject:" + error.Message); return 1; }
    }

    private static int RejectRange(Action action)
    {
        try { action(); return 0; }
        catch (ArgumentOutOfRangeException) { return 1; }
    }

    private void StartMontage(float startTime = 0) =>
        animation.PlayMontage(MontageId, Targets(), startTime: startTime);
}
