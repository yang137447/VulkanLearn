using System.Numerics;
using System.Runtime.CompilerServices;

namespace VulkanLearn;

[AttributeUsage(AttributeTargets.Property)]
public sealed class ExposeAttribute : Attribute
{
    // Expose 只开放默认配置；inline Blueprint 必须另外声明实例编辑权限。
    public bool InstanceEditable { get; set; }
}

public readonly record struct BlueprintHandle(ulong Context, string Id);

// 作者引用只存稳定标识，不能把某次 Play 的上下文句柄写进 BP/Scene。
public readonly record struct AssetId(string Path);
public readonly record struct BlueprintReference(string Id)
{
    public BlueprintHandle? Resolve(BlueprintWorld world) => string.IsNullOrEmpty(Id) ? null : world.Find(Id);
}

public enum EndPlayReason { Stopped, WorldChanged, Shutdown }

// Transform 是本次 GT 回调的值缓存；宿主在整个回调批次成功返回后才写入原生 World。
public sealed class Transform
{
    internal Action? CheckAccess;
    private Vector3 position, rotation, scale = Vector3.One;
    public Vector3 Position { get { CheckAccess?.Invoke(); return position; } set { CheckAccess?.Invoke(); position = value; } }
    public Vector3 Rotation { get { CheckAccess?.Invoke(); return rotation; } set { CheckAccess?.Invoke(); rotation = value; } }
    public Vector3 Scale { get { CheckAccess?.Invoke(); return scale; } set { CheckAccess?.Invoke(); scale = value; } }
    public void Translate(Vector3 offset) => Position += offset;
    public void Rotate(Vector3 degrees) => Rotation += degrees;
    public Matrix4x4 Matrix => Matrix4x4.CreateScale(Scale) *
        Matrix4x4.CreateFromQuaternion(Quaternion.CreateFromYawPitchRoll(
            Rotation.Y * MathF.PI / 180, Rotation.X * MathF.PI / 180, Rotation.Z * MathF.PI / 180)) *
        Matrix4x4.CreateTranslation(Position);
}

// 组件集合在回调结束后统一换代；局部变换与父链保留完整矩阵，旧组件句柄不能复用。
public class SceneComponent
{
    public ComponentHandle Handle { get; internal set; }
    public bool IsAlive => AliveQuery?.Invoke() ?? false;
    internal Func<bool>? AliveQuery;
    public string Id { get; internal set; } = "";
    public string Kind { get; internal set; } = "";
    public SceneComponent? Parent { get; internal set; }
    internal Blueprint Owner = null!;
    public Transform Transform { get; } = new();
    // 子组件局部 TRS 相对父挂接点；SpringArm 的组件原点仍在臂根，挂接点在臂端。
    public Matrix4x4 WorldMatrix => Transform.Matrix * (Parent?.GetSocketTransform() ?? Owner.Transform.Matrix);
    public Quaternion WorldRotation => Quaternion.Normalize(
        (Parent?.SocketWorldRotation ?? NativeBridge.Degrees(Owner.Transform.Rotation)) *
        NativeBridge.Degrees(Transform.Rotation));
    public virtual Matrix4x4 GetSocketTransform() => WorldMatrix;
    internal virtual Quaternion SocketWorldRotation => WorldRotation;
}

// 相机只发布镜头值；实际渲染相机由原生 World 拥有，Activate 受所属 GT 回调边界约束。
public sealed class CameraComponent : SceneComponent
{
    public float HorizontalFov { get; set; } = 60;
    public float NearClip { get; set; } = 1;
    public float FarClip { get; set; } = 10000;
    // autoActivate 相机的初始选择优先级；同优先级按声明顺序取首个。
    private int priority;
    public int Priority
    {
        get => priority;
        set => priority = value >= -100000 && value <= 100000
            ? value : throw new ArgumentOutOfRangeException(nameof(Priority));
    }
    internal Action<bool> Activation = null!;
    internal Func<bool> ActiveQuery = null!;
    public bool IsActive => ActiveQuery();
    public void Activate() => Activation(true);
    public void Deactivate() => Activation(false);
}

// 强度固定为方向光 lux、点光/聚光 cd；Scale 只参与位置合成，不修改辐射量或锥角。
public abstract class LightComponent : SceneComponent
{
    public Vector3 Color { get; set; } = Vector3.One;
    public float Intensity { get; set; } = 1000;
    public bool Visible { get; set; } = true;
}
public sealed class PointLightComponent : LightComponent { }
public sealed class DirectionalLightComponent : LightComponent { }
public sealed class SpotLightComponent : LightComponent
{
    public float InnerConeAngle { get; set; } = 30;
    public float OuterConeAngle { get; set; } = 45;
}

public sealed class RigidBody
{
    internal Vector3 Velocity;
    internal Vector3 Impulse;
    internal bool VelocityChanged;
    public string Motion { get; internal set; } = "";
    public Vector3 LinearVelocity { get => Velocity; set { Velocity = value; VelocityChanged = true; } }
    public void AddImpulse(Vector3 impulse) => Impulse += impulse;
}

