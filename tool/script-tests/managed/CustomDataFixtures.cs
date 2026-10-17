using System.Numerics;
using System.Text.Json.Serialization;
using VulkanLearn;

namespace ScriptFixtures;

public enum DataDamageKind { Normal, Fire }

public struct DataStats
{
    public int Damage { get; set; }
    public Vector3 Offset;
    public DataStats() { Damage = 12; Offset = new(1, 2, 3); }
}

public class DataSettings
{
    public DataStats Stats { get; set; } = new();
    public List<Vector3> Points { get; set; } = [new(4, 5, 6)];
    public Dictionary<string, DataStats> Slots { get; set; } = new()
        { ["base"] = new(), ["removed"] = new() { Damage = 7 } };
    public float[] Weights = [1, 2];
    public Quaternion Orientation { get; set; } = Quaternion.Identity;
    public DataDamageKind Kind { get; set; } = DataDamageKind.Normal;
    public AssetId Asset { get; set; } = new("Common/Data/test.json");
    [JsonIgnore] public object Ignored { get; set; } = new();
    [NonSerialized] public object IgnoredField = new();
}

public sealed class CustomDataBlueprint : Blueprint
{
    private static readonly DataSettings Shared = new();
    [Expose(InstanceEditable = true), Migrate] public DataSettings Settings { get; set; } = Shared;
    [Expose(InstanceEditable = true)] public List<List<int>> Grid { get; set; } = [];
    [Expose(InstanceEditable = true)] public List<DataStats> Entries { get; set; } = [];
    [Expose(InstanceEditable = true)] public DataSettings Empty { get; set; } = null!;
    [Expose(InstanceEditable = true)] public List<int> EmptyList { get; set; } = null!;
    [Expose(InstanceEditable = true)] public Dictionary<string, int> EmptyDictionary { get; set; } = null!;
    protected override void BeginPlay()
    {
        Transform.Position = new(Settings.Stats.Damage, Settings.Points[0].X, Settings.Slots["base"].Damage);
        if (Name == "mutate")
        {
            Settings.Points[0] = new(99, 99, 99);
            Settings.Slots["base"] = new() { Damage = 99 };
        }
    }
}

public sealed class CustomDataComponent : Component
{
    private static readonly DataSettings Shared = new();
    [Expose] public DataSettings Settings { get; set; } = Shared;
    [Expose] public float Observed { get; set; }
    protected override void BeginPlay()
    {
        Observed = Settings.Points[0].X;
        if (Id == "first") Settings.Points[0] = new(99, 99, 99);
    }
}

public sealed class DynamicDataBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        AddScriptComponent<CustomDataComponent>("first");
        AddScriptComponent<CustomDataComponent>("second");
        var configuration = new DataSettings
        {
            Stats = new() { Damage = 32, Offset = new(9, 8, 7) },
            Points = [new(13, 14, 15)]
        };
        AddScriptComponent(new()
        {
            Id = "configured", Class = typeof(CustomDataComponent).FullName!,
            Properties = new Dictionary<string, object> { ["Settings"] = configuration }
        });
        configuration.Points[0] = new(99, 99, 99);
    }
}

public sealed class RuntimeInvalidDataComponent : Component
{
    [Expose(InstanceEditable = true)] public int InvalidMode { get; set; }
    [Expose, Migrate] public DataSettings Settings { get; set; } = new();
    protected override void Tick(float dt)
    {
        if (InvalidMode == 0) Settings = null!;
        else if (InvalidMode == 1)
        {
            var cycle = new DerivedDataSettings();
            cycle.Cycle = cycle;
            Settings = cycle;
        }
        else Settings.Weights = [float.NaN];
    }
}

public sealed class RuntimeInvalidDataBlueprint : Blueprint
{
    protected override void Tick(float dt) => Transform.Translate(new(dt, 0, 0));
}

public sealed class RecursiveData
{
    public RecursiveData Child { get; set; } = null!;
}

public sealed class UnsupportedMigrationBlueprint : Blueprint
{
    [Migrate] public RecursiveData Data { get; set; } = new();
}

public sealed class NonStringDictionaryMigrationBlueprint : Blueprint
{
    [Migrate] public Dictionary<int, string> Data { get; set; } = [];
}

public sealed class DelegateMigrationBlueprint : Blueprint
{
    [Migrate] public Action Data { get; set; } = () => { };
}

public sealed class EngineObjectMigrationBlueprint : Blueprint
{
    [Migrate] public Transform Data { get; set; } = new();
}

public sealed class MultidimensionalMigrationBlueprint : Blueprint
{
    [Migrate] public float[,] Data { get; set; } = new float[1, 1];
}

public sealed class DeepMigrationBlueprint : Blueprint
{
    [Migrate]
    public List<List<List<List<List<List<List<List<List<List<List<List<List<List<List<List<List<int>>>>>>>>>>>>>>>>> Data { get; set; } = [];
}

public sealed class DerivedDataSettings : DataSettings
{
    public object Cycle { get; set; } = null!;
}

public sealed class PolymorphicMigrationBlueprint : Blueprint
{
    [Migrate] public DataSettings Data { get; set; } = new DerivedDataSettings();
}
