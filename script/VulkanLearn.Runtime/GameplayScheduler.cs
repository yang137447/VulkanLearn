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

    private sealed partial class GameplayScheduler(Context context)
    {
        private Context AssetContext => context;
        private ulong next;
        private readonly Dictionary<ulong, ScheduledTimer> timers = [];
        private readonly Dictionary<ulong, Subscription> subscriptions = [];
        private readonly List<EventRequest> events = [];
        private readonly Dictionary<ulong, RayRequest> rays = [];
        private readonly HashSet<ulong> sentRays = [];
        private readonly List<Action> undo = [];
        private bool releasing;
        public bool HasPendingEvents => events.Count != 0;

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
            foreach (var id in timers.Values.Where(t => t.Component == component).Select(t => t.Id).ToArray()) timers.Remove(id);
            foreach (var id in subscriptions.Values.Where(s => s.Component == component).Select(s => s.Id).ToArray()) subscriptions.Remove(id);
            foreach (var id in rays.Values.Where(r => r.Component == component).Select(r => r.Id).ToArray())
            { rays.Remove(id); sentRays.Remove(id); }
            RemoveComponentAssets(component);
            events.RemoveAll(e => e.Component == component);
        }
        public object Counts() => new { timers = timers.Count, subscriptions = subscriptions.Count, events = events.Count,
            rays = rays.Count, assets = assets.Count, commands = context.Commands.Count,
            componentCommands = context.ComponentCommands.Count + context.LogicComponentCommands.Count };
        public void RemoveOwner(Blueprint owner)
        {
            RemoveHud(owner);
            RemoveAssetOwner(owner);
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
