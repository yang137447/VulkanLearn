using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

public sealed class TimeProbeBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool Services { get; set; }
    private int ticks, timerFires;
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
        if (dt != Time.DeltaTime) throw new Exception("Invalid variable tick time.");
        ticks++;
        tickFrame = Time.FrameIndex;
    }
    protected override void LateTick(float dt)
    {
        // LateTick 与 Tick 共用同一个帧时钟：dt 不变，FrameIndex 也不得在本帧内推进。
        if (dt != Time.DeltaTime || tickFrame != Time.FrameIndex)
            throw new Exception("Invalid late tick order/time.");
        Transform.Position = new((float)Time.TimeSincePlay, (float)Time.UnscaledTimeSincePlay,
            (float)Time.TimeSinceStartup);
        Transform.Rotation = new(ticks, Time.TimeScale, Time.UnscaledDeltaTime);
        Transform.Scale = new(1 + timerFires, 1 + ticks, 1);
    }
}
