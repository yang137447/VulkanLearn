using System.Numerics;
using System.Text.Json;

namespace VulkanLearn;

// 粒子句柄只在一次 Play 上下文内有效；Id 是原生场景对象名，Context 防止跨 Play 复用。
public readonly record struct ParticleHandle(ulong Context, string Id);

// 发射器身份使用稳定槽位身份 + 显示名。native 的 stableIndex 只增不减，
// destroy 后 slot 会保留为 tombstone，因此同一 id 不会指向新发射器。
public readonly record struct ParticleEmitterHandle(
    ulong Context, string SystemId, string Id, uint StableIndex);

public enum ParticleEventKind
{
    Spawn,
    Death,
    Collision
}

// 事件由 native 在 GT 边界写入下一帧 tick 请求；C# 只在回调线程分发，
// 不会把托管委托交给渲染线程。
public readonly record struct ParticleEvent(
    ulong Context,
    string SystemId,
    string EmitterId,
    ParticleEventKind Kind,
    float Time,
    Vector3 Position,
    Vector3 Normal);

public readonly record struct ParticleEmitterTelemetry(
    uint StableIndex,
    string Name,
    bool Active,
    bool Enabled,
    uint RestartEpoch,
    uint MaxParticles,
    uint BudgetedMaxParticles,
    uint AliveParticles,
    bool AliveAvailable);

// AliveAvailable=false 表示渲染器尚未回填，不能把 AliveParticles 的 0 当作无粒子。
public readonly record struct ParticleTelemetry(
    uint EmitterCount,
    uint ActiveEmitterCount,
    uint EnabledEmitterCount,
    uint VisibleEmitterCount,
    uint AuthoredCapacity,
    uint BudgetedCapacity,
    bool AliveAvailable,
    uint AliveParticles,
    uint LastFrameBudget,
    ParticleEmitterTelemetry[] Emitters);

public sealed class ParticleEventSubscription : IDisposable
{
    private Action? cancel;
    internal ParticleEventSubscription(Action cancel) => this.cancel = cancel;
    public void Dispose() => Interlocked.Exchange(ref cancel, null)?.Invoke();
}

public enum ParticleDataChannelType
{
    Float,
    Vec2,
    Vec3,
    Vec4,
    Int,
    Bool,
    Quat
}

