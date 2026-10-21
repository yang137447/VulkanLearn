using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

public sealed class DeltaMovementBlueprint : Blueprint
{
    protected override void Tick(float dt) => Transform.Translate(new(dt, 0, 0));
}

public sealed class DeltaResetBlueprint : Blueprint
{
    protected override void Tick(float dt) => Transform.Position = Vector3.Zero;
}

public sealed class DeltaImpulseBlueprint : Blueprint
{
    protected override void Tick(float dt) => Body!.AddImpulse(Vector3.UnitX);
}

public sealed class DeltaDisabledAssetBlueprint : Blueprint
{
    protected override void BeginPlay() => Assets.LoadBlueprintAsync("Common/Blueprints/BP_dynamic.json",
        _ => throw new InvalidOperationException("Disabled owner received an asset callback."));
    protected override void Tick(float dt) => Enabled = false;
}
