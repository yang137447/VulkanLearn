using System.Numerics;
using VulkanLearn;

namespace ReloadFixtures;

public sealed class PassiveMigrationBlueprint : Blueprint { }
public sealed class PassiveMigrationComponent : Component
{
#if RELOAD_V2
    [Expose] public int DefaultValue { get; set; } = 200;
#else
    [Expose] public int DefaultValue { get; set; } = 100;
#endif
}

public sealed class MigrationBlueprint : Blueprint
{
    [Migrate] public int Counter { get; set; }
#if RELOAD_V2
    public int Unmarked { get; set; } = 200;
#else
    public int Unmarked { get; set; } = 100;
#endif
    [Expose(InstanceEditable = true)] public bool RejectReload { get; set; }
    [Expose(InstanceEditable = true)] public bool MoveOnReload { get; set; }
    [Expose(InstanceEditable = true)] public bool SpawnChild { get; set; }
    [Expose(InstanceEditable = true)] public bool ChangeStructure { get; set; }
    protected override void BeginPlay()
    {
        Counter = 40;
        BindServices();
        if (SpawnChild) World.Spawn("Common/Blueprints/BP_migration.json", new(5, 500, 7));
    }
    private void BindServices()
    {
        Timers.Set(0, () => Counter += 10);
        Events.Subscribe("reload", _ => Counter += 100);
    }
    protected override void OnReload()
    {
        if (RejectReload) throw new InvalidOperationException("migration-hook-rejected");
        if (MoveOnReload) Transform.Position += Vector3.UnitY;
        if (ChangeStructure) AddScriptComponent<MigrationComponent>("forbidden");
        BindServices();
    }
    protected override void Tick(float dt)
    {
        if (Body == null) Transform.Position = new(Counter, ++Unmarked,
#if RELOAD_V2
            2
#else
            1
#endif
        );
    }
}

public sealed class MigrationComponent : Component
{
    [Migrate, Expose] public int Counter { get; set; }
#if RELOAD_V2
    [Expose] public int Unmarked { get; set; } = 200;
#else
    [Expose] public int Unmarked { get; set; } = 100;
#endif
    protected override void BeginPlay() { Counter = 50; Unmarked = 150; }
    protected override void Tick(float dt) { ++Counter; ++Unmarked; }
    protected override void OnReload() => Timers.Set(0, () => Counter += 10);
}

public sealed class ChangedMigrationBlueprint : Blueprint
{
#if RELOAD_V2
    [Migrate] public string Value { get; set; } = "new";
#else
    [Migrate] public int Value { get; set; } = 77;
#endif
}