// 托管侧保留类型信息；Components 的未使用分量保持 0。
public readonly record struct ParticleDataChannelValue(
    ParticleDataChannelType Type, Vector4 Components)
{
    public bool IsScalar =>
        Type is ParticleDataChannelType.Float or
            ParticleDataChannelType.Int or ParticleDataChannelType.Bool;

    public float Scalar => Components.X;

    public static ParticleDataChannelValue Float(float value) =>
        new(ParticleDataChannelType.Float, new Vector4(value, 0, 0, 0));
    public static ParticleDataChannelValue Int(int value) =>
        new(ParticleDataChannelType.Int, new Vector4(value, 0, 0, 0));
    public static ParticleDataChannelValue Bool(bool value) =>
        new(ParticleDataChannelType.Bool, new Vector4(value ? 1 : 0, 0, 0, 0));
    public static ParticleDataChannelValue Vec2(Vector2 value) =>
        new(ParticleDataChannelType.Vec2, new Vector4(value.X, value.Y, 0, 0));
    public static ParticleDataChannelValue Vec3(Vector3 value) =>
        new(ParticleDataChannelType.Vec3, new Vector4(value.X, value.Y, value.Z, 0));
    public static ParticleDataChannelValue Vec4(Vector4 value) =>
        new(ParticleDataChannelType.Vec4, value);
    public static ParticleDataChannelValue Quat(Quaternion value) =>
        new(ParticleDataChannelType.Quat,
            new Vector4(value.X, value.Y, value.Z, value.W));

    public Vector2 ToVector2() => new(Components.X, Components.Y);
    public Vector3 ToVector3() => new(Components.X, Components.Y, Components.Z);
    public Vector4 ToVector4() => Components;
    public Quaternion ToQuaternion() =>
        new(Components.X, Components.Y, Components.Z, Components.W);

    internal object[] ToCommandValues()
    {
        return Type switch
        {
            ParticleDataChannelType.Int =>
                [(int)Components.X],
            ParticleDataChannelType.Bool =>
                [Components.X != 0],
            ParticleDataChannelType.Vec2 =>
                [Components.X, Components.Y],
            ParticleDataChannelType.Vec3 =>
                [Components.X, Components.Y, Components.Z],
            ParticleDataChannelType.Vec4 or ParticleDataChannelType.Quat =>
                [Components.X, Components.Y, Components.Z, Components.W],
            _ => [Components.X]
        };
    }

    internal object ToPayload() => new Dictionary<string, object?>
    {
        ["type"] = TypeName,
        ["values"] = ToCommandValues()
    };

    internal string TypeName => Type switch
    {
        ParticleDataChannelType.Float => "float",
        ParticleDataChannelType.Vec2 => "vec2",
        ParticleDataChannelType.Vec3 => "vec3",
        ParticleDataChannelType.Vec4 => "vec4",
        ParticleDataChannelType.Int => "int",
        ParticleDataChannelType.Bool => "bool",
        ParticleDataChannelType.Quat => "quat",
        _ => "float"
    };

    internal bool Validate(string name)
    {
        var components = ToCommandValues();
        foreach (var component in components)
        {
            float value = component switch
            {
                float number => number,
                int integer => integer,
                bool flag => flag ? 1 : 0,
                _ => throw new ArgumentOutOfRangeException(name,
                    "Particle data channel contains a non-numeric component.")
            };
            if (!float.IsFinite(value) || Math.Abs(value) > 1.0e6f)
                throw new ArgumentOutOfRangeException(name,
                    "Particle data channel components must be finite and in [-1e6, 1e6].");
        }
        if (Type == ParticleDataChannelType.Int &&
            Math.Truncate(Components.X) != Components.X)
            throw new ArgumentOutOfRangeException(name,
                "Particle data channel int value must be integral.");
        return true;
    }

    internal static bool TryParse(JsonElement element,
        out ParticleDataChannelValue value)
    {
        value = default;
        switch (element.ValueKind)
        {
        case JsonValueKind.Number:
            return TryScalar(ParticleDataChannelType.Float, element, out value);
        case JsonValueKind.True:
        case JsonValueKind.False:
            value = Bool(element.GetBoolean());
            return true;
        case JsonValueKind.Array:
            return TryArray(element, out value);
        case JsonValueKind.Object:
            return TryObject(element, out value);
        default:
            return false;
        }
    }

    private static bool TryObject(JsonElement element,
        out ParticleDataChannelValue value)
    {
        value = default;
        if (element.TryGetProperty("type", out var typeElement) &&
            typeElement.ValueKind == JsonValueKind.String)
        {
            if (!TryType(typeElement.GetString()!, out var type)) return false;
            if (element.TryGetProperty("values", out var values))
                return TryTyped(type, values, out value);
            if (element.TryGetProperty("value", out var scalar))
                return TryTyped(type, scalar, out value);
            return false;
        }
        foreach (var (name, type) in new[]
        {
            ("float", ParticleDataChannelType.Float),
            ("vec2", ParticleDataChannelType.Vec2),
            ("vec3", ParticleDataChannelType.Vec3),
            ("vec4", ParticleDataChannelType.Vec4),
            ("int", ParticleDataChannelType.Int),
            ("bool", ParticleDataChannelType.Bool),
            ("quat", ParticleDataChannelType.Quat)
        })
        {
            if (element.TryGetProperty(name, out var typed))
                return TryTyped(type, typed, out value);
        }
        if (element.TryGetProperty("values", out var valuesElement))
            return TryParse(valuesElement, out value);
        if (element.TryGetProperty("value", out var valueElement))
            return TryParse(valueElement, out value);
        return false;
    }

    private static bool TryArray(JsonElement element,
        out ParticleDataChannelValue value)
    {
        value = default;
        var type = element.GetArrayLength() switch
        {
            2 => ParticleDataChannelType.Vec2,
            3 => ParticleDataChannelType.Vec3,
            4 => ParticleDataChannelType.Vec4,
            _ => (ParticleDataChannelType?)null
        };
        return type is { } inferred && TryTyped(inferred, element, out value);
    }

    private static bool TryTyped(ParticleDataChannelType type,
        JsonElement element, out ParticleDataChannelValue value)
    {
        value = default;
        var required = type switch
        {
            ParticleDataChannelType.Vec2 => 2,
            ParticleDataChannelType.Vec3 => 3,
            ParticleDataChannelType.Vec4 or ParticleDataChannelType.Quat => 4,
            _ => 1
        };
        var components = new float[required];
        if (required == 1 && element.ValueKind != JsonValueKind.Array)
        {
            if (!TryComponent(type, element, out components[0])) return false;
        }
        else
        {
            if (element.ValueKind != JsonValueKind.Array ||
                element.GetArrayLength() != required) return false;
            var index = 0;
            foreach (var component in element.EnumerateArray())
                if (!TryComponent(type, component, out components[index++]))
                    return false;
        }
        value = new ParticleDataChannelValue(type,
            new Vector4(components.ElementAtOrDefault(0),
                components.ElementAtOrDefault(1),
                components.ElementAtOrDefault(2),
                components.ElementAtOrDefault(3)));
        return true;
    }

    private static bool TryComponent(ParticleDataChannelType type,
        JsonElement element, out float value)
    {
        value = 0;
        if (type == ParticleDataChannelType.Bool &&
            element.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            value = element.GetBoolean() ? 1 : 0;
            return true;
        }
        if (element.ValueKind != JsonValueKind.Number) return false;
        if (!element.TryGetSingle(out value)) return false;
        if (!float.IsFinite(value) || Math.Abs(value) > 1.0e6f) return false;
        if (type == ParticleDataChannelType.Int &&
            Math.Truncate(value) != value) return false;
        if (type == ParticleDataChannelType.Bool &&
            value != 0 && value != 1) return false;
        return true;
    }

    private static bool TryScalar(ParticleDataChannelType type,
        JsonElement element, out ParticleDataChannelValue value)
    {
        value = default;
        if (!TryComponent(type, element, out var component)) return false;
        value = new ParticleDataChannelValue(type, new Vector4(component, 0, 0, 0));
        return true;
    }

    private static bool TryType(string name, out ParticleDataChannelType type)
    {
        type = name switch
        {
            "float" => ParticleDataChannelType.Float,
            "vec2" => ParticleDataChannelType.Vec2,
            "vec3" => ParticleDataChannelType.Vec3,
            "vec4" => ParticleDataChannelType.Vec4,
            "int" => ParticleDataChannelType.Int,
            "bool" => ParticleDataChannelType.Bool,
            "quat" or "quaternion" => ParticleDataChannelType.Quat,
            _ => (ParticleDataChannelType)(-1)
        };
        return (int)type >= 0;
    }
}

