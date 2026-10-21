using System.Numerics;
using System.Text.Json;

namespace VulkanLearn;

// GT 在物理、LateTick 和动画指令同步后求臂端，再批量扫球；RT 只消费最终相机快照。
// UE 的 -X 后向映射到 VL 的 +Z；长度和 SocketOffset 使用厘米，不随父级缩放伸长。
public sealed class SpringArmComponent : SceneComponent
{
    private float targetArmLength = 300;
    private float probeRadius = 12;
    private float cameraLagSpeed = 10;
    private float cameraLagMaxDistance;
    private float cameraRotationLagSpeed = 10;
    private uint collisionMask = 65535;
    private Vector3 socketOffset;
    private Vector3 targetOffset;
    private Vector3 rotationOffset;
    private float cameraLagMaxTimeStep = 1f / 60;

    public float TargetArmLength
    {
        get => targetArmLength;
        set => targetArmLength = Range(value, 0, 100000, nameof(TargetArmLength));
    }

    public Vector3 SocketOffset
    {
        get => socketOffset;
        set => socketOffset = Finite(value, nameof(SocketOffset));
    }
    public Vector3 TargetOffset
    {
        get => targetOffset;
        set => targetOffset = Finite(value, nameof(TargetOffset));
    }

    public float ProbeRadius
    {
        get => probeRadius;
        set => probeRadius = Range(value, 0, 100000, nameof(ProbeRadius));
    }

    public bool DoCollisionTest { get; set; } = true;

    public uint CollisionMask
    {
        get => collisionMask;
        set => collisionMask = value <= 65535 ? value : throw new ArgumentOutOfRangeException(nameof(CollisionMask));
    }

    // 与 UE VInterpTo 一致：alpha=clamp(speed*dt,0,1)，speed=0 直接到目标；lag 只作用于臂根。
    public bool EnableCameraLag { get; set; }
    public float CameraLagSpeed
    {
        get => cameraLagSpeed;
        set => cameraLagSpeed = Range(value, 0, 1000, nameof(CameraLagSpeed));
    }
    public float CameraLagMaxDistance
    {
        get => cameraLagMaxDistance;
        set => cameraLagMaxDistance = Range(value, 0, 100000, nameof(CameraLagMaxDistance));
    }

    public bool UseCameraLagSubstepping { get; set; } = true;
    public float CameraLagMaxTimeStep
    {
        get => cameraLagMaxTimeStep;
        set => cameraLagMaxTimeStep = Range(value, 0.005f, 0.5f, nameof(CameraLagMaxTimeStep));
    }

    // 禁止继承的轴保留臂的作者局部角；AbsoluteRotation 跳过父链旋转及逐轴继承。
    public bool EnableCameraRotationLag { get; set; }
    public float CameraRotationLagSpeed
    {
        get => cameraRotationLagSpeed;
        set => cameraRotationLagSpeed = Range(value, 0, 1000, nameof(CameraRotationLagSpeed));
    }
    public bool UsePawnControlRotation { get; set; }
    public bool InheritPitch { get; set; } = true;
    public bool InheritYaw { get; set; } = true;
    public bool InheritRoll { get; set; } = true;
    public bool AbsoluteRotation { get; set; }
    public Vector3 RotationOffset
    {
        get => rotationOffset;
        set => rotationOffset = Finite(value, nameof(RotationOffset));
    }
    public bool DrawDebug { get; set; }

    // 默认挂接点保留完整父缩放/shear，但臂长不随缩放伸长；所有类型子组件共用这一空间。
    public override Matrix4x4 GetSocketTransform()
    {
        var matrix = WorldMatrix;
        var rotation = SocketWorldRotation;
        matrix *= Matrix4x4.CreateFromQuaternion(rotation * Quaternion.Inverse(WorldRotation));
        matrix.Translation = HasSocket ? SocketPosition : WorldMatrix.Translation + TargetOffset +
            Vector3.Transform(new Vector3(0, 0, TargetArmLength) + SocketOffset, rotation);
        return matrix;
    }
    internal override Quaternion SocketWorldRotation =>
        HasSocket ? SmoothedWorldRotation : NativeBridge.Degrees(GetTargetRotation());