// 服务仅在所属 Blueprint 的 GT 回调中可用，结构请求在该批回调结束后处理。
public sealed class BlueprintWorld
{
    internal Func<string, Vector3, Vector3?, Vector3?, BlueprintHandle> SpawnRequest = null!;
    internal Action<BlueprintHandle> DestroyRequest = null!;
    internal Func<BlueprintHandle, bool> AliveQuery = null!;
    internal Func<string, BlueprintHandle?> FindQuery = null!;
    internal Func<string, IReadOnlyList<BlueprintHandle>> FindAllQuery = null!;
    internal Func<BlueprintHandle, BlueprintInfo> BlueprintQuery = null!;
    public BlueprintHandle Spawn(string assetPath, Vector3 position, Vector3? rotation = null, Vector3? scale = null)
        => SpawnRequest(assetPath, position, rotation, scale);
    public BlueprintHandle Spawn(AssetId asset, Vector3 position, Vector3? rotation = null, Vector3? scale = null)
        => SpawnRequest(asset.Path, position, rotation, scale);
    public void Destroy(BlueprintHandle blueprint) => DestroyRequest(blueprint);
    public bool IsAlive(BlueprintHandle blueprint) => AliveQuery(blueprint);
    public BlueprintHandle? Find(string id) => FindQuery(id);
    public IReadOnlyList<BlueprintHandle> FindAll(string className = "") => FindAllQuery(className);
    public BlueprintInfo GetBlueprint(BlueprintHandle blueprint) => BlueprintQuery(blueprint);
}

public readonly record struct BlueprintInfo(BlueprintHandle Handle, string Name, string ClassName,
    Vector3 Position, Vector3 Rotation, Vector3 Scale, bool Enabled);

public abstract partial class Blueprint
{
    public BlueprintHandle Handle { get; internal set; }
    public string Name { get; internal set; } = "";
    public bool Enabled { get; set; } = true;
    public Transform Transform { get; } = new();
    // 供 UsePawnControlRotation 使用的视角控制量（Pitch/Yaw/Roll 度）；默认跟随 actor 初始朝向，
    // 正式控制器可在 Tick 里按玩家输入更新它，SpringArm 只读取，不反写 actor。
    private Vector3 controlRotation;
    [Migrate] public Vector3 ControlRotation
    {
        get => controlRotation;
        set => controlRotation = float.IsFinite(value.X) && float.IsFinite(value.Y) && float.IsFinite(value.Z)
            ? value : throw new ArgumentOutOfRangeException(nameof(ControlRotation));
    }
    public Input Input { get; internal set; } = new();
    public Time Time { get; internal set; } = new();
    public IReadOnlyList<SceneComponent> Components { get; internal set; } = Array.Empty<SceneComponent>();
    public SceneComponent GetComponent(string id) => Components.Single(c => c.Id == id);
    public T GetComponent<T>(string id) where T : SceneComponent => (T)GetComponent(id);
    public BlueprintWorld World { get; internal set; } = new();
    public RigidBody? Body { get; internal set; }
    public CharacterController? Character { get; internal set; }
    public BlueprintTimers Timers { get; internal set; } = new();
    public BlueprintEvents Events { get; internal set; } = new();
    public BlueprintPhysics Physics { get; internal set; } = new();
    public BlueprintAssets Assets { get; internal set; } = new();
    protected virtual void OnCreate() { }
    protected virtual void BeginPlay() { }
    protected virtual void Tick(float deltaTime) { }
    protected virtual void LateTick(float deltaTime) { }
    protected virtual void EndPlay(EndPlayReason reason) { }
    protected virtual void OnCollisionEnter(BlueprintHandle other) { }
    protected virtual void OnCollisionExit(BlueprintHandle other) { }
    protected virtual void OnTriggerEnter(BlueprintHandle other) { }
    protected virtual void OnTriggerExit(BlueprintHandle other) { }
    internal void Create() => OnCreate();
    internal void Begin() => BeginPlay();
    internal void Update(float dt) => Tick(dt);
    internal void UpdateLate(float dt) => LateTick(dt);
    internal void End(EndPlayReason reason) => EndPlay(reason);
    internal void Collision(BlueprintHandle other, bool enter)
    { if (enter) OnCollisionEnter(other); else OnCollisionExit(other); }
    internal void Trigger(BlueprintHandle other, bool enter)
    { if (enter) OnTriggerEnter(other); else OnTriggerExit(other); }
    public void Destroy() => PendingDestroy = true;
    internal bool PendingDestroy { get; set; }
}

public sealed class Input
{
    internal Vector2 Movement;
    internal float Vertical;
    internal Dictionary<string, (bool Held, bool Pressed, bool Released)> Actions = new(StringComparer.Ordinal);
    internal Dictionary<string, float> Axes = new(StringComparer.Ordinal);
    public bool IsPressed(string action) => Button(action).Held;
    public bool WasPressed(string action) => Button(action).Pressed;
    public bool WasReleased(string action) => Button(action).Released;
    private (bool Held, bool Pressed, bool Released) Button(string name) =>
        Actions.TryGetValue(name, out var state) ? state : throw new ArgumentException($"Unknown input action: {name}");
    public Vector2 GetVector2(string action) => action == "Move" ? new(GetAxis("MoveRight"), GetAxis("MoveForward"))
        : throw new ArgumentException($"Unknown input action: {action}");
    public float GetAxis(string action) => Axes.TryGetValue(action, out var axis) ? axis : action switch
    {
        "MoveForward" => Movement.Y, "MoveRight" => Movement.X, "MoveUp" => Vertical,
        _ => throw new ArgumentException($"Unknown input axis: {action}")
    };
}

public static class Log
{
    [ThreadStatic] internal static List<string>? Messages;
    [ThreadStatic] internal static List<object>? Entries;
    private static void Write(string severity, string message, string file, int line)
    {
        (Messages ?? throw new InvalidOperationException("Log is only available during a game-thread callback.")).Add(message);
        Entries?.Add(new { severity, message, file, line, column = 0 });
    }
    public static void Debug(string message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0) =>
        Write("Debug", message, file, line);
    public static void Info(string message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0) =>
        Write("Info", message, file, line);
    public static void Warning(string message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0) =>
        Write("Warning", message, file, line);
    public static void Error(string message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0) =>
        Write("Error", message, file, line);
}