// World 级命名类型化通道。它不是某个粒子实例的 userParameters：同一 Play 内
// 所有 Blueprint 读取同一份值；Set/Clear 通过 owner 校验后的 particleCommands
// 写回 active World，Get 读取最近一次 native 快照并在本帧命令上即时可见。
public sealed class ParticleDataChannels
{
    internal Action<string, ParticleDataChannelValue?> ChangeRequest = null!;
    internal Func<string, ParticleDataChannelValue?> ValueQuery = null!;

    public void Set(string name, float value) =>
        ChangeRequest(name, ParticleDataChannelValue.Float(value));
    public void Set(string name, ParticleDataChannelValue value) =>
        ChangeRequest(name, value);
    public void Float(string name, float value) =>
        ChangeRequest(name, ParticleDataChannelValue.Float(value));
    public void Int(string name, int value) =>
        ChangeRequest(name, ParticleDataChannelValue.Int(value));
    public void Bool(string name, bool value) =>
        ChangeRequest(name, ParticleDataChannelValue.Bool(value));
    public void Vector2(string name, System.Numerics.Vector2 value) =>
        ChangeRequest(name, ParticleDataChannelValue.Vec2(value));
    public void Vector3(string name, System.Numerics.Vector3 value) =>
        ChangeRequest(name, ParticleDataChannelValue.Vec3(value));
    public void Vector4(string name, System.Numerics.Vector4 value) =>
        ChangeRequest(name, ParticleDataChannelValue.Vec4(value));
    public void Quaternion(string name, System.Numerics.Quaternion value) =>
        ChangeRequest(name, ParticleDataChannelValue.Quat(value));
    public void Clear(string name) => ChangeRequest(name, null);

