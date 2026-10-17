using System.Numerics;
using System.Text.Json;

namespace VulkanLearn;

public static partial class NativeBridge
{
    // 基线只保存最后一次已交付的值。先比较再分配，避免活跃实例每阶段构造完整字典与组件树。
    // 全量控制请求与首个 Spawn 仍导出完整记录；一次性命令始终独立于状态基线。
    private static object Export(Context context, bool delta = false)
    {
        List<object>? updates = null;
        foreach (var instance in context.Instances)
        {
            if (delta && !instance.Dirty) continue;
            instance.Dirty = false;
            bool complete = !delta || instance.LastExport == null;
            var baseline = instance.LastExport ??= new(StringComparer.Ordinal);
            Dictionary<string, object>? update = complete
                ? new(StringComparer.Ordinal) { ["id"] = instance.Blueprint.Handle.Id } : null;
            var blueprint = instance.Blueprint;
            ExportValue(baseline, ref update, blueprint.Handle.Id, "template", instance.Template, complete);
            ExportVector(baseline, ref update, blueprint.Handle.Id, "position", blueprint.Transform.Position, complete);
            ExportVector(baseline, ref update, blueprint.Handle.Id, "rotation", blueprint.Transform.Rotation, complete);
            ExportVector(baseline, ref update, blueprint.Handle.Id, "scale", blueprint.Transform.Scale, complete);
            ExportValue(baseline, ref update, blueprint.Handle.Id, "destroyed", blueprint.PendingDestroy, complete);
            ExportValue(baseline, ref update, blueprint.Handle.Id, "failed", instance.Failed, complete);
            ExportValue(baseline, ref update, blueprint.Handle.Id, "enabled", blueprint.Enabled, complete);
            if (complete || ComponentsChanged(context, instance))
            {
                ExportValue(baseline, ref update, blueprint.Handle.Id, "components",
                    ExportComponents(context, instance), complete);
                instance.ExportedComponentNodes = instance.ComponentNodes;
            }
            if (blueprint.Body is { } body)
            {
                if (body.VelocityChanged) AddUpdate(ref update, blueprint.Handle.Id, "velocity", Values(body.Velocity));
                if (body.Impulse != Vector3.Zero) AddUpdate(ref update, blueprint.Handle.Id, "impulse", Values(body.Impulse));
                body.VelocityChanged = false; body.Impulse = Vector3.Zero;
            }
            if (blueprint.Character is { } character)
            {
                ExportVector(baseline, ref update, blueprint.Handle.Id, "walkVelocity", character.WalkVelocity, complete);
                if (complete || character.JumpRequested)
                    AddUpdate(ref update, blueprint.Handle.Id, "jump", character.JumpRequested);
                character.JumpRequested = false;
            }
            ExportCommand(ref update, blueprint.Handle.Id, "raycasts", context.Services.TakeRays(blueprint), complete);
            ExportCommand(ref update, blueprint.Handle.Id, "assetRequests", context.Services.TakeAssets(blueprint), complete);
            ExportCommand(ref update, blueprint.Handle.Id, "assetCancels", context.Services.TakeAssetCancellations(blueprint), complete);
            ExportValue(baseline, ref update, blueprint.Handle.Id, "scriptComponents", ExportScriptComponents(instance), complete);
            ExportValue(baseline, ref update, blueprint.Handle.Id, "hud", context.Services.ExportHud(blueprint), complete);
            if (update != null) (updates ??= []).Add(update);
        }
        return updates?.ToArray() ?? Array.Empty<object>();
    }

    private static void AddUpdate(ref Dictionary<string, object>? update, string id, string field, object value)
    {
        update ??= new(StringComparer.Ordinal) { ["id"] = id };
        update[field] = value;
    }

    private static void ExportCommand(ref Dictionary<string, object>? update, string id, string field,
        Array values, bool complete)
    {
        if (complete || values.Length != 0) AddUpdate(ref update, id, field, values);
    }

