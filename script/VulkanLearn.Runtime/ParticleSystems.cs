using System.Numerics;

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

    public ParticleSystem Spawn(string assetPath, Vector3 position,
        Vector3? rotation = null, Vector3? scale = null) =>
        SpawnRequest(assetPath, position, rotation, scale);
    public ParticleSystem? Find(string id) => FindRequest(id);
    public void DestroyAll() => DestroyAllRequest();
}
