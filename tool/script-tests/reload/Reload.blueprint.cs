using System.Numerics;
using VulkanLearn;

namespace ReloadFixtures;

public sealed class ReloadBlueprint : Blueprint
{
#if RELOAD_V2
    [Expose(InstanceEditable = true)] public float Speed { get; set; } = 20;
    [Expose(InstanceEditable = true)] public string SchemaValue { get; set; } = "new";
    [Expose] public bool Restricted { get; set; }
#else
    [Expose(InstanceEditable = true)] public float Speed { get; set; } = 10;
    [Expose(InstanceEditable = true)] public float SchemaValue { get; set; } = 1;
    [Expose(InstanceEditable = true)] public bool Restricted { get; set; }
#endif
    [Expose(InstanceEditable = true)] public bool RejectV2 { get; set; }
    private static int starts;
    protected override void OnCreate()
    {
#if RELOAD_V2
        if (RejectV2) throw new InvalidOperationException("V2 rejected by author fixture");
#endif
    }
    protected override void BeginPlay()
    {
        Transform.Position = new(Speed, ++starts, 0);
        Timers.Set(0, () => Transform.Position += Vector3.UnitZ);
        Events.Subscribe("reload", _ => Transform.Position += Vector3.UnitZ);
    }
}