    private static void ExportValue(Dictionary<string, object> baseline, ref Dictionary<string, object>? update,
        string id, string field, object value, bool complete)
    {
        if (!complete && baseline.TryGetValue(field, out var previous) && SameRuntimeExport(previous, value)) return;
        baseline[field] = value;
        AddUpdate(ref update, id, field, value);
    }

    private static bool SameVector(object before, Vector3 after) =>
        before is float[] values && values[0] == after.X && values[1] == after.Y && values[2] == after.Z;

    private static void SynchronizeVector(Dictionary<string, object> baseline, string field, Vector3 value)
    {
        // 物理响应已独占本次 GT 边界，之前的导出已序列化；原地推进私有基线无需再分配数组。
        var values = (float[])baseline[field];
        values[0] = value.X; values[1] = value.Y; values[2] = value.Z;
    }

    private static void ExportVector(Dictionary<string, object> baseline, ref Dictionary<string, object>? update,
        string id, string field, Vector3 value, bool complete)
    {
        if (!complete && baseline.TryGetValue(field, out var previous) && SameVector(previous, value)) return;
        var values = Values(value);
        baseline[field] = values;
        AddUpdate(ref update, id, field, values);
    }

    private static bool ComponentsChanged(Context context, Instance instance)
    {
        // 结构命令替换节点数组；同结构只检查运行时可写值，作者 JSON 不逐帧重新枚举。
        if (!ReferenceEquals(instance.ExportedComponentNodes, instance.ComponentNodes)) return true;
        var previous = (Dictionary<string, object>[])instance.LastExport!["components"];
        for (int index = 0; index < instance.Blueprint.Components.Count; ++index)
        {
            var component = instance.Blueprint.Components[index];
            var before = previous[index];
            if (!SameVector(before["position"], component.Transform.Position) ||
                !SameVector(before["rotation"], component.Transform.Rotation) ||
                !SameVector(before["scale"], component.Transform.Scale)) return true;
            if (component is CameraComponent camera &&
                (!Equals(before["fov"], camera.HorizontalFov) || !Equals(before["near_clip"], camera.NearClip) ||
                 !Equals(before["far_clip"], camera.FarClip) || !Equals(before["active"], context.ActiveCamera == camera)))
                return true;
            if (component is LightComponent light &&
                (!SameVector(before["color"], light.Color) || !Equals(before["intensity"], light.Intensity) ||
                 !Equals(before["visible"], light.Visible))) return true;
            if (component is SpotLightComponent spot &&
                (!Equals(before["coneAngleInner"], spot.InnerConeAngle) || !Equals(before["coneAngleOuter"], spot.OuterConeAngle)))
                return true;
        }
        return false;
    }

    private static Dictionary<string, object>[] ExportComponents(Context context, Instance instance)
    {
        var result = new Dictionary<string, object>[instance.ComponentNodes.Length];
        for (int index = 0; index < result.Length; ++index)
        {
            var node = instance.ComponentNodes[index];
            var component = instance.Blueprint.Components[index];
            var value = node.EnumerateObject().ToDictionary(p => p.Name, p => (object)p.Value);
            value["position"] = Values(component.Transform.Position);
            value["rotation"] = Values(component.Transform.Rotation);
            value["scale"] = Values(component.Transform.Scale);
            if (component is CameraComponent camera)
            {
                value["fov"] = camera.HorizontalFov; value["near_clip"] = camera.NearClip;
                value["far_clip"] = camera.FarClip; value["active"] = context.ActiveCamera == camera;
            }
            if (component is LightComponent light)
            {
                value["color"] = Values(light.Color); value["intensity"] = light.Intensity;
                value["visible"] = light.Visible;
                if (light is SpotLightComponent spot)
                { value["coneAngleInner"] = spot.InnerConeAngle; value["coneAngleOuter"] = spot.OuterConeAngle; }
            }
            result[index] = value;
        }
        return result;
    }
}
