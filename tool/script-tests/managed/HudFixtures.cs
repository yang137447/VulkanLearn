using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

public sealed class HudProbeBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public int Mode { get; set; }
    protected override void BeginPlay() => Hud.SetText("status", "owner");
    protected override void Tick(float dt)
    {
        switch (Mode)
        {
            case 1: Hud.SetText("status", "updated"); break;
            case 2: Hud.Remove("status"); break;
            case 3: Destroy(); break;
            case 4: GetScriptComponent("first").Enabled = false; break;
            case 5: RemoveScriptComponent(GetScriptComponent("first").Handle); break;
            case 6:
                Hud.SetText("status", "fault");
                throw new InvalidOperationException("hud-owner-failure");
        }
    }
}

public sealed class HudProbeComponent : Component
{
    [Expose(InstanceEditable = true)] public bool FailTick { get; set; }
    [Expose(InstanceEditable = true)] public int Lines { get; set; } = 1;
    protected override void BeginPlay()
    {
        for (int index = 0; index < Lines; ++index) Hud.SetText(index == 0 ? "status" : $"line-{index}", Id);
    }
    protected override void Tick(float dt)
    {
        if (!FailTick) return;
        Hud.Remove("status");
        Hud.SetText("failed", "discard");
        throw new InvalidOperationException("hud-component-failure");
    }
}

public sealed class HudInvalidBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public string Key { get; set; } = "status";
    [Expose(InstanceEditable = true)] public string Text { get; set; } = "value";
    [Expose(InstanceEditable = true)] public int Lines { get; set; } = 1;
    protected override void BeginPlay()
    {
        for (int index = 0; index < Lines; ++index) Hud.SetText(index == 0 ? Key : $"line-{index}", Text);
    }
}

public sealed class HudCreateAccessBlueprint : Blueprint
{
    protected override void OnCreate() => Hud.SetText("invalid", "create");
}

public sealed class HudThreadAccessBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        bool rejected = Task.Run(() =>
        {
            try { Hud.SetText("worker", "invalid"); return false; }
            catch (InvalidOperationException) { return true; }
        }).GetAwaiter().GetResult();
        if (rejected) Transform.Position = Vector3.UnitX;
    }
}

public sealed class HudForeignAccessBlueprint : Blueprint
{
    private static GameplayHud? saved;
    protected override void BeginPlay()
    {
        if (Handle.Id == "first") saved = Hud;
        else
        {
            try { saved!.SetText("foreign", "invalid"); }
            catch (InvalidOperationException) { Transform.Position = Vector3.UnitX; }
            finally { saved = null; }
        }
    }
}

public sealed class HudStaleAccessBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool UseSaved { get; set; }
    private static GameplayHud? saved;
    protected override void BeginPlay()
    {
        if (!UseSaved) saved = Hud;
        else
        {
            try { saved!.SetText("stale", "invalid"); }
            catch (InvalidOperationException) { Transform.Position = Vector3.UnitX; }
            finally { saved = null; }
        }
    }
}
