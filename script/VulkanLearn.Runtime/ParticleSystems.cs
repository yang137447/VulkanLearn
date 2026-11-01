using System.Numerics;

namespace VulkanLearn;

// 粒子句柄只在一次 Play 上下文内有效；Id 是原生场景对象名，Context 防止跨 Play 复用。
public readonly record struct ParticleHandle(ulong Context, string Id);

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

    public ParticleHandle Handle { get; internal set; }
    public string Id => Handle.Id;
    public bool IsAlive => AliveQuery(Handle);
    public void Destroy() => DestroyRequest(Handle);
    public void SetVisible(bool visible) => VisibleRequest(Handle, visible);
    public void SetSpawnRateScale(float scale) => SpawnRateScaleRequest(Handle, scale);
    public void SetColorTint(Vector4 color) => ColorTintRequest(Handle, color);
    public void SetEmitterEnabled(int index, bool enabled) => EmitterRequest(Handle, index, enabled);
    public void ResetOverrides() => ResetRequest(Handle);
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
