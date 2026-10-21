using System.Numerics;
using System.Reflection;
using System.Text.Json;

namespace VulkanLearn;

// 只迁移显式值属性，不保留委托、对象图或上一代加载上下文。
[AttributeUsage(AttributeTargets.Property)]
public sealed class MigrateAttribute : Attribute { }

public abstract partial class Blueprint
{
    protected virtual void OnReload() { }
    internal void Reload() => OnReload();
}

public static partial class NativeBridge
{
    private static PropertyInfo[] MigrationProperties(Type type) => Accessors(type).Migrated;

    private static object CaptureProperties(object value) => MigrationProperties(value.GetType()).Select(p =>
    {
        var schema = PropertySchema(p.PropertyType, value.GetType().FullName + "." + p.Name);
        return new { name = p.Name, type = p.PropertyType.FullName,
            value = PropertyValue(p.GetValue(value), p.PropertyType, value.GetType().FullName + "." + p.Name),
            schema = DataMigrationSchema(schema) };
    }).ToArray();

    private static void RestoreProperties(object target, JsonElement fields)
    {
        var properties = MigrationProperties(target.GetType()).ToDictionary(p => p.Name, StringComparer.Ordinal);
        foreach (var field in fields.EnumerateArray())
        {
            string name = field.GetProperty("name").GetString()!;
            if (!properties.TryGetValue(name, out var property) ||
                property.PropertyType.FullName != field.GetProperty("type").GetString())
                throw new InvalidOperationException($"Migrated property was removed or changed type: {target.GetType().FullName}.{name}");
            var schema = Schema(property.PropertyType);
            var path = target.GetType().FullName + "." + name;
            ValidateDataMigrationSchema(schema, field.GetProperty("schema"), path);
            property.SetValue(target, ReadData(schema, field.GetProperty("value"), property.GetValue(target), path, new(), 0));
        }
    }

    private static object CaptureMigration(Context context)
    {
        // 边界后排队的结构请求和消息仍属于旧代，不能在换代时静默丢弃。
        if (context.Commands.Count != 0 || context.ComponentCommands.Count != 0 ||
            context.LogicComponentCommands.Count != 0 || context.Services.HasPendingEvents)
            throw new InvalidOperationException("Finish pending World structure commands and events before live reload.");
        return new
        {
            nextBlueprint = context.NextBlueprint,
            nextComponent = context.NextComponent,
            contacts = context.Contacts.OrderBy(p => p.Item1, StringComparer.Ordinal)
                .ThenBy(p => p.Item2, StringComparer.Ordinal).Select(p => new[] { p.Item1, p.Item2 }).ToArray(),
            triggers = context.Triggers.OrderBy(p => p.Item1, StringComparer.Ordinal)
                .ThenBy(p => p.Item2, StringComparer.Ordinal).Select(p => new[] { p.Item1, p.Item2 }).ToArray(),
            activeCamera = context.ActiveCamera == null ? null : new
                { owner = context.ActiveCamera.Owner.Handle.Id, component = context.ActiveCamera.Id },
            instances = context.Instances.Select(i => new
            {
                id = i.Blueprint.Handle.Id,
                type = i.Blueprint.GetType().FullName,
                i.Started,
                i.Failed,
                enabled = i.Blueprint.Enabled,
                destroyed = i.Blueprint.PendingDestroy,
                properties = CaptureProperties(i.Blueprint),
                springArms = i.Blueprint.Components.OfType<SpringArmComponent>().Select(CaptureSpringArmHistory).ToArray(),
                components = i.LogicComponents.Select(c => new
                {
                    id = c.Value.Handle.Id,
                    type = c.Value.GetType().FullName,
                    c.Started,
                    failed = c.Value.Failed,
                    enabled = c.Value.Enabled,
                    properties = CaptureProperties(c.Value)
                }).ToArray()
            }).ToArray()
        };
    }