    public Vector3 GetTargetRotation() => NativeBridge.SpringArmRotation(this);
    public Vector3 GetUnfixedCameraPosition() => UnfixedCameraPosition;
    public bool IsCollisionFixApplied() => CollisionFixApplied;
    public void ResetCameraLag() => HasHistory = false;

    internal bool PendingProbe;
    internal bool NeedsEvaluation;
    internal bool HasSocket;
    internal Vector3 SocketPosition;
    internal bool HasHistory;
    internal Vector3 SmoothedWorldPosition;
    internal Quaternion SmoothedWorldRotation;
    internal Vector3 PreviousTargetPosition;
    internal Quaternion PreviousTargetRotation;
    internal Vector3 ProbeOrigin;
    internal Vector3 UnfixedCameraPosition;
    internal bool CollisionFixApplied;

    internal SpringArmComponent CaptureSettings() => (SpringArmComponent)MemberwiseClone();
    internal void RestoreSettings(SpringArmComponent source)
    {
        targetArmLength = source.targetArmLength; probeRadius = source.probeRadius;
        collisionMask = source.collisionMask; socketOffset = source.socketOffset; targetOffset = source.targetOffset;
        rotationOffset = source.rotationOffset; DoCollisionTest = source.DoCollisionTest;
        EnableCameraLag = source.EnableCameraLag; cameraLagSpeed = source.cameraLagSpeed;
        cameraLagMaxDistance = source.cameraLagMaxDistance; UseCameraLagSubstepping = source.UseCameraLagSubstepping;
        cameraLagMaxTimeStep = source.cameraLagMaxTimeStep; EnableCameraRotationLag = source.EnableCameraRotationLag;
        cameraRotationLagSpeed = source.cameraRotationLagSpeed; UsePawnControlRotation = source.UsePawnControlRotation;
        InheritPitch = source.InheritPitch; InheritYaw = source.InheritYaw; InheritRoll = source.InheritRoll;
        AbsoluteRotation = source.AbsoluteRotation; DrawDebug = source.DrawDebug; HasHistory = source.HasHistory;
    }

    private static float Range(float value, float min, float max, string name) =>
        float.IsFinite(value) && value >= min && value <= max
            ? value : throw new ArgumentOutOfRangeException(name);

    private static Vector3 Finite(Vector3 value, string name) =>
        float.IsFinite(value.X) && float.IsFinite(value.Y) && float.IsFinite(value.Z)
            ? value : throw new ArgumentOutOfRangeException(name);
}

public static partial class NativeBridge
{
    // 平滑历史只进入程序集迁移快照，不混入作者组件 schema 或逐帧导出。
    private static object CaptureSpringArmHistory(SpringArmComponent arm) => new
    {
        id = arm.Id, arm.HasHistory,
        position = Values(arm.SmoothedWorldPosition),
        rotation = Values(arm.SmoothedWorldRotation),
        previousPosition = Values(arm.PreviousTargetPosition),
        previousRotation = Values(arm.PreviousTargetRotation),
        unfixed = Values(arm.UnfixedCameraPosition), arm.CollisionFixApplied,
        arm.HasSocket, socket = Values(arm.SocketPosition)
    };

    private static void RestoreSpringArmHistory(Blueprint owner, JsonElement node)
    {
        var arm = owner.GetComponent<SpringArmComponent>(node.GetProperty("id").GetString()!)!;
        arm.HasHistory = node.GetProperty("HasHistory").GetBoolean();
        arm.SmoothedWorldPosition = Vector(node.GetProperty("position"));
        arm.SmoothedWorldRotation = ReadArmQuaternion(node.GetProperty("rotation"));
        arm.PreviousTargetPosition = Vector(node.GetProperty("previousPosition"));
        arm.PreviousTargetRotation = ReadArmQuaternion(node.GetProperty("previousRotation"));
        arm.UnfixedCameraPosition = Vector(node.GetProperty("unfixed"));
        arm.CollisionFixApplied = node.GetProperty("CollisionFixApplied").GetBoolean();
        arm.HasSocket = node.GetProperty("HasSocket").GetBoolean();
        arm.SocketPosition = Vector(node.GetProperty("socket"));
    }

