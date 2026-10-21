using System.Numerics;

namespace VulkanLearn;

public readonly record struct TimerHandle(ulong Context, ulong Id);
public readonly record struct SubscriptionHandle(ulong Context, ulong Id);
public readonly record struct GameEvent(BlueprintHandle Sender, string Name, string Payload);
public readonly record struct RaycastHit(BlueprintHandle Blueprint, Vector3 Point, Vector3 Normal, float Distance);

// 所有服务属于一个 Blueprint/Play 上下文；只有 live GT 回调可操作，失败回调撤销服务注册和消息。
public sealed class BlueprintTimers
{
    internal Func<double, Action, bool, TimerHandle> Schedule = null!;
    internal Action<TimerHandle> CancelRequest = null!;
    public TimerHandle Set(double seconds, Action callback, bool repeat = false) => Schedule(seconds, callback, repeat);
    public void Cancel(TimerHandle timer) => CancelRequest(timer);
}
public sealed class BlueprintEvents
{
    internal Func<string, Action<GameEvent>, SubscriptionHandle> SubscribeRequest = null!;
    internal Action<SubscriptionHandle> UnsubscribeRequest = null!;
    internal Action<BlueprintHandle?, string, string> PublishRequest = null!;
    public SubscriptionHandle Subscribe(string name, Action<GameEvent> callback) => SubscribeRequest(name, callback);
    public void Unsubscribe(SubscriptionHandle subscription) => UnsubscribeRequest(subscription);
    public void Publish(string name, string payload = "") => PublishRequest(null, name, payload);
    public void Send(BlueprintHandle target, string name, string payload = "") => PublishRequest(target, name, payload);
}
public sealed class BlueprintPhysics
{
    internal Action<Vector3, Vector3, Action<RaycastHit?>, bool, uint, float, bool> RaycastRequest = null!;
    // 有界线段查询；默认忽略自己，在物理边界后回调，LateTick 内提交的查询在下一帧交付。
    public void Raycast(Vector3 from, Vector3 to, Action<RaycastHit?> callback, bool includeTriggers = false,
        uint collisionMask = 65535) => RaycastRequest(from, to, callback, includeTriggers, collisionMask, 0, false);
    public void SphereCast(Vector3 from, Vector3 to, float radius, Action<RaycastHit?> callback,
        bool includeTriggers = false, uint collisionMask = 65535) =>
        RaycastRequest(from, to, callback, includeTriggers, collisionMask, radius, false);
    // 端点使用 Blueprint 局部坐标，在物理步后的根 TRS 中转换；半径、命中点与距离仍是世界厘米。
    public void SphereCastLocal(Vector3 from, Vector3 to, float worldRadius, Action<RaycastHit?> callback,
        bool includeTriggers = false, uint collisionMask = 65535) =>
        RaycastRequest(from, to, callback, includeTriggers, collisionMask, worldRadius, true);
}
public sealed class CharacterController
{
    internal Vector3 WalkVelocity;
    internal Vector3 RootMotionVelocity;
    internal bool RootMotionActive;
    internal bool JumpRequested;
    public bool IsGrounded { get; internal set; }
    public Vector3 LinearVelocity { get; internal set; }
    public void Move(Vector3 velocity) => WalkVelocity = velocity;
    // 根运动覆盖本帧速度；Jolt 负责碰撞/重力，下一次消费前不会与 Move 叠加。
    public void ConsumeRootMotion(Vector3 velocity) { RootMotionVelocity = velocity; RootMotionActive = true; }
    public void ClearRootMotion() { RootMotionVelocity = Vector3.Zero; RootMotionActive = false; }
    public void Jump() => JumpRequested = true;
}
