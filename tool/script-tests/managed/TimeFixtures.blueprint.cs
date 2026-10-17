using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

public sealed class TimeProbeBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool Services { get; set; }
    [Expose(InstanceEditable = true)] public bool FailFixed { get; set; }
    private int ticks, fixedTicks, timerFires;
    private ulong tickFrame;
    protected override void BeginPlay()
    {
        if (Services)
        {
            Timers.Set(0.1, () => timerFires++, repeat: true);
            Events.Subscribe("probe", _ => { });
        }
    }
    protected override void Tick(float dt)
    {
        if (Time.IsFixedTick || dt != Time.DeltaTime) throw new Exception("Invalid variable tick time.");
        ticks++;
        tickFrame = Time.FrameIndex;
    }
    protected override void FixedTick(float dt)
    {
        if (!Time.IsFixedTick || dt != Time.FixedDeltaTime || dt != Time.DeltaTime || tickFrame != Time.FrameIndex)
            throw new Exception("Invalid fixed tick order/time.");
        fixedTicks++;
        if (FailFixed)
        {
            Transform.Position = new(999, 999, 999);
            Timers.Set(0, () => { });
            Events.Subscribe("failed", _ => { });
            throw new InvalidOperationException("fixed-probe-exception");
        }
    }
    protected override void LateTick(float dt)
    {
        if (Time.IsFixedTick || dt != Time.DeltaTime || tickFrame != Time.FrameIndex)
            throw new Exception("Invalid late tick order/time.");
        Transform.Position = new((float)Time.TimeSincePlay, (float)Time.UnscaledTimeSincePlay,
            (float)Time.TimeSinceStartup);
        Transform.Rotation = new(fixedTicks, Time.TimeScale, Time.UnscaledDeltaTime);
        Transform.Scale = new(1 + timerFires, 1 + ticks, 1);
    }
}

public sealed class FixedPhysicsProbeBlueprint : Blueprint
{
    private int steps;
    protected override void FixedTick(float dt)
    {
        // 每次回调必须读到上一次真正物理子步的结果，批量回调后再求解会在第二步失败。
        if (MathF.Abs(Transform.Position.X - steps) > 0.01f)
            throw new InvalidOperationException("FixedTick/Jolt substeps are not paired.");
        steps++;
        Body!.LinearVelocity = new(60, 0, 0);
    }
}