    // 旧 API：只返回标量通道，向量/四元数请使用对应的 Get* 方法。
    public float? Get(string name) =>
        ValueQuery(name) is { IsScalar: true } value ? value.Scalar : null;
    public ParticleDataChannelValue? GetValue(string name) => ValueQuery(name);
    public float? GetFloat(string name) =>
        ValueQuery(name) is { Type: ParticleDataChannelType.Float } value
            ? value.Scalar : null;
    public int? GetInt(string name) =>
        ValueQuery(name) is { Type: ParticleDataChannelType.Int } value
            ? (int)value.Scalar : null;
    public bool? GetBool(string name) =>
        ValueQuery(name) is { Type: ParticleDataChannelType.Bool } value
            ? value.Scalar != 0 : null;
    public System.Numerics.Vector2? GetVector2(string name) =>
        ValueQuery(name) is { Type: ParticleDataChannelType.Vec2 } value
            ? value.ToVector2() : null;
    public System.Numerics.Vector3? GetVector3(string name) =>
        ValueQuery(name) is { Type: ParticleDataChannelType.Vec3 } value
            ? value.ToVector3() : null;
    public System.Numerics.Vector4? GetVector4(string name) =>
        ValueQuery(name) is { Type: ParticleDataChannelType.Vec4 } value
            ? value.ToVector4() : null;
    public System.Numerics.Quaternion? GetQuaternion(string name) =>
        ValueQuery(name) is { Type: ParticleDataChannelType.Quat } value
            ? value.ToQuaternion() : null;

    public bool TryGet(string name, out float value)
    {
        var found = ValueQuery(name);
        value = found is { IsScalar: true } scalar ? scalar.Scalar : 0.0f;
        return found is { IsScalar: true };
    }
}

// 粒子实例的托管视图。所有成员都通过所属 Blueprint 的 GT 回调委托访问，
// IsAlive 只反映本地 live 表，不在此处查询原生 World。
public sealed class ParticleSystem
{
    internal Func<ParticleHandle, bool> AliveQuery = null!;
    internal Action<ParticleHandle, bool> VisibleRequest = null!;
    internal Action<ParticleHandle, float> SpawnRateScaleRequest = null!;
    internal Action<ParticleHandle, Vector4> ColorTintRequest = null!;
    internal Action<ParticleHandle, int, bool> EmitterRequest = null!;
    internal Action<ParticleHandle> DestroyRequest = null!;
    internal Action<ParticleHandle> ResetRequest = null!;
    internal Func<ParticleHandle, ParticleTelemetry?> TelemetryQuery = null!;
    internal Func<ParticleHandle, int, string?, ParticleEmitterHandle> SpawnEmitterRequest = null!;
    internal Action<ParticleHandle, string> DestroyEmitterRequest = null!;
    internal Action<ParticleHandle, string> RestartEmitterRequest = null!;
    internal Action<ParticleHandle, string, bool> NamedEmitterRequest = null!;
    internal Action<ParticleHandle, uint, float> BudgetRequest = null!;
    internal Action<ParticleHandle, string, float?> UserParameterRequest = null!;
    internal Action<ParticleHandle, string, string, string?, Vector3?, Vector3?, Vector3?> AttachRequest = null!;
    internal Action<ParticleHandle> DetachRequest = null!;
    internal Func<ParticleHandle, Action<ParticleEvent>, ParticleEventSubscription> SubscribeRequest = null!;
    internal Dictionary<string, ParticleEmitterHandle> Emitters = new(StringComparer.Ordinal);
    public event Action<ParticleTelemetry>? TelemetryChanged;

