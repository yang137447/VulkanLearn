using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

public enum TravelMode { Walk, Fly }

public sealed class TypedPropertiesBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public Vector2 Planar { get; set; } = new(1, 2);
    [Expose(InstanceEditable = true)] public Vector3 Offset { get; set; } = new(3, 4, 5);
    [Expose(InstanceEditable = true)] public Vector4 Weights { get; set; } = new(6, 7, 8, 9);
    [Expose(InstanceEditable = true)] public TravelMode Mode { get; set; } = TravelMode.Walk;
    [Expose(InstanceEditable = true)] public AssetId Asset { get; set; } = new("Common/Blueprints/BP_child.json");
    [Expose(InstanceEditable = true)] public BlueprintReference Target { get; set; }
    [Expose] public Vector3 PrivateOffset { get; set; } = Vector3.One;
    protected override void BeginPlay()
    {
        Transform.Position = Offset + new Vector3(Planar, Weights.W);
        if (Mode == TravelMode.Fly) Transform.Position += Vector3.UnitY * 100;
        if (Target.Resolve(World) is { } target)
            Transform.Position += World.GetBlueprint(target).Position;
        if (Asset.Path != "Common/Blueprints/BP_child.json") throw new InvalidOperationException("Asset binding failed.");
    }
}

public sealed class LifecycleBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool FailCreate { get; set; }
    [Expose(InstanceEditable = true)] public bool FailTick { get; set; }
    [Expose(InstanceEditable = true)] public bool SelfDestroy { get; set; }
    protected override void OnCreate()
    {
        if (FailCreate) throw new InvalidOperationException("create failure");
        Transform.Position = new(1, 0, 0);
    }
    protected override void BeginPlay() => Transform.Translate(new(10, 0, 0));
    protected override void Tick(float deltaTime)
    {
        Transform.Translate(new(100, 0, 0));
        if (FailTick) throw new InvalidOperationException("tick failure");
        if (SelfDestroy) Destroy();
    }
    protected override void LateTick(float deltaTime) => Transform.Translate(new(1000, 0, 0));
    protected override void EndPlay(EndPlayReason reason) => Log.Info($"end:{Handle.Id}:{reason}");
}
