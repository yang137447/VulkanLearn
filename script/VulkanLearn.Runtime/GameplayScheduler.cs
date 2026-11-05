using System.Numerics;
using System.Text.Json;

namespace VulkanLearn;

public static partial class NativeBridge
{
    private sealed record ScheduledTimer(ulong Id, Blueprint Owner, Component? Component, double Due, double Interval, Action Callback);
    private sealed record Subscription(ulong Id, Blueprint Owner, Component? Component, string Name, Action<GameEvent> Callback);
    private sealed record EventRequest(Blueprint Owner, Component? Component, BlueprintHandle? Target, string Name, string Payload);
    private sealed record RayRequest(ulong Id, Blueprint Owner, Component? Component, Vector3 From, Vector3 To,
        bool IncludeTriggers, uint Mask, float Radius, bool Local, Action<RaycastHit?> Callback);
    // 使用引用类型，保证回滚时按消息实例移除，不会误删内容相同的另一条命令。
    private sealed class ParticleMessage(Blueprint owner, object value, bool cleanup = false)
    {
        public Blueprint Owner { get; } = owner;
        public object Value { get; } = value;
        public bool Cleanup { get; } = cleanup;
    }
    private sealed record ParticleSubscription(
        ulong Id, Blueprint Owner, string SystemId, Action<ParticleEvent> Callback);

    private sealed partial class GameplayScheduler(Context context)
    {
        private Context AssetContext => context;
        private ulong next;
        private readonly Dictionary<ulong, ScheduledTimer> timers = [];
        private readonly Dictionary<ulong, Subscription> subscriptions = [];
        private readonly List<EventRequest> events = [];
        private readonly Dictionary<ulong, RayRequest> rays = [];
        private readonly HashSet<ulong> sentRays = [];
        private readonly Dictionary<Blueprint, Dictionary<string, ParticleSystem>> particles = [];
        private readonly List<ParticleMessage> particleMessages = [];
        // 数据通道不按 owner 分表：native World 只有一个全局命名空间，这里
        // 保存最近一帧副本供 Get 使用；Set/Clear 在同一帧内先更新本地值。
        private readonly Dictionary<string, ParticleDataChannelValue> particleDataChannels =
            new(StringComparer.Ordinal);
        private readonly Dictionary<ulong, ParticleSubscription> particleSubscriptions = [];
        // native 兴趣按 Blueprint owner 计数；每个 owner 只在首个订阅/最后
        // 一个订阅移除时发命令，避免 A 的清理误删 B 的兴趣。
        private readonly HashSet<string> particleEventInterestOwners = new(StringComparer.Ordinal);
        private readonly List<Action> undo = [];
        private bool releasing;
        public bool HasPendingEvents => events.Count != 0;
        public bool HasPendingParticles => particleMessages.Count != 0;

