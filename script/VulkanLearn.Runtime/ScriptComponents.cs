using System.Numerics;
using System.Text.Json;

namespace VulkanLearn;

public readonly record struct ScriptComponentHandle(ulong Context, string BlueprintId, string Id, ulong Generation);

// 逻辑组件不占渲染父链；生命周期独立隔离，服务仍由所属 Blueprint 的 GT 边界统一发布。
public abstract class Component
{
    public ScriptComponentHandle Handle { get; internal set; }
    public string Id => Handle.Id;
    public Blueprint Owner { get; internal set; } = null!;
    public bool IsAlive => AliveQuery?.Invoke() ?? false;
    public bool Failed { get; internal set; }
    private bool enabled = true;
    internal Action? CheckAccess;
    internal Func<bool>? AliveQuery;
    public bool Enabled { get => enabled; set { CheckAccess?.Invoke(); enabled = value; } }
    internal void RestoreEnabled(bool value) => enabled = value;
    public Transform Transform { get { CheckAccess?.Invoke(); return Owner.Transform; } }
    public Input Input => Owner.Input;
    public Time Time => Owner.Time;
    public BlueprintWorld World => Owner.World;
    public BlueprintTimers Timers => Owner.Timers;
    public BlueprintEvents Events => Owner.Events;
    public BlueprintPhysics Physics => Owner.Physics;
    public BlueprintAssets Assets => Owner.Assets;
    public GameplayHud Hud => Owner.Hud;
    public DebugDraw DebugDraw => Owner.DebugDraw;
    protected virtual void OnCreate() { }
    protected virtual void BeginPlay() { }
    protected virtual void Tick(float deltaTime) { }
    protected virtual void LateTick(float deltaTime) { }
    protected virtual void EndPlay(EndPlayReason reason) { }
    protected virtual void OnReload() { }
    internal void Create() => OnCreate();
    internal void Begin() => BeginPlay();
    internal void Update(float dt) => Tick(dt);
    internal void UpdateLate(float dt) => LateTick(dt);
    internal void End(EndPlayReason reason) => EndPlay(reason);
    internal void Reload() => OnReload();
}

public sealed class ScriptComponentDescription
{
    public string Id { get; init; } = "";
    public string Class { get; init; } = "";
    public bool Enabled { get; init; } = true;
    public IReadOnlyDictionary<string, object> Properties { get; init; } = new Dictionary<string, object>();
}

public abstract partial class Blueprint
{
    public IReadOnlyList<Component> ScriptComponents { get; internal set; } = Array.Empty<Component>();
    internal Func<ScriptComponentDescription, ScriptComponentHandle> AddScriptComponentRequest = null!;
    internal Action<ScriptComponentHandle> RemoveScriptComponentRequest = null!;
    internal Func<ScriptComponentHandle, Component?> ResolveScriptComponentQuery = null!;
    public Component GetScriptComponent(string id) => ScriptComponents.Single(c => c.Id == id);
    public T GetScriptComponent<T>(string id) where T : Component => (T)GetScriptComponent(id);
    public ScriptComponentHandle AddScriptComponent(ScriptComponentDescription description) => AddScriptComponentRequest(description);
    public ScriptComponentHandle AddScriptComponent<T>(string id) where T : Component =>
        AddScriptComponent(new() { Id = id, Class = typeof(T).FullName! });
    public void RemoveScriptComponent(ScriptComponentHandle handle) => RemoveScriptComponentRequest(handle);
    public Component? ResolveScriptComponent(ScriptComponentHandle handle) => ResolveScriptComponentQuery(handle);
}

public static partial class NativeBridge
{
    private static Component? callbackComponent;
    private sealed class LogicInstance(Component value, JsonElement node)
    {
        public Component Value = value;
        public JsonElement Node = node;
        public JsonElement Configuration;
        public bool Started;
    }
    private sealed partial class Instance
    {
        public readonly List<LogicInstance> LogicComponents = [];
    }
    private sealed record LogicComponentCommand(Instance Owner, string Kind, ScriptComponentHandle Handle,
        JsonElement Node);

