namespace VulkanLearn;

// 时间值由原生 GT 调度发布；TimeScale 只能通过受控原生命令修改，不允许某个回调改变其它实例的时间。
// 每帧只有一个变长 dt：Tick/LateTick 共用同一个帧时钟，不再存在固定步时间字段。
public sealed class Time
{
    public float DeltaTime { get; internal set; }
    public float UnscaledDeltaTime { get; internal set; }
    public float TimeScale { get; internal set; } = 1;
    public double TimeSinceStartup { get; internal set; }
    public double TimeSincePlay { get; internal set; }
    public double UnscaledTimeSincePlay { get; internal set; }
    public ulong FrameIndex { get; internal set; }
}