        public void Commit() => undo.Clear();
        public void Rollback()
        {
            for (int index = undo.Count - 1; index >= 0; --index) undo[index]();
            undo.Clear();
        }
        private void Require(Blueprint owner)
        {
            RequireCallback(context, owner);
            if (releasing || owner.PendingDestroy) throw new InvalidOperationException("Owner is ending play.");
        }
        private void RequireRead(Blueprint owner)
        {
            // EndPlay 是只读收尾阶段：releasing 时禁止命令/结构修改，但允许
            // 读取已经冻结的 World 镜像；真正的 callback/线程/context 校验仍执行。
            RequireCallback(context, owner);
        }
        private static void Name(string name)
        {
            if (string.IsNullOrWhiteSpace(name) || name.Length > 128)
                throw new ArgumentException("Event names require 1..128 characters.");
        }
        public void Bind(Blueprint owner)
        {
            BindAssets(owner);
            owner.Timers = new()
            {
                Schedule = (seconds, callback, repeat) =>
                {
                    Require(owner); ArgumentNullException.ThrowIfNull(callback);
                    if (!double.IsFinite(seconds) || seconds < 0 || repeat && seconds == 0)
                        throw new ArgumentException("Timer delay must be finite, nonnegative, and positive for repeats.");
                    if (timers.Count >= 1024) throw new InvalidOperationException("Timer budget exceeded.");
                    ulong id = ++next;
                    timers.Add(id, new(id, owner, callbackComponent, context.Time.TimeSincePlay + seconds, repeat ? seconds : 0, callback));
                    undo.Add(() => timers.Remove(id));
                    return new(context.Id, id);
                },
                CancelRequest = token =>
                {
                    Require(owner);
                    if (token.Context != context.Id) throw new InvalidOperationException("Stale timer context.");
                    if (!timers.TryGetValue(token.Id, out var timer)) return;
                    if (timer.Owner != owner || timer.Component != callbackComponent)
                        throw new InvalidOperationException("Timer belongs to another callback owner.");
                    timers.Remove(token.Id); undo.Add(() => timers.Add(token.Id, timer));
                }
            };
            owner.Events = new()
            {
                SubscribeRequest = (name, callback) =>
                {
                    Require(owner); Name(name); ArgumentNullException.ThrowIfNull(callback);
                    if (subscriptions.Count >= 1024) throw new InvalidOperationException("Subscription budget exceeded.");
                    ulong id = ++next;
                    subscriptions.Add(id, new(id, owner, callbackComponent, name, callback));
                    undo.Add(() => subscriptions.Remove(id));
                    return new(context.Id, id);
                },
                UnsubscribeRequest = token =>
                {
                    Require(owner);
                    if (token.Context != context.Id) throw new InvalidOperationException("Stale subscription context.");
                    if (!subscriptions.TryGetValue(token.Id, out var subscription)) return;
                    if (subscription.Owner != owner || subscription.Component != callbackComponent)
                        throw new InvalidOperationException("Subscription belongs to another callback owner.");
                    subscriptions.Remove(token.Id); undo.Add(() => subscriptions.Add(token.Id, subscription));
                },
                PublishRequest = (target, name, payload) =>
                {
                    Require(owner); Name(name);
                    if (payload == null || payload.Length > 4096) throw new ArgumentException("Event payload exceeds 4096 characters.");
                    if (target != null) RequireLive(context, target.Value);
                    if (events.Count >= 256) throw new InvalidOperationException("Event queue budget exceeded.");
                    var request = new EventRequest(owner, callbackComponent, target, name, payload);
                    events.Add(request); undo.Add(() => events.Remove(request));
                }
            };
            owner.Physics = new()
            {
                RaycastRequest = (from, to, callback, includeTriggers, mask, radius, local) =>
                {
                    Require(owner); ArgumentNullException.ThrowIfNull(callback);
                    if (!Finite(from) || !Finite(to) || !float.IsFinite(Vector3.Distance(from, to)) || from == to)
                        throw new ArgumentException("Raycast requires a finite nonzero segment.");
                    if (mask > 65535 || !float.IsFinite(radius) || radius < 0 || radius > 100000)
                        throw new ArgumentException("Query requires 16-bit collision mask and finite bounded radius.");
                    if (rays.Count >= 256) throw new InvalidOperationException("Raycast queue budget exceeded.");
                    ulong id = ++next;
                    rays.Add(id, new(id, owner, callbackComponent, from, to, includeTriggers, mask, radius, local, callback));
                    undo.Add(() => rays.Remove(id));
                }
            };
        }
        public void BindParticles(Blueprint owner)
        {
            owner.Particles = new()
            {
                SpawnRequest = (path, position, rotation, scale) =>
                    QueueParticleSpawn(owner, path, position, rotation, scale),
                FindRequest = id => FindParticle(owner, id),
                DestroyAllRequest = () => DestroyAllParticles(owner),
                Data = new()
                {
                    ChangeRequest = (name, value) => ChangeParticleData(owner, name, value),
                    ValueQuery = name => ReadParticleData(owner, name)
                }
            };
        }
        private Dictionary<string, ParticleSystem> ParticleTable(Blueprint owner)
        {
            if (!particles.TryGetValue(owner, out var live))
            {
                live = new(StringComparer.Ordinal);
                particles.Add(owner, live);
            }
            return live;
        }
        private static void ValidateSpawnRateScale(float scale)
        {
            if (!float.IsFinite(scale) || scale < 0 || scale > 16)
                throw new ArgumentOutOfRangeException(nameof(scale),
                    "Particle spawn rate scale must be finite and in [0, 16].");
        }
        private static void ValidateColorTint(Vector4 color)
        {
            if (!float.IsFinite(color.X) || !float.IsFinite(color.Y) ||
                !float.IsFinite(color.Z) || !float.IsFinite(color.W) ||
                color.X < 0 || color.X > 16 || color.Y < 0 || color.Y > 16 ||
                color.Z < 0 || color.Z > 16 || color.W < 0 || color.W > 1)
                throw new ArgumentOutOfRangeException(nameof(color),
                    "Particle color tint requires finite RGB in [0, 16] and alpha in [0, 1].");
        }
        private static void ValidateEmitterBudgetScale(float scale)
        {
            if (!float.IsFinite(scale) || scale < 0 || scale > 16)
                throw new ArgumentOutOfRangeException(nameof(scale),
                    "Particle emitter budget scale must be finite and in [0, 16].");
        }
        private static void ValidateUserParameter(string key, float? value)
        {
            if (string.IsNullOrEmpty(key) || key.Contains('\0') ||
                System.Text.Encoding.UTF8.GetByteCount(key) > 64)
                throw new ArgumentException("Particle user parameter key requires 1..64 UTF-8 bytes without NUL.", nameof(key));
            if (value is { } number && (!float.IsFinite(number) || Math.Abs(number) > 1.0e6f))
                throw new ArgumentOutOfRangeException(nameof(value),
                    "Particle user parameter value must be finite and in [-1e6, 1e6].");
        }
        private static void ValidateDataChannel(string key,
            ParticleDataChannelValue? value)
        {
            if (string.IsNullOrEmpty(key) || key.Contains('\0') ||
                System.Text.Encoding.UTF8.GetByteCount(key) > 64)
                throw new ArgumentException("Particle data channel key requires 1..64 UTF-8 bytes without NUL.", nameof(key));
            value?.Validate(key);
        }
        private ParticleDataChannelValue? ReadParticleData(
            Blueprint owner, string name)
        {
            RequireRead(owner);
            ValidateDataChannel(name, null);
            return particleDataChannels.TryGetValue(name, out var value) ? value : null;
        }
        private void ChangeParticleData(Blueprint owner, string name,
            ParticleDataChannelValue? value)
        {
            Require(owner);
            ValidateDataChannel(name, value);
            if (value is null)
            {
                // clear 对不存在的 key 保持幂等；不把必然失败的 native 命令
                // 投递出去，避免 EndPlay 清理顺序把整个帧批次判为非法。
                if (!particleDataChannels.TryGetValue(name, out var previous)) return;
                QueueParticle(owner, new
                {
                    op = "clearData",
                    owner = owner.Handle.Id,
                    key = name
                });
                particleDataChannels.Remove(name);
                undo.Add(() => particleDataChannels[name] = previous);
                return;
            }
            bool existed = particleDataChannels.TryGetValue(name, out var old);
            if (!existed && particleDataChannels.Count >= 64)
                throw new InvalidOperationException("Particle data channel budget exceeded.");
            QueueParticle(owner, new
            {
                op = "data",
                owner = owner.Handle.Id,
                key = name,
                name,
                type = value.Value.TypeName,
                values = value.Value.ToCommandValues()
            });
            particleDataChannels[name] = value.Value;
            if (existed) undo.Add(() => particleDataChannels[name] = old);
            else undo.Add(() => particleDataChannels.Remove(name));
        }
        private static void ValidateEmitterIndex(int index)
        {
            if (index < 0 || index >= 16)
                throw new ArgumentOutOfRangeException(nameof(index), "Particle emitter index is out of range.");
        }
        private void QueueParticle(Blueprint owner, object value, bool cleanup = false)
        {
            // 普通命令共享 256 条预算；EndPlay 清理命令与 DrawDebug 一样不受预算限制，
            // 以免销毁时泄漏原生实例。清理消息不参与回调回滚。
            if (!cleanup && particleMessages.Count >= 256)
                throw new InvalidOperationException("Particle command budget exceeded.");
            var message = new ParticleMessage(owner, value, cleanup);
            particleMessages.Add(message);
            if (!cleanup) undo.Add(() => particleMessages.Remove(message));
            if (context.InstanceIds.TryGetValue(owner.Handle.Id, out var instance)) instance.Dirty = true;
        }
        private void QueueParticleEventInterest(Blueprint owner, bool enabled)
        {
            if (releasing) return;
            bool wasPresent = particleEventInterestOwners.Contains(owner.Handle.Id);
            bool changed = enabled
                ? particleEventInterestOwners.Add(owner.Handle.Id)
                : particleEventInterestOwners.Remove(owner.Handle.Id);
            if (!changed) return;
            QueueParticle(owner, new
            {
                op = "events",
                owner = owner.Handle.Id,
                enabled
            }, cleanup: !enabled);
            undo.Add(() =>
            {
                if (wasPresent) particleEventInterestOwners.Add(owner.Handle.Id);
                else particleEventInterestOwners.Remove(owner.Handle.Id);
            });
        }
        private ParticleSystem QueueParticleSpawn(Blueprint owner, string assetPath, Vector3 position,
            Vector3? requestedRotation, Vector3? requestedScale)
        {
            Require(owner);
            if (string.IsNullOrEmpty(assetPath))
                throw new ArgumentException("Particle asset path must be non-empty.", nameof(assetPath));
            Vector3 rotation = requestedRotation ?? Vector3.Zero;
            Vector3 scale = requestedScale ?? Vector3.One;
            ValidateTransform(new() { Position = position, Rotation = rotation, Scale = scale });
            string id = $"~particle.{++context.NextParticle}";
            QueueParticle(owner, new { op = "spawn", owner = owner.Handle.Id, id, path = assetPath,
                position = Values(position), rotation = Values(rotation), scale = Values(scale) });
            var particle = new ParticleSystem { Handle = new(context.Id, id) };
            BindParticleRequests(owner, particle);
            var live = ParticleTable(owner);
            live.Add(id, particle);
            undo.Add(() => live.Remove(id));
            return particle;
        }
        private void BindParticleRequests(Blueprint owner, ParticleSystem particle)
        {
            particle.AliveQuery = handle => ParticleAlive(owner, handle);
            particle.DestroyRequest = handle =>
            {
                var liveParticle = RequireParticle(owner, handle);
                QueueParticle(owner, new { op = "destroy", owner = owner.Handle.Id, id = handle.Id });
                var live = particles[owner];
                live.Remove(handle.Id);
                RemoveParticleSubscriptions(owner, handle.Id);
                undo.Add(() => live.Add(handle.Id, liveParticle));
            };
            particle.VisibleRequest = (handle, visible) =>
            {
                RequireParticle(owner, handle);
                QueueParticle(owner, new { op = "visible", owner = owner.Handle.Id, id = handle.Id, value = visible });
            };
            particle.SpawnRateScaleRequest = (handle, scale) =>
            {
                RequireParticle(owner, handle);
                ValidateSpawnRateScale(scale);
                QueueParticle(owner, new { op = "spawnRateScale", owner = owner.Handle.Id, id = handle.Id, value = scale });
            };
            particle.ColorTintRequest = (handle, color) =>
            {
                RequireParticle(owner, handle);
                ValidateColorTint(color);
                QueueParticle(owner, new { op = "colorTint", owner = owner.Handle.Id, id = handle.Id,
                    value = new[] { color.X, color.Y, color.Z, color.W } });
            };
            particle.EmitterRequest = (handle, index, enabled) =>
            {
                RequireParticle(owner, handle);
                ValidateEmitterIndex(index);
                QueueParticle(owner, new { op = "emitter", owner = owner.Handle.Id, id = handle.Id,
                    index, enabled });
            };
            particle.ResetRequest = handle =>
            {
                RequireParticle(owner, handle);
                QueueParticle(owner, new { op = "reset", owner = owner.Handle.Id, id = handle.Id });
            };
            particle.TelemetryQuery = handle =>
            {
                var liveParticle = RequireParticle(owner, handle);
                return liveParticle.Telemetry;
            };
            particle.SpawnEmitterRequest = (handle, sourceIndex, name) =>
            {
                var liveParticle = RequireParticle(owner, handle);
                if (sourceIndex < 0 || sourceIndex >= 16)
                    throw new ArgumentOutOfRangeException(nameof(sourceIndex));
                if (name != null && (name.Length == 0 || name.Length > 128))
                    throw new ArgumentException("Particle emitter name requires 1..128 characters.", nameof(name));
                if (liveParticle.Emitters.Count >= 16)
                    throw new InvalidOperationException("Particle emitter budget exceeded.");
                ulong sequence = ++context.NextParticleEmitter;
                string emitterId = name ?? $"~emitter.{sequence}";
                if (liveParticle.Emitters.ContainsKey(emitterId))
                    throw new ArgumentException("Particle emitter name already exists.", nameof(name));
                var emitter = new ParticleEmitterHandle(context.Id, handle.Id, emitterId, uint.MaxValue);
                QueueParticle(owner, new { op = "spawnEmitter", owner = owner.Handle.Id,
                    id = handle.Id, source = sourceIndex, emitter = emitterId });
                liveParticle.Emitters[emitterId] = emitter;
                undo.Add(() => liveParticle.Emitters.Remove(emitterId));
                return emitter;
            };
            particle.DestroyEmitterRequest = (handle, emitterId) =>
            {
                var liveParticle = RequireParticle(owner, handle);
                if (!liveParticle.Emitters.ContainsKey(emitterId))
                    throw new InvalidOperationException("Stale or foreign particle emitter handle.");
                QueueParticle(owner, new { op = "destroyEmitter", owner = owner.Handle.Id,
                    id = handle.Id, emitter = emitterId });
                var removed = liveParticle.Emitters[emitterId];
                liveParticle.Emitters.Remove(emitterId);
                undo.Add(() => liveParticle.Emitters[emitterId] = removed);
            };
            particle.RestartEmitterRequest = (handle, emitterId) =>
            {
                var liveParticle = RequireParticle(owner, handle);
                if (!liveParticle.Emitters.ContainsKey(emitterId))
                    throw new InvalidOperationException("Stale or foreign particle emitter handle.");
                QueueParticle(owner, new { op = "restartEmitter", owner = owner.Handle.Id,
                    id = handle.Id, emitter = emitterId });
            };
            particle.NamedEmitterRequest = (handle, emitterId, enabled) =>
            {
                var liveParticle = RequireParticle(owner, handle);
                if (!liveParticle.Emitters.ContainsKey(emitterId))
                    throw new InvalidOperationException("Stale or foreign particle emitter handle.");
                QueueParticle(owner, new { op = "emitter", owner = owner.Handle.Id,
                    id = handle.Id, emitter = emitterId, enabled });
            };
            particle.BudgetRequest = (handle, maxTotal, scale) =>
            {
                RequireParticle(owner, handle);
                if (maxTotal > 1_000_000)
                    throw new ArgumentOutOfRangeException(nameof(maxTotal));
                ValidateEmitterBudgetScale(scale);
                QueueParticle(owner, new { op = "budget", owner = owner.Handle.Id,
                    id = handle.Id, maxTotal, emitterScale = scale });
            };
            particle.UserParameterRequest = (handle, key, value) =>
            {
                RequireParticle(owner, handle);
                ValidateUserParameter(key, value);
                if (value.HasValue)
                    QueueParticle(owner, new { op = "userParam", owner = owner.Handle.Id,
                        id = handle.Id, key, value = value.Value });
                else
                    QueueParticle(owner, new { op = "clearUserParam", owner = owner.Handle.Id,
                        id = handle.Id, key });
            };
            particle.AttachRequest = (handle, kind, targetName, component,
                localPosition, localRotation, localScale) =>
            {
                RequireParticle(owner, handle);
                if (kind != "mesh" && kind != "component")
                    throw new ArgumentException("Particle attachment kind must be mesh or component.", nameof(kind));
                if (string.IsNullOrEmpty(targetName) || targetName.Length > 128)
                    throw new ArgumentException("Particle attachment target requires 1..128 characters.", nameof(targetName));
                if (component != null && component.Length > 64)
                    throw new ArgumentException("Particle attachment component exceeds 64 characters.", nameof(component));
                Vector3 position = localPosition ?? Vector3.Zero;
                Vector3 rotation = localRotation ?? Vector3.Zero;
                Vector3 scale = localScale ?? Vector3.One;
                ValidateTransform(new() { Position = position, Rotation = rotation, Scale = scale });
                QueueParticle(owner, new { op = "attach", owner = owner.Handle.Id,
                    id = handle.Id, targetKind = kind, targetName, component,
                    position = Values(position), rotation = Values(rotation), scale = Values(scale) });
            };
            particle.DetachRequest = handle =>
            {
                RequireParticle(owner, handle);
                QueueParticle(owner, new { op = "detach", owner = owner.Handle.Id, id = handle.Id });
            };
            particle.SubscribeRequest = (handle, callback) =>
            {
                var liveParticle = RequireParticle(owner, handle);
                ArgumentNullException.ThrowIfNull(callback);
                if (particleSubscriptions.Count >= 256)
                    throw new InvalidOperationException("Particle subscription budget exceeded.");
                bool ownerHadSubscriptions = particleSubscriptions.Values
                    .Any(s => s.Owner == owner);
                ulong id = ++next;
                var subscription = new ParticleSubscription(id, owner, liveParticle.Id, callback);
                particleSubscriptions.Add(id, subscription);
                if (!ownerHadSubscriptions) QueueParticleEventInterest(owner, true);
                undo.Add(() => particleSubscriptions.Remove(id));
                return new ParticleEventSubscription(() =>
                {
                    if (particleSubscriptions.Remove(id))
                    {
                        undo.Add(() => particleSubscriptions.Add(id, subscription));
                        if (!particleSubscriptions.Values.Any(s => s.Owner == owner))
                            QueueParticleEventInterest(owner, false);
                    }
                });
            };
        }
        private ParticleSystem RequireParticle(Blueprint owner, ParticleHandle handle)
        {
            Require(owner);
            if (handle.Context != context.Id || !particles.TryGetValue(owner, out var live) ||
                !live.TryGetValue(handle.Id, out var particle) || particle.Handle != handle)
                throw new InvalidOperationException("Stale or foreign particle handle.");
            return particle;
        }
        private bool ParticleAlive(Blueprint owner, ParticleHandle handle)
        {
            Require(owner);
            return handle.Context == context.Id && particles.TryGetValue(owner, out var live) &&
                live.TryGetValue(handle.Id, out var particle) && particle.Handle == handle;
        }
        private ParticleSystem? FindParticle(Blueprint owner, string id)
        {
            Require(owner);
            return !string.IsNullOrEmpty(id) && particles.TryGetValue(owner, out var live) &&
                live.TryGetValue(id, out var particle) ? particle : null;
        }
        private void DestroyAllParticles(Blueprint owner)
        {
            Require(owner);
            if (!particles.TryGetValue(owner, out var live) || live.Count == 0) return;
            var values = live.Values.ToArray();
            if (particleMessages.Count + values.Length > 256)
                throw new InvalidOperationException("Particle command budget exceeded.");
            foreach (var particle in values)
            {
                QueueParticle(owner, new { op = "destroy", owner = owner.Handle.Id, id = particle.Id });
                live.Remove(particle.Id);
                undo.Add(() => live.Add(particle.Id, particle));
            }
        }
        public object[] TakeParticles(Blueprint owner)
        {
            if (particleMessages.Count == 0) return Array.Empty<object>();
            var values = particleMessages.Where(m => m.Owner == owner).Select(m => m.Value).ToArray();
            particleMessages.RemoveAll(m => m.Owner == owner);
            return values;
        }
        public void ApplyParticleTelemetry(JsonElement telemetry)
        {
            if (telemetry.ValueKind != JsonValueKind.Array) return;
            foreach (var item in telemetry.EnumerateArray())
            {
                if (item.ValueKind != JsonValueKind.Object ||
                    !item.TryGetProperty("name", out var name) ||
                    name.ValueKind != JsonValueKind.String) continue;
                string systemId = name.GetString()!;
                var emitterValues = new List<ParticleEmitterTelemetry>();
                if (item.TryGetProperty("emitters", out var emitters) &&
                    emitters.ValueKind == JsonValueKind.Array)
                {
                    foreach (var emitter in emitters.EnumerateArray())
                    {
                        emitterValues.Add(new(
                            emitter.TryGetProperty("index", out var index) ? index.GetUInt32() : 0,
                            emitter.TryGetProperty("name", out var emitterName) ? emitterName.GetString()! : "",
                            emitter.TryGetProperty("active", out var active) && active.GetBoolean(),
                            emitter.TryGetProperty("enabled", out var enabled) && enabled.GetBoolean(),
                            emitter.TryGetProperty("restartEpoch", out var restartEpoch) ? restartEpoch.GetUInt32() : 0,
                            emitter.TryGetProperty("maxParticles", out var max) ? max.GetUInt32() : 0,
                            emitter.TryGetProperty("budgetedMaxParticles", out var budgetedMax) ? budgetedMax.GetUInt32() : 0,
                            emitter.TryGetProperty("aliveParticles", out var emitterAlive) ? emitterAlive.GetUInt32() : 0,
                            emitter.TryGetProperty("aliveAvailable", out var emitterAliveAvailable) && emitterAliveAvailable.GetBoolean()));
                    }
                }
                var snapshot = new ParticleTelemetry(
                    item.TryGetProperty("emitterCount", out var count) ? count.GetUInt32() : 0,
                    item.TryGetProperty("activeEmitterCount", out var activeCount) ? activeCount.GetUInt32() : 0,
                    item.TryGetProperty("enabledEmitterCount", out var enabledCount) ? enabledCount.GetUInt32() : 0,
                    item.TryGetProperty("visibleEmitterCount", out var visibleCount) ? visibleCount.GetUInt32() : 0,
                    item.TryGetProperty("authoredCapacity", out var authored) ? authored.GetUInt32() : 0,
                    item.TryGetProperty("budgetedCapacity", out var budgeted) ? budgeted.GetUInt32() : 0,
                    item.TryGetProperty("aliveAvailable", out var systemAliveAvailable) && systemAliveAvailable.GetBoolean(),
                    item.TryGetProperty("aliveParticles", out var systemAlive) ? systemAlive.GetUInt32() : 0,
                    item.TryGetProperty("lastFrameBudget", out var frameBudget) ? frameBudget.GetUInt32() : 0,
                    emitterValues.ToArray());
                foreach (var table in particles.Values)
                {
                    if (!table.TryGetValue(systemId, out var particle)) continue;
                    particle.ApplyTelemetry(snapshot);
                    foreach (var emitter in emitterValues)
                        if (particle.Emitters.TryGetValue(emitter.Name, out var handle) &&
                            handle.StableIndex != emitter.StableIndex)
                            particle.Emitters[emitter.Name] = handle with { StableIndex = emitter.StableIndex };
                }
            }
        }
        public void ApplyParticleDataChannels(JsonElement channels)
        {
            if (channels.ValueKind != JsonValueKind.Object) return;
            particleDataChannels.Clear();
            foreach (var entry in channels.EnumerateObject())
            {
                if (entry.Name.Length == 0 || entry.Name.Contains('\0') ||
                    System.Text.Encoding.UTF8.GetByteCount(entry.Name) > 64) continue;
                if (!ParticleDataChannelValue.TryParse(entry.Value, out var value))
                    continue;
                if (particleDataChannels.Count >= 64 &&
                    !particleDataChannels.ContainsKey(entry.Name)) continue;
                particleDataChannels[entry.Name] = value;
            }
        }
        public Dictionary<string, ParticleDataChannelValue> ParticleDataChannelSnapshot() =>
            new(particleDataChannels, StringComparer.Ordinal);
        public void RestoreParticleDataChannels(
            IReadOnlyDictionary<string, ParticleDataChannelValue> values)
        {
            particleDataChannels.Clear();
            foreach (var entry in values)
            {
                if (string.IsNullOrEmpty(entry.Key) ||
                    System.Text.Encoding.UTF8.GetByteCount(entry.Key) > 64 ||
                    entry.Key.Contains('\0')) continue;
                try
                {
                    entry.Value.Validate(entry.Key);
                }
                catch (ArgumentOutOfRangeException)
                {
                    continue;
                }
                if (particleDataChannels.Count >= 64) break;
                particleDataChannels[entry.Key] = entry.Value;
            }
        }
        public void ApplyParticleEvents(JsonElement events)
        {
            if (events.ValueKind != JsonValueKind.Array) return;
            foreach (var item in events.EnumerateArray())
            {
                if (item.ValueKind != JsonValueKind.Object) continue;
                string systemId = item.TryGetProperty("system", out var system) ? system.GetString()! : "";
                string emitterId = item.TryGetProperty("emitter", out var emitter) ? emitter.GetString()! : "";
                string kindText = item.TryGetProperty("kind", out var kind) ? kind.GetString()! : "";
                if (!Enum.TryParse<ParticleEventKind>(kindText, true, out var kindValue)) continue;
                float time = item.TryGetProperty("time", out var timeValue) ? timeValue.GetSingle() : 0;
                Vector3 position = ReadParticleEventVector(item, "position");
                Vector3 normal = ReadParticleEventVector(item, "normal");
                var particleEvent = new ParticleEvent(
                    context.Id, systemId, emitterId, kindValue, time, position, normal);
                foreach (var subscription in particleSubscriptions.Values
                    .Where(s => s.SystemId == systemId).OrderBy(s => s.Id).ToArray())
                {
                    if (!particleSubscriptions.ContainsKey(subscription.Id) ||
                        !CanRun(subscription.Owner)) continue;
                    Invoke(subscription.Owner, "ParticleEvent",
                        () => subscription.Callback(particleEvent));
                }
            }
        }
        private static Vector3 ReadParticleEventVector(JsonElement item, string field)
        {
            if (!item.TryGetProperty(field, out var value) ||
                value.ValueKind != JsonValueKind.Array || value.GetArrayLength() != 3)
                return Vector3.Zero;
            return new(value[0].GetSingle(), value[1].GetSingle(), value[2].GetSingle());
        }
        private void RemoveParticleSubscriptions(Blueprint owner, string? systemId = null)
        {
            bool removed = false;
            foreach (var id in particleSubscriptions.Values
                .Where(s => s.Owner == owner &&
                    (systemId == null || s.SystemId == systemId))
                .Select(s => s.Id).ToArray())
                removed = particleSubscriptions.Remove(id) || removed;
            if (removed && !particleSubscriptions.Values.Any(s => s.Owner == owner))
                QueueParticleEventInterest(owner, false);
        }
        private void RemoveParticles(Blueprint owner)
        {
            if (!particles.Remove(owner, out var live)) return;
            RemoveParticleSubscriptions(owner);
            // EndPlay 必须先补发 destroy，避免原生 World 遗漏仍在 live 表的实例；
            // 已经排队的用户命令保留，由本帧 Export 继续交付。
            if (releasing) return;
            foreach (var particle in live.Values)
                QueueParticle(owner, new { op = "destroy", owner = owner.Handle.Id, id = particle.Id }, cleanup: true);
        }
        public string[] ParticleIds(Blueprint owner) =>
            particles.TryGetValue(owner, out var live) ? live.Keys.OrderBy(id => id, StringComparer.Ordinal).ToArray() : [];
        public Dictionary<string, string[]> ParticleEmitterIds(Blueprint owner)
        {
            var result = new Dictionary<string, string[]>(StringComparer.Ordinal);
            if (!particles.TryGetValue(owner, out var live)) return result;
            foreach (var particle in live.Values)
                result[particle.Id] = particle.Emitters.Keys
                    .OrderBy(id => id, StringComparer.Ordinal).ToArray();
            return result;
        }
        public void RestoreParticleEmitters(Blueprint owner,
            IReadOnlyDictionary<string, string[]> emitters)
        {
            if (!particles.TryGetValue(owner, out var live)) return;
            foreach (var entry in emitters)
            {
                if (!live.TryGetValue(entry.Key, out var particle))
                    throw new InvalidOperationException("Migration particle emitter owner is missing.");
                foreach (var emitterId in entry.Value)
                {
                    if (string.IsNullOrEmpty(emitterId) ||
                        particle.Emitters.ContainsKey(emitterId))
                        throw new InvalidOperationException("Migration particle emitter identity is invalid or duplicated.");
                    particle.Emitters.Add(emitterId,
                        new ParticleEmitterHandle(context.Id, particle.Id, emitterId, uint.MaxValue));
                }
            }
        }
        public void RestoreParticles(Blueprint owner, IEnumerable<string> ids)
        {
            var live = ParticleTable(owner);
            foreach (var id in ids)
            {
                if (string.IsNullOrEmpty(id) || live.ContainsKey(id))
                    throw new InvalidOperationException("Migration particle identity is invalid or duplicated.");
                var particle = new ParticleSystem { Handle = new(context.Id, id) };
                BindParticleRequests(owner, particle);
                live.Add(id, particle);
            }
        }
        public void RunTimers()
        {
            // 每帧固定领取一次到期集合；重复计时器跳过积压，回调新建的零延迟任务留到下一帧。
            var due = timers.Values.Where(t => t.Due <= context.Time.TimeSincePlay)
                .OrderBy(t => t.Due).ThenBy(t => t.Id).ToArray();
            foreach (var timer in due)
            {
                if (!timers.ContainsKey(timer.Id) || !CanRun(timer.Owner, timer.Component)) continue;
                if (timer.Interval == 0) timers.Remove(timer.Id);
                else timers[timer.Id] = timer with { Due = context.Time.TimeSincePlay + timer.Interval };
                Invoke(timer.Owner, "Timer", timer.Callback, timer.Component);
            }
        }
        public void RunEvents()
        {
            var pending = events.ToArray();
            events.Clear();
            foreach (var message in pending)
            {
                foreach (var subscription in subscriptions.Values.Where(s => s.Name == message.Name &&
                    (message.Target == null || message.Target == s.Owner.Handle)).OrderBy(s => s.Id).ToArray())
                {
                    if (!subscriptions.ContainsKey(subscription.Id) || !CanRun(subscription.Owner, subscription.Component)) continue;
                    Invoke(subscription.Owner, "Event", () => subscription.Callback(
                        new(message.Owner.Handle, message.Name, message.Payload)), subscription.Component);
                }
            }
        }
        public object[] TakeRays(Blueprint owner) => rays.Count == 0 ? [] :
            rays.Values.Where(r => r.Owner == owner && !sentRays.Contains(r.Id))
            .OrderBy(r => r.Id).Select(r =>
            {
                sentRays.Add(r.Id);
                return (object)new { id = r.Id, owner = owner.Handle.Id, from = Values(r.From),
                    to = Values(r.To), includeTriggers = r.IncludeTriggers, collisionMask = r.Mask,
                    radius = r.Radius, localSpace = r.Local };
            }).ToArray();
        // 引擎级查询在物理后的 camera_prepare 排队，camera_finish 同帧消费；不推进游戏时钟。
        public void QueueSpringArmRay(Blueprint owner, Vector3 from, Vector3 to, float radius, uint mask,
            Action<RaycastHit?> callback)
        {
            if (releasing || !CanRun(owner) || rays.Count >= 256 ||
                !Finite(from) || !Finite(to) || from == to) return;
            ulong id = ++next;
            var request = new RayRequest(id, owner, null, from, to, false, mask, radius, false, callback);
            rays.Add(id, request);
            undo.Add(() => rays.Remove(id));
        }
        // arm ID 进入 key，多个臂的调试线不能互相覆盖；帧末发布后由单帧生命周期自动回收。
        public void QueueSpringArmDebug(Blueprint owner, string armId, Vector3 begin, Vector3 end, Vector3? hit)
        {
            if (!CanRun(owner)) return;
            var color = new[] { 1f, 0.85f, 0.15f, 1f };
            var empty = Array.Empty<float[]>();
            QueueDebug(owner, new { op = "draw", shape = 1, publisher = "SpringArm", key = "~armLine/" + armId,
                category = "SpringArm", color, thickness = 2, duration = 0, depth = 0,
                begin = Values(begin), end = Values(end), extents = Values(Vector3.Zero),
                orientation = new[] { 0f, 0f, 0f, 1f }, radius = 0f, halfHeight = 0f, size = 0f,
                segments = 16, closed = false, points = empty, text = "" }, component: null, cost: 1024);
            if (hit is { } point)
                QueueDebug(owner, new { op = "draw", shape = 5, publisher = "SpringArm", key = "~armHit/" + armId,
                    category = "SpringArm", color, thickness = 2, duration = 0, depth = 0,
                    begin = Values(point), end = Values(point), extents = Values(Vector3.Zero),
                    orientation = new[] { 0f, 0f, 0f, 1f }, radius = 8f, halfHeight = 0f, size = 0f,
                    segments = 16, closed = false, points = empty, text = "" }, component: null, cost: 1024);
        }
        public void CompleteRays(JsonElement results)
        {
            foreach (var result in results.EnumerateArray())
            {
                ulong id = result.GetProperty("id").GetUInt64();
                if (!rays.Remove(id, out var ray)) continue;
                sentRays.Remove(id);
                if (!CanRun(ray.Owner, ray.Component)) continue;
                RaycastHit? hit = result.TryGetProperty("blueprint", out var target)
                    ? new(new(context.Id, target.GetString()!), Vector(result.GetProperty("point")),
                        Vector(result.GetProperty("normal")), result.GetProperty("distance").GetSingle()) : null;
                Invoke(ray.Owner, "Raycast", () => ray.Callback(hit), ray.Component);
            }
        }
        private static bool CanRun(Blueprint blueprint, Component? component = null) => blueprint.Enabled &&
            !blueprint.PendingDestroy && (component == null || component.IsAlive && component.Enabled && !component.Failed);
        private void Invoke(Blueprint owner, string phase, Action action, Component? component = null)
        {
            var instance = context.Instances.Single(i => i.Blueprint == owner);
            Callback(context, instance, phase, 0, EndPlayReason.Stopped, custom: action, scriptComponent: component);
        }
        public void RemoveComponent(Component component)
        {
            RemoveHud(component.Owner, component);
            RemoveDebug(component.Owner, component);
            foreach (var id in timers.Values.Where(t => t.Component == component).Select(t => t.Id).ToArray()) timers.Remove(id);
            foreach (var id in subscriptions.Values.Where(s => s.Component == component).Select(s => s.Id).ToArray()) subscriptions.Remove(id);
            foreach (var id in rays.Values.Where(r => r.Component == component).Select(r => r.Id).ToArray())
            { rays.Remove(id); sentRays.Remove(id); }
            RemoveComponentAssets(component);
            events.RemoveAll(e => e.Component == component);
        }
        public object Counts() => new { timers = timers.Count, subscriptions = subscriptions.Count, events = events.Count,
            rays = rays.Count, assets = assets.Count, particles = particleMessages.Count, commands = context.Commands.Count,
            componentCommands = context.ComponentCommands.Count + context.LogicComponentCommands.Count };
        public void RemoveOwner(Blueprint owner)
        {
            RemoveHud(owner);
            RemoveDebug(owner);
            RemoveAssetOwner(owner);
            RemoveParticles(owner);
            foreach (var id in timers.Values.Where(t => t.Owner == owner).Select(t => t.Id).ToArray()) timers.Remove(id);
            foreach (var id in subscriptions.Values.Where(s => s.Owner == owner).Select(s => s.Id).ToArray()) subscriptions.Remove(id);
            foreach (var id in rays.Values.Where(r => r.Owner == owner).Select(r => r.Id).ToArray())
            { rays.Remove(id); sentRays.Remove(id); }
            events.RemoveAll(e => e.Owner == owner);
        }
        public void Release()
        {
            ReleaseAssets();
            releasing = true;
            debugMessages.Clear();
            debugPublishers.Clear();
            particles.Clear(); particleMessages.Clear();
            particleEventInterestOwners.Clear();
            timers.Clear(); subscriptions.Clear(); events.Clear(); rays.Clear(); sentRays.Clear(); undo.Clear(); hud.Clear();
        }
        // EndPlay 仍可读取通道；回调结束后才释放托管镜像。World 表本身
        // 随 Play 上下文之外的原生 World 存活，下次 start/tick 会重新下发。
        public void ClearParticleDataChannels() =>
            particleDataChannels.Clear();
    }
    private static object GetServiceCounts(Context context) => context.Services.Counts();
    private static Instance RequireLive(Context context, BlueprintHandle handle)
    {
        var instance = handle.Context == context.Id ? context.Instances.SingleOrDefault(i =>
            i.Blueprint.Handle == handle && !i.Blueprint.PendingDestroy) : null;
        return instance ?? throw new InvalidOperationException("Stale Blueprint handle.");
    }
    private static void BindWorldQueries(Context context, Blueprint blueprint)
    {
        blueprint.World.FindQuery = id =>
        {
            RequireCallback(context, blueprint);
            return context.Instances.FirstOrDefault(i => i.Blueprint.Handle.Id == id && !i.Blueprint.PendingDestroy)?.Blueprint.Handle;
        };
        blueprint.World.FindAllQuery = type =>
        {
            RequireCallback(context, blueprint);
            return Array.AsReadOnly(context.Instances.Where(i => !i.Blueprint.PendingDestroy &&
                (type.Length == 0 || i.Blueprint.GetType().FullName == type)).Select(i => i.Blueprint.Handle).ToArray());
        };
        blueprint.World.BlueprintQuery = handle =>
        {
            RequireCallback(context, blueprint);
            var target = RequireLive(context, handle).Blueprint;
            return new(target.Handle, target.Name, target.GetType().FullName!, target.Transform.Position,
                target.Transform.Rotation, target.Transform.Scale, target.Enabled);
        };
    }
}
