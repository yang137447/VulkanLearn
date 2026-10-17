namespace VulkanLearn;

// 时间值由原生 GT 调度发布；TimeScale 只能通过受控原生命令修改，不允许某个回调改变其它实例的时间。
public sealed class Time
{
    public float DeltaTime { get; internal set; }
    public float UnscaledDeltaTime { get; internal set; }
    public float FixedDeltaTime => 1f / 60;
    public float TimeScale { get; internal set; } = 1;
    public double TimeSinceStartup { get; internal set; }
    public double TimeSincePlay { get; internal set; }
    public double UnscaledTimeSincePlay { get; internal set; }
    public ulong FrameIndex { get; internal set; }
    public ulong FixedFrameIndex { get; internal set; }
    public bool IsFixedTick { get; internal set; }
}

public abstract partial class Blueprint
{
    protected virtual void FixedTick(float deltaTime) { }
    internal void UpdateFixed(float dt) => FixedTick(dt);
}