    public ParticleHandle Handle { get; internal set; }
    public string Id => Handle.Id;
    public bool IsAlive => AliveQuery(Handle);
    public ParticleTelemetry? Telemetry { get; private set; }
    public IReadOnlyDictionary<string, ParticleEmitterHandle> EmitterHandles => Emitters;
    public void Destroy() => DestroyRequest(Handle);
    public void SetVisible(bool visible) => VisibleRequest(Handle, visible);
    public void SetSpawnRateScale(float scale) => SpawnRateScaleRequest(Handle, scale);
    public void SetColorTint(Vector4 color) => ColorTintRequest(Handle, color);
    public void SetEmitterEnabled(int index, bool enabled) => EmitterRequest(Handle, index, enabled);
    public void SetEmitterEnabled(ParticleEmitterHandle emitter, bool enabled) =>
        NamedEmitterRequest(Handle, RequireEmitter(emitter).Id, enabled);
    public void ResetOverrides() => ResetRequest(Handle);

    public ParticleEmitterHandle SpawnEmitter(int sourceIndex, string? name = null)
        => SpawnEmitterRequest(Handle, sourceIndex, name);

    public ParticleEmitterHandle? FindEmitter(string id) =>
        !string.IsNullOrEmpty(id) && Emitters.TryGetValue(id, out var emitter)
            ? emitter : null;

    public void DestroyEmitter(ParticleEmitterHandle emitter)
    {
        var live = RequireEmitter(emitter);
        DestroyEmitterRequest(Handle, live.Id);
        Emitters.Remove(live.Id);
    }

    public void RestartEmitter(ParticleEmitterHandle emitter) =>
        RestartEmitterRequest(Handle, RequireEmitter(emitter).Id);

    public void SetEmitterBudget(uint maxTotalParticles, float emitterBudgetScale) =>
        BudgetRequest(Handle, maxTotalParticles, emitterBudgetScale);

    public void SetUserParameter(string key, float value) =>
        UserParameterRequest(Handle, key, value);

    public void ClearUserParameter(string key) =>
        UserParameterRequest(Handle, key, null);

    public void AttachTo(string targetKind, string targetName, string? component = null,
        Vector3? localPosition = null, Vector3? localRotation = null, Vector3? localScale = null) =>
        AttachRequest(Handle, targetKind, targetName, component,
            localPosition, localRotation, localScale);

    public void Detach() => DetachRequest(Handle);

    public ParticleEventSubscription Subscribe(Action<ParticleEvent> callback) =>
        SubscribeRequest(Handle, callback);

    internal void ApplyTelemetry(ParticleTelemetry telemetry)
    {
        Telemetry = telemetry;
        TelemetryChanged?.Invoke(telemetry);
    }

    private ParticleEmitterHandle RequireEmitter(ParticleEmitterHandle emitter)
    {
        if (emitter.Context != Handle.Context || emitter.SystemId != Id ||
            !Emitters.TryGetValue(emitter.Id, out var live))
            throw new InvalidOperationException("Stale or foreign particle emitter handle.");
        // StableIndex 由 native 遥测回填；句柄身份只由 Context/SystemId/Id 决定，
        // 因此旧句柄不会因为索引从 MaxValue 更新而失效。
        return live;
    }
}

// 每个 Blueprint 持有自己的 live 表；Spawn 产生的命令在本帧 GT 边界后统一导出。
public sealed class ParticleSystems
{
    internal Func<string, Vector3, Vector3?, Vector3?, ParticleSystem> SpawnRequest = null!;
    internal Func<string, ParticleSystem?> FindRequest = null!;
    internal Action DestroyAllRequest = null!;

    public ParticleDataChannels Data { get; internal set; } = new();

    public ParticleSystem Spawn(string assetPath, Vector3 position,
        Vector3? rotation = null, Vector3? scale = null) =>
        SpawnRequest(assetPath, position, rotation, scale);
    public ParticleSystem? Find(string id) => FindRequest(id);
    public void DestroyAll() => DestroyAllRequest();
}