    private static LogicInstance NewScriptComponent(Context context, Instance instance, JsonElement node,
        ScriptComponentHandle? handle = null)
    {
        string id = node.GetProperty("id").GetString()!, className = node.GetProperty("class").GetString()!;
        if (string.IsNullOrEmpty(id) || id.IndexOfAny(['/', '\\', '|', '\0']) >= 0 ||
            !TryResolveGameplayType(context.Assembly.ComponentTypes, className, out var type))
            throw new ArgumentException("Script component requires a stable ID and a registered Component class.");
        var value = (Component)Activator.CreateInstance(type)!;
        value.Owner = instance.Blueprint;
        value.Handle = handle ?? new(context.Id, value.Owner.Handle.Id, id, ++context.NextComponent);
        value.RestoreEnabled(!node.TryGetProperty("enabled", out var enabled) || enabled.GetBoolean());
        BindExposedProperties(value, node.GetProperty("properties"));
        value.AliveQuery = () => !context.Released && !value.Owner.PendingDestroy &&
            instance.LogicComponents.Any(c => ReferenceEquals(c.Value, value));
        value.CheckAccess = () =>
        {
            if (context.Released || !instance.LogicComponents.Any(c => ReferenceEquals(c.Value, value)))
                throw new InvalidOperationException("Stale script component.");
        };
        return new(value, node.Clone())
        {
            Configuration = JsonSerializer.SerializeToElement(Properties(type).ToDictionary(p => p.Name,
                p => PropertyValue(p.GetValue(value), p.PropertyType, type.FullName + "." + p.Name)))
        };
    }

    private static void PublishScriptComponents(Instance instance) =>
        instance.Blueprint.ScriptComponents = Array.AsReadOnly(instance.LogicComponents.Select(c => c.Value).ToArray());

    private static void BindScriptComponents(Context context, Instance instance, JsonElement declaration)
    {
        if (declaration.TryGetProperty("scriptComponents", out var components))
            foreach (var node in components.EnumerateArray())
                instance.LogicComponents.Add(NewScriptComponent(context, instance, node));
        if (instance.LogicComponents.Count > 128 ||
            instance.LogicComponents.Select(c => c.Value.Id).Distinct(StringComparer.Ordinal).Count() != instance.LogicComponents.Count)
            throw new ArgumentException("Duplicate or excessive script components.");
        PublishScriptComponents(instance);
        var owner = instance.Blueprint;
        owner.ResolveScriptComponentQuery = handle =>
        {
            RequireCallback(context, owner);
            if (handle.Context != context.Id || handle.BlueprintId != owner.Handle.Id)
                throw new InvalidOperationException("Stale or foreign script component context.");
            return owner.ScriptComponents.SingleOrDefault(c => c.Handle == handle);
        };
        owner.AddScriptComponentRequest = description =>
        {
            RequireCallback(context, owner);
            if (context.LogicComponentCommands.Count >= 256) throw new InvalidOperationException("Script component command budget exceeded.");
            if (!TryResolveGameplayType(context.Assembly.ComponentTypes, description.Class, out var componentType))
                throw new ArgumentException("Unknown script component class.");
            var node = JsonSerializer.SerializeToElement(new
            {
                id = description.Id, @class = description.Class, enabled = description.Enabled,
                properties = DescriptionProperties(componentType, description.Properties)
            });
            var candidate = NewScriptComponent(context, instance, node);
            var ids = instance.LogicComponents.Select(c => c.Value.Id).ToHashSet(StringComparer.Ordinal);
            foreach (var command in context.LogicComponentCommands.Where(c => c.Owner == instance))
            {
                if (command.Kind == "add") ids.Add(command.Handle.Id); else ids.Remove(command.Handle.Id);
            }
            if (ids.Count >= 128 || !ids.Add(description.Id))
                throw new InvalidOperationException("Duplicate or excessive script components.");
            context.LogicComponentCommands.Add(new(instance, "add", candidate.Value.Handle, node));
            return candidate.Value.Handle;
        };
        owner.RemoveScriptComponentRequest = handle =>
        {
            RequireCallback(context, owner);
            var current = instance.LogicComponents.SingleOrDefault(c => c.Value.Id == handle.Id)?.Value.Handle;
            var pending = context.LogicComponentCommands.LastOrDefault(c => c.Owner == instance && c.Handle.Id == handle.Id);
            if (pending != null) current = pending.Kind == "add" ? pending.Handle : null;
            if (handle.Context != context.Id || handle.BlueprintId != owner.Handle.Id || current != handle)
                throw new InvalidOperationException("Stale or foreign script component.");
            if (context.LogicComponentCommands.Count >= 256) throw new InvalidOperationException("Script component command budget exceeded.");
            context.LogicComponentCommands.Add(new(instance, "remove", handle, default));
        };
    }

    private static void CreateScriptComponents(Context context, Instance instance)
    {
        foreach (var component in instance.LogicComponents)
        {
            long started = System.Diagnostics.Stopwatch.GetTimestamp();
            Exception? failure = null;
            try { component.Value.Create(); }
            catch (Exception error) { failure = error; throw; }
            finally
            {
                RecordComponentTiming(instance, component.Value.Id, component.Value.GetType().FullName!, "OnCreate",
                    started, failure);
                RecordComponentState(instance, component.Value.Id, component.Value.GetType().FullName!, "OnCreate",
                    component.Value.Enabled, failure != null, failure);
            }
        }
    }

