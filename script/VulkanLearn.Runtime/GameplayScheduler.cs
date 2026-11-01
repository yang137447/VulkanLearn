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
                DestroyAllRequest = () => DestroyAllParticles(owner)
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
        private void RemoveParticles(Blueprint owner)
        {
            if (!particles.Remove(owner, out var live)) return;
            // EndPlay 必须先补发 destroy，避免原生 World 遗漏仍在 live 表的实例；
            // 已经排队的用户命令保留，由本帧 Export 继续交付。
            if (releasing) return;
            foreach (var particle in live.Values)
                QueueParticle(owner, new { op = "destroy", owner = owner.Handle.Id, id = particle.Id }, cleanup: true);
        }
        public string[] ParticleIds(Blueprint owner) =>
            particles.TryGetValue(owner, out var live) ? live.Keys.OrderBy(id => id, StringComparer.Ordinal).ToArray() : [];
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
            timers.Clear(); subscriptions.Clear(); events.Clear(); rays.Clear(); sentRays.Clear(); undo.Clear(); hud.Clear();
        }
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