    private static Quaternion ReadArmQuaternion(JsonElement node) =>
        new(node[0].GetSingle(), node[1].GetSingle(), node[2].GetSingle(), node[3].GetSingle());
    private static float[] Values(Quaternion value) => [value.X, value.Y, value.Z, value.W];

    private static void ReadSpringArmNode(SpringArmComponent arm, JsonElement node)
    {
        arm.TargetArmLength = node.TryGetProperty("targetArmLength", out var length) ? length.GetSingle() : 300;
        arm.SocketOffset = node.TryGetProperty("socketOffset", out var offset) ? Vector(offset) : Vector3.Zero;
        arm.TargetOffset = node.TryGetProperty("targetOffset", out var target) ? Vector(target) : Vector3.Zero;
        arm.ProbeRadius = node.TryGetProperty("probeRadius", out var radius) ? radius.GetSingle() : 12;
        arm.DoCollisionTest = !node.TryGetProperty("doCollisionTest", out var collision) || collision.GetBoolean();
        arm.CollisionMask = node.TryGetProperty("collisionMask", out var mask) ? UIntValue(mask) : 65535;
        arm.EnableCameraLag = node.TryGetProperty("enableCameraLag", out var lag) && lag.GetBoolean();
        arm.CameraLagSpeed = node.TryGetProperty("cameraLagSpeed", out var lagSpeed) ? lagSpeed.GetSingle() : 10;
        arm.CameraLagMaxDistance = node.TryGetProperty("cameraLagMaxDistance", out var lagMax) ? lagMax.GetSingle() : 0;
        arm.UseCameraLagSubstepping = !node.TryGetProperty("useCameraLagSubstepping", out var substep) || substep.GetBoolean();
        arm.CameraLagMaxTimeStep = node.TryGetProperty("cameraLagMaxTimeStep", out var step) ? step.GetSingle() : 1f / 60;
        arm.EnableCameraRotationLag = node.TryGetProperty("enableCameraRotationLag", out var rotLag) && rotLag.GetBoolean();
        arm.CameraRotationLagSpeed = node.TryGetProperty("cameraRotationLagSpeed", out var rotSpeed) ? rotSpeed.GetSingle() : 10;
        arm.UsePawnControlRotation = node.TryGetProperty("usePawnControlRotation", out var pawn) && pawn.GetBoolean();
        arm.InheritPitch = !node.TryGetProperty("inheritPitch", out var pitch) || pitch.GetBoolean();
        arm.InheritYaw = !node.TryGetProperty("inheritYaw", out var yaw) || yaw.GetBoolean();
        arm.InheritRoll = !node.TryGetProperty("inheritRoll", out var roll) || roll.GetBoolean();
        arm.AbsoluteRotation = node.TryGetProperty("absoluteRotation", out var absolute) && absolute.GetBoolean();
        arm.RotationOffset = node.TryGetProperty("rotationOffset", out var rotOffset) ? Vector(rotOffset) : Vector3.Zero;
        arm.DrawDebug = node.TryGetProperty("drawDebug", out var debug) && debug.GetBoolean();
        if (node.TryGetProperty("__springArmSocket", out var socket))
        {
            arm.HasSocket = true;
            arm.SocketPosition = Vector(socket.GetProperty("position"));
            arm.SmoothedWorldRotation = Degrees(Vector(socket.GetProperty("rotation")));
        }
    }

    private static int ComponentDepth(SceneComponent component)
    {
        int depth = 0;
        for (var parent = component.Parent; parent != null; parent = parent.Parent)
            ++depth;
        return depth;
    }

    internal static Quaternion Degrees(Vector3 degrees) =>
        Quaternion.CreateFromYawPitchRoll(degrees.Y * MathF.PI / 180, degrees.X * MathF.PI / 180,
            degrees.Z * MathF.PI / 180);