    private static void InvokeScriptComponent(Component component, string phase, float dt, EndPlayReason reason, Action? custom)
    {
        switch (phase)
        {
            case "BeginPlay": component.Begin(); break;
            case "Tick": component.Update(dt); break;
            case "LateTick": component.UpdateLate(dt); break;
            case "EndPlay": component.End(reason); break;
            case "OnReload": component.Reload(); break;
            default: custom?.Invoke(); break;
        }
    }

    private static void RunScriptComponents(Context context, Instance instance, string phase, float dt, EndPlayReason reason)
    {
        var owner = instance.Blueprint;
        foreach (var component in instance.LogicComponents.ToArray())
        {
            bool active = !instance.Failed && owner.Enabled && !owner.PendingDestroy && !component.Value.Failed && component.Value.Enabled;
            if ((phase == "EndPlay" || !active) && component.Started)
            {
                Callback(context, instance, "EndPlay", dt, reason, scriptComponent: component.Value);
                component.Started = false;
                context.Services.RemoveComponent(component.Value);
            }
            if (!active || phase == "EndPlay") continue;
            if (!component.Started)
            {
                component.Started = true;
                Callback(context, instance, "BeginPlay", dt, reason, scriptComponent: component.Value);
            }
            if (phase != "BeginPlay" && !component.Value.Failed && component.Value.Enabled && !owner.PendingDestroy)
                Callback(context, instance, phase, dt, reason, scriptComponent: component.Value);
            if (component.Started && (component.Value.Failed || !component.Value.Enabled || instance.Failed ||
                !owner.Enabled || owner.PendingDestroy))
            {
                Callback(context, instance, "EndPlay", dt, reason, scriptComponent: component.Value);
                component.Started = false;
                context.Services.RemoveComponent(component.Value);
            }
            RecordComponentState(instance, component.Value.Id, component.Value.GetType().FullName!,
                component.Started ? phase : "EndPlay",
                component.Value.Enabled, component.Value.Failed);
        }
    }

    private static void ReloadScriptComponent(Context context, Instance instance, Component component)
    {
        callbackBlueprint = instance.Blueprint; callbackComponent = component;
        long started = System.Diagnostics.Stopwatch.GetTimestamp();
        Exception? failure = null;
        try { component.Reload(); context.Services.Commit(); }
        catch (Exception error) { failure = error; throw; }
        finally
        {
            RecordComponentTiming(instance, component.Id, component.GetType().FullName!, "OnReload", started, failure);
            RecordComponentState(instance, component.Id, component.GetType().FullName!, "OnReload",
                component.Enabled, component.Failed || failure != null, failure);
            callbackBlueprint = null; callbackComponent = null;
        }
    }

    private static object[] ExportScriptComponents(Instance instance) => instance.LogicComponents.Count == 0 ?
        Array.Empty<object>() : instance.LogicComponents.Select(c =>
        (object)new Dictionary<string, object> { ["id"] = c.Value.Id, ["class"] = c.Value.GetType().FullName!,
            ["enabled"] = c.Value.Enabled, ["failed"] = c.Value.Failed, ["started"] = c.Started,
            ["generation"] = c.Value.Handle.Generation, ["authoredProperties"] = c.Node.GetProperty("properties"),
            // 自定义运行时数据可以脱离配置形状；逐帧快照只复制其作者值，实际图仅在显式迁移时遍历。
            ["properties"] = Properties(c.Value.GetType()).ToDictionary(p => p.Name, p =>
                Composite(Schema(p.PropertyType)) ? (object)c.Configuration.GetProperty(p.Name) :
                    PropertyValue(p.GetValue(c.Value), p.PropertyType, c.Value.GetType().FullName + "." + p.Name)) }).ToArray();

    private static void DrainScriptComponentCommands(Context context)
    {
        // 生命周期回调新排队的结构变更留给下一边界，避免枚举过程中修改组件集合。
        var commands = context.LogicComponentCommands.ToArray(); context.LogicComponentCommands.Clear();
        foreach (var command in commands)
        {
            var instance = command.Owner;
            if (instance.Failed || instance.Blueprint.PendingDestroy) continue;
            instance.Dirty = true;
            context.DetailsDirty = true;
            if (command.Kind == "remove")
            {
                var component = instance.LogicComponents.SingleOrDefault(c => c.Value.Handle == command.Handle);
                if (component == null) continue;
                if (component.Started) Callback(context, instance, "EndPlay", 0, EndPlayReason.Stopped, scriptComponent: component.Value);
                context.Services.RemoveComponent(component.Value);
                instance.LogicComponents.Remove(component);
                instance.ComponentMetrics.Remove(component.Value.Id);
            }
            else
            {
                var component = NewScriptComponent(context, instance, command.Node, command.Handle);
                instance.LogicComponents.Add(component); PublishScriptComponents(instance);
                Callback(context, instance, "OnCreate", 0, EndPlayReason.Stopped, custom: component.Value.Create,
                    scriptComponent: component.Value);
            }
            PublishScriptComponents(instance);
        }
    }
}