    private static object? RestoreMigration(Context context, JsonElement snapshot)
    {
        var sources = snapshot.GetProperty("instances").EnumerateArray()
            .ToDictionary(n => n.GetProperty("id").GetString()!, StringComparer.Ordinal);
        if (sources.Count != context.Instances.Count)
            throw new InvalidOperationException("Migration instance identities differ.");
        context.NextBlueprint = snapshot.GetProperty("nextBlueprint").GetUInt64();
        context.NextComponent = Math.Max(context.NextComponent, snapshot.GetProperty("nextComponent").GetUInt64());
        var runtimeStates = snapshot.GetProperty("states").EnumerateArray()
            .ToDictionary(n => n.GetProperty("id").GetString()!, StringComparer.Ordinal);
        foreach (var instance in context.Instances)
        {
            var blueprint = instance.Blueprint;
            if (!sources.TryGetValue(blueprint.Handle.Id, out var source) ||
                source.GetProperty("type").GetString() != blueprint.GetType().FullName)
                throw new InvalidOperationException("Migration Blueprint identity or type differs.");
            RestoreRuntimeValues(instance, runtimeStates[blueprint.Handle.Id]);
            RestoreProperties(blueprint, source.GetProperty("properties"));
            foreach (var arm in source.GetProperty("springArms").EnumerateArray()) RestoreSpringArmHistory(blueprint, arm);
            instance.Started = source.GetProperty("Started").GetBoolean();
            instance.Failed = source.GetProperty("Failed").GetBoolean();
            blueprint.Enabled = source.GetProperty("enabled").GetBoolean();
            blueprint.PendingDestroy = source.GetProperty("destroyed").GetBoolean();
            var components = source.GetProperty("components").EnumerateArray()
                .ToDictionary(n => n.GetProperty("id").GetString()!, StringComparer.Ordinal);
            if (components.Count != instance.LogicComponents.Count)
                throw new InvalidOperationException("Migration component identities differ.");
            foreach (var component in instance.LogicComponents)
            {
                var value = component.Value;
                if (!components.TryGetValue(value.Handle.Id, out var node) ||
                    node.GetProperty("type").GetString() != value.GetType().FullName)
                    throw new InvalidOperationException("Migration component identity or type differs.");
                RestoreProperties(value, node.GetProperty("properties"));
                component.Started = node.GetProperty("Started").GetBoolean();
                value.Enabled = node.GetProperty("enabled").GetBoolean();
                value.Failed = node.GetProperty("failed").GetBoolean();
            }
        }
        // Jolt World 没有换代，托管的接触边沿基线也必须保留，避免已有重叠重复 Enter。
        context.Contacts = snapshot.GetProperty("contacts").EnumerateArray()
            .Select(pair => (pair[0].GetString()!, pair[1].GetString()!)).ToHashSet();
        context.Triggers = snapshot.GetProperty("triggers").EnumerateArray()
            .Select(pair => (pair[0].GetString()!, pair[1].GetString()!)).ToHashSet();
        context.ActiveCamera = null;
        if (snapshot.GetProperty("activeCamera") is { ValueKind: JsonValueKind.Object } camera)
        {
            var owner = context.Instances.Single(i => i.Blueprint.Handle.Id == camera.GetProperty("owner").GetString());
            context.ActiveCamera = owner.Blueprint.GetComponent<CameraComponent>(camera.GetProperty("component").GetString()!);
        }
        // 候选只允许重建 owner 内的服务。重载钩子的世界修改由原生侧导出比对拒绝。
        context.Playing = true;
        var restoredCamera = context.ActiveCamera;
        foreach (var instance in context.Instances)
        {
            if (!instance.Started || instance.Failed || instance.Blueprint.PendingDestroy) continue;
            callbackBlueprint = instance.Blueprint;
            long started = BeginCallbackTiming(instance, "OnReload");
            try { instance.Blueprint.Reload(); context.Services.Commit(); }
            catch (Exception error) { RecordCallbackException(instance, "OnReload", error); throw; }
            finally { EndCallbackTiming(instance, "OnReload", started); callbackBlueprint = null; }
            foreach (var component in instance.LogicComponents)
                if (component.Started && !component.Value.Failed)
                    ReloadScriptComponent(context, instance, component.Value);
            ValidateBlueprint(instance.Blueprint, instance.Blueprint.Transform.Scale);
        }
        if (context.Commands.Count != 0 || context.ComponentCommands.Count != 0 || context.LogicComponentCommands.Count != 0)
            throw new InvalidOperationException("OnReload cannot change World structure.");
        if (!ReferenceEquals(restoredCamera, context.ActiveCamera))
            throw new InvalidOperationException("OnReload cannot change the active camera.");
        return null;
    }

    private static void RestoreRuntimeValues(Instance instance, JsonElement state)
    {
        var blueprint = instance.Blueprint;
        ReadTransform(blueprint.Transform, state);
        var nodes = state.GetProperty("components").EnumerateArray()
            .ToDictionary(NodeId, StringComparer.Ordinal);
        foreach (var component in blueprint.Components)
        {
            var node = nodes[component.Id];
            ReadOptionalTransform(component.Transform, node);
            if (component is CameraComponent camera)
            {
                camera.HorizontalFov = node.GetProperty("fov").GetSingle();
                camera.NearClip = node.GetProperty("near_clip").GetSingle();
                camera.FarClip = node.GetProperty("far_clip").GetSingle();
                if (node.TryGetProperty("priority", out var priority)) camera.Priority = IntValue(priority);
            }
            if (component is SpringArmComponent arm) ReadSpringArmNode(arm, node);
            if (component is LightComponent light)
            {
                light.Color = Vector(node.GetProperty("color"));
                light.Intensity = node.GetProperty("intensity").GetSingle();
                light.Visible = node.GetProperty("visible").GetBoolean();
                if (light is SpotLightComponent spot)
                {
                    spot.InnerConeAngle = node.GetProperty("coneAngleInner").GetSingle();
                    spot.OuterConeAngle = node.GetProperty("coneAngleOuter").GetSingle();
                }
            }
        }
        if (blueprint.Body is { } body && state.TryGetProperty("bodyVelocity", out var velocity))
            body.Velocity = Vector(velocity);
        if (blueprint.Character is { } character)
        {
            if (state.TryGetProperty("grounded", out var grounded)) character.IsGrounded = grounded.GetBoolean();
            if (state.TryGetProperty("walkVelocity", out var walk)) character.WalkVelocity = Vector(walk);
        }
    }

    private static object MigrationExport(Context context)
    {
        if (context.Commands.Count != 0 || context.ComponentCommands.Count != 0 || context.LogicComponentCommands.Count != 0)
            throw new InvalidOperationException("OnReload cannot change World structure.");
        return Export(context);
    }
}