    private static Vector3 EulerDegrees(Quaternion q)
    {
        // VL 使用 YXZ：CreateFromYawPitchRoll 的逆；混合 pitch/roll 时不能使用 XYZ 公式。
        var sinPitch = Math.Clamp(2 * (q.W * q.X - q.Y * q.Z), -1f, 1f);
        var pitch = MathF.Asin(sinPitch);
        var yaw = MathF.Atan2(2 * (q.W * q.Y + q.Z * q.X), 1 - 2 * (q.X * q.X + q.Y * q.Y));
        var roll = MathF.Atan2(2 * (q.W * q.Z + q.X * q.Y), 1 - 2 * (q.X * q.X + q.Z * q.Z));
        if (MathF.Abs(sinPitch) > 0.999999f)
        {
            yaw = 2 * MathF.Atan2(q.Y, q.W);
            roll = 0;
        }
        return new Vector3(pitch, yaw, roll) * (180 / MathF.PI);
    }

    internal static Vector3 SpringArmRotation(SpringArmComponent arm)
    {
        var euler = arm.UsePawnControlRotation ? arm.Owner.ControlRotation :
            arm.AbsoluteRotation ? arm.Transform.Rotation : EulerDegrees(arm.WorldRotation);
        if (!arm.AbsoluteRotation)
        {
            if (!arm.InheritPitch) euler.X = arm.Transform.Rotation.X;
            if (!arm.InheritYaw) euler.Y = arm.Transform.Rotation.Y;
            if (!arm.InheritRoll) euler.Z = arm.Transform.Rotation.Z;
        }
        return EulerDegrees(Quaternion.Normalize(Degrees(euler) * Degrees(arm.RotationOffset)));
    }

    private static float LagAlpha(float speed, float dt) => speed <= 0 ? 1 : Math.Clamp(speed * dt, 0, 1);

    private static void PrepareSpringArm(SpringArmComponent arm, float dt, bool snap)
    {
        var origin = Vector3.Transform(Vector3.Zero, arm.WorldMatrix) + arm.TargetOffset;
        var rotation = Degrees(SpringArmRotation(arm));
        if (snap || !arm.HasHistory)
        {
            arm.SmoothedWorldPosition = origin;
            arm.SmoothedWorldRotation = rotation;
        }
        else
        {
            float elapsed = 0;
            // 子步沿前后两帧原始目标插值，避免用已滞后的结果估算目标速度导致帧率依赖。
            do
            {
                float step = arm.UseCameraLagSubstepping ? Math.Min(dt - elapsed, arm.CameraLagMaxTimeStep) : dt;
                elapsed += step;
                float fraction = dt > 0 ? Math.Min(elapsed / dt, 1) : 1;
                var positionTarget = Vector3.Lerp(arm.PreviousTargetPosition, origin, fraction);
                var rotationTarget = Quaternion.Slerp(arm.PreviousTargetRotation, rotation, fraction);
                arm.SmoothedWorldPosition = arm.EnableCameraLag
                    ? Vector3.Lerp(arm.SmoothedWorldPosition, positionTarget, LagAlpha(arm.CameraLagSpeed, step)) : origin;
                arm.SmoothedWorldRotation = arm.EnableCameraRotationLag
                    ? Quaternion.Normalize(Quaternion.Slerp(arm.SmoothedWorldRotation, rotationTarget,
                        LagAlpha(arm.CameraRotationLagSpeed, step))) : rotation;
            }
            while (elapsed < dt - 1e-7f);
        }
        if (arm.EnableCameraLag && arm.CameraLagMaxDistance > 0)
        {
            var lag = arm.SmoothedWorldPosition - origin;
            var distance = lag.Length();
            if (distance > arm.CameraLagMaxDistance)
                arm.SmoothedWorldPosition = origin + lag * (arm.CameraLagMaxDistance / distance);
        }
        arm.PreviousTargetPosition = origin;
        arm.PreviousTargetRotation = rotation;
        arm.HasHistory = true;
        arm.ProbeOrigin = origin;
        arm.UnfixedCameraPosition = arm.SmoothedWorldPosition +
            Vector3.Transform(new Vector3(0, 0, arm.TargetArmLength) + arm.SocketOffset, arm.SmoothedWorldRotation);
    }

