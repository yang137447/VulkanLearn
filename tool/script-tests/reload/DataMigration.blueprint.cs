using System.Numerics;
using VulkanLearn;

namespace ReloadFixtures;

public class MigratingData
{
    public int Counter { get; set; } = 20;
    public List<Vector3> Points { get; set; } = [new(1, 2, 3)];
    public Dictionary<string, int> Counts { get; set; } = new() { ["initial"] = 2 };
#if RELOAD_V2
    public int Added { get; set; } = 77;
#endif
}

public sealed class DataMigrationBlueprint : Blueprint
{
    [Migrate] public MigratingData Data { get; set; } = new();
    protected override void Tick(float dt)
    {
        ++Data.Counter;
        Data.Points[0] += Vector3.One;
        Data.Counts["initial"] += 3;
    }
}

public class ChangedNestedData
{
#if RELOAD_V2
    public double Number { get; set; } = 2;
#else
    public float Number { get; set; } = 1;
#endif
}

public sealed class ChangedNestedMigrationBlueprint : Blueprint
{
    [Migrate] public List<ChangedNestedData> Data { get; set; } = [new()];
}