    private static void UpdateSpringArmTarget(Context context, SpringArmComponent arm, RaycastHit? hit = null)
    {
        // Jolt 球扫的 Point/Distance 已是接触时的球心，不能再次减半径。
        var target = hit?.Point ?? arm.UnfixedCameraPosition;
        arm.CollisionFixApplied = Vector3.DistanceSquared(target, arm.UnfixedCameraPosition) > 1e-6f;
        // 只更新父挂接空间，不改写任何子组件的作者局部 TRS。
        arm.SocketPosition = target;
        arm.HasSocket = true;
        if (context.InstanceIds.TryGetValue(arm.Owner.Handle.Id, out var instance)) instance.Dirty = true;
        arm.PendingProbe = false;
        if (arm.DrawDebug)
        {
            context.Services.QueueSpringArmDebug(arm.Owner, arm.Handle.Generation.ToString(), arm.ProbeOrigin, target, hit?.Point);
        }
    }

    private static void ResetSpringArmCameras(Context context)
    {
        foreach (var instance in context.Instances)
        {
            var owner = instance.Blueprint;
            if (!owner.Enabled || owner.PendingDestroy || instance.Failed) continue;
            foreach (var arm in owner.Components.OfType<SpringArmComponent>().OrderBy(ComponentDepth))
            {
                PrepareSpringArm(arm, 0, true);
                UpdateSpringArmTarget(context, arm);
            }
        }
    }

    private static void QueueSpringArmProbes(Context context)
    {
        foreach (var instance in context.Instances)
            foreach (var arm in instance.Blueprint.Components.OfType<SpringArmComponent>())
                arm.NeedsEvaluation = !instance.Failed && instance.Blueprint.Enabled && !instance.Blueprint.PendingDestroy;
        QueueReadySpringArms(context);
    }

    private static void QueueReadySpringArms(Context context)
    {
        foreach (var instance in context.Instances)
        {
            var owner = instance.Blueprint;
            if (!owner.Enabled || owner.PendingDestroy || instance.Failed) continue;
            foreach (var arm in owner.Components.OfType<SpringArmComponent>().OrderBy(ComponentDepth))
            {
                if (!arm.NeedsEvaluation) continue;
                bool waiting = false;
                // 嵌套臂必须等祖先本帧碰撞落点确定；声明顺序不决定空间更新顺序。
                for (var parent = arm.Parent; parent != null; parent = parent.Parent)
                    if (parent is SpringArmComponent ancestor && (ancestor.NeedsEvaluation || ancestor.PendingProbe))
                        waiting = true;
                if (waiting) continue;
                arm.NeedsEvaluation = false;
                PrepareSpringArm(arm, context.Time.DeltaTime, false);
                if (!arm.DoCollisionTest || arm.TargetArmLength == 0 ||
                    Vector3.DistanceSquared(arm.ProbeOrigin, arm.UnfixedCameraPosition) <= 1e-6f)
                {
                    UpdateSpringArmTarget(context, arm);
                    continue;
                }
                // 障碍物可以移动；即使臂静止也必须查询，不能只按相机端点缓存命中。
                arm.PendingProbe = true;
                instance.Dirty = true;
                context.Services.QueueSpringArmRay(owner, arm.ProbeOrigin, arm.UnfixedCameraPosition,
                    arm.ProbeRadius, arm.CollisionMask, hit => UpdateSpringArmTarget(context, arm, hit));
            }
        }
    }

    // 射线因预算/失效被丢弃时，不能把相机留在上一帧的缩短位置。
    private static void FlushPendingSpringArms(Context context)
    {
        foreach (var instance in context.Instances)
        {
            var owner = instance.Blueprint;
            if (!owner.Enabled || owner.PendingDestroy || instance.Failed) continue;
            foreach (var arm in owner.Components.OfType<SpringArmComponent>())
                if (arm.PendingProbe) UpdateSpringArmTarget(context, arm);
        }
        QueueReadySpringArms(context);
    }
}
