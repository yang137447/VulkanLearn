using System.Numerics;
using System.Text.Json;

namespace VulkanLearn;

public readonly record struct ComponentHandle(ulong Context, string BlueprintId, string Id, ulong Generation);

public sealed class ComponentCollision
{
    public string Shape { get; init; } = "box";
    public Vector3 HalfExtents { get; init; } = new(50);
    public float Radius { get; init; } = 50;
    public float HalfHeight { get; init; } = 50;
}

// 描述只含作者值。Mesh 必须已经由当前 World/Spawn 模板正式加载，不在回调里导入资产。
public sealed class ComponentDescription
{
    public string Id { get; init; } = "";
    public string Kind { get; init; } = "scene";
    public string Parent { get; init; } = "";
    public Vector3 Position { get; init; }
    public Vector3 Rotation { get; init; }
    public Vector3 Scale { get; init; } = Vector3.One;
    public AssetId Mesh { get; init; }
    public AssetId Material { get; init; }
    public float HorizontalFov { get; init; } = 60;
    public float NearClip { get; init; } = 1;
    public float FarClip { get; init; } = 10000;
    public Vector3 Color { get; init; } = Vector3.One;
    public float Intensity { get; init; } = 1000;
    public float InnerConeAngle { get; init; } = 30;
    public float OuterConeAngle { get; init; } = 45;
    public ComponentCollision? Collision { get; init; }
}

public sealed class MeshComponent : SceneComponent
{
    public AssetId Mesh { get; internal set; }
    public AssetId Material { get; internal set; }
}

public abstract partial class Blueprint
{
    internal Func<ComponentDescription, ComponentHandle> AddComponentRequest = null!;
    internal Action<ComponentHandle> RemoveComponentRequest = null!;
    internal Action<ComponentHandle, ComponentHandle?> AttachComponentRequest = null!;
    internal Func<ComponentHandle, SceneComponent?> ResolveComponentQuery = null!;
    public ComponentHandle AddComponent(ComponentDescription description) => AddComponentRequest(description);
    public void RemoveComponent(ComponentHandle component) => RemoveComponentRequest(component);
    public void AttachComponent(ComponentHandle component, ComponentHandle? parent = null) =>
        AttachComponentRequest(component, parent);
    public SceneComponent? ResolveComponent(ComponentHandle component) => ResolveComponentQuery(component);
}

public static partial class NativeBridge
{
    private static string NodeId(JsonElement node) => node.TryGetProperty("id", out var id) ? id.GetString()! : "root";
    private sealed record ComponentCommand(string Kind, Blueprint Owner, ComponentHandle Handle,
        JsonElement Node, ComponentHandle? Parent);

    private static void BindComponentIdentity(Context context, Blueprint owner, SceneComponent component)
    {
        component.Handle = new(context.Id, owner.Handle.Id, component.Id, ++context.NextComponent);
        component.AliveQuery = () => !context.Released && !owner.PendingDestroy &&
            owner.Components.Any(c => ReferenceEquals(c, component));
        component.Transform.CheckAccess = () =>
        {
            // Destroy 回调的末尾仍需验证/导出已冻结值；移除组件和已释放上下文永远不能再访问。
            if (context.Released || !owner.Components.Any(c => ReferenceEquals(c, component)))
                throw new InvalidOperationException("Stale component handle.");
        };
    }

    private static JsonElement ComponentNode(ComponentDescription description)
    {
        if (string.IsNullOrEmpty(description.Id) || description.Id.IndexOfAny(['/', '\\', '|', '\0']) >= 0)
            throw new ArgumentException("Components require a stable id without separators.");
        var node = new Dictionary<string, object> { ["id"] = description.Id, ["type"] = description.Kind,
            ["parent"] = description.Parent, ["position"] = Values(description.Position),
            ["rotation"] = Values(description.Rotation), ["scale"] = Values(description.Scale) };
        if (description.Kind == "mesh")
        {
            node["modelPath"] = description.Mesh.Path ?? "";
            if (!string.IsNullOrEmpty(description.Material.Path)) node["materialOverride"] = description.Material.Path;
        }
        else if (description.Kind == "camera")
        {
            node["fov"] = description.HorizontalFov; node["near_clip"] = description.NearClip;
            node["far_clip"] = description.FarClip;
        }
        else if (description.Kind is "pointLight" or "spotLight" or "directionalLight")
        {
            node["intensityUnit"] = description.Kind == "directionalLight" ? "lux" : "cd";
            node["color"] = Values(description.Color); node["intensity"] = description.Intensity;
            if (description.Kind == "spotLight")
            { node["coneAngleInner"] = description.InnerConeAngle; node["coneAngleOuter"] = description.OuterConeAngle; }
        }
        else if (description.Kind != "scene") throw new ArgumentException("Unsupported component kind.");
        if (description.Collision is { } collision)
        {
            var primitive = new Dictionary<string, object> { ["shape"] = collision.Shape };
            if (collision.Shape == "box")
            {
                if (!Finite(collision.HalfExtents) || collision.HalfExtents.X <= 0 ||
                    collision.HalfExtents.Y <= 0 || collision.HalfExtents.Z <= 0)
                    throw new ArgumentException("Collision box requires positive finite half extents.");
                primitive["halfExtents"] = Values(collision.HalfExtents);
            }
            else if (collision.Shape is "sphere" or "capsule")
            {
                if (!float.IsFinite(collision.Radius) || collision.Radius <= 0 ||
                    collision.Shape == "capsule" && (!float.IsFinite(collision.HalfHeight) || collision.HalfHeight <= 0))
                    throw new ArgumentException("Collision requires positive finite dimensions.");
                primitive["radius"] = collision.Radius;
                if (collision.Shape == "capsule") primitive["halfHeight"] = collision.HalfHeight;
            }
            else throw new ArgumentException("Component collision requires primitive shape.");
            node["collision"] = primitive;
        }
        return JsonSerializer.SerializeToElement(node);
    }

    private static void ValidateComponentTree(IEnumerable<JsonElement> nodes)
    {
        var parents = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var node in nodes)
        {
            string id = NodeId(node);
            if (!parents.TryAdd(id, node.TryGetProperty("parent", out var parent) ? parent.GetString()! : ""))
                throw new ArgumentException("Duplicate component id.");
        }
        if (parents.Count > 128) throw new InvalidOperationException("Component budget exceeded.");
        foreach (var id in parents.Keys)
        {
            var visited = new HashSet<string>(StringComparer.Ordinal);
            string current = id;
            while (current != "")
            {
                if (!parents.TryGetValue(current, out var parent) || !visited.Add(current))
                    throw new ArgumentException("Component parent missing or cyclic.");
                current = parent;
            }
        }
    }

    private static SceneComponent DynamicComponent(Context context, Blueprint owner, JsonElement node,
        ComponentHandle handle)
    {
        string kind = node.GetProperty("type").GetString()!;
        SceneComponent component = kind switch
        {
            "camera" => new CameraComponent(), "pointLight" => new PointLightComponent(),
            "spotLight" => new SpotLightComponent(), "directionalLight" => new DirectionalLightComponent(),
            "mesh" => new MeshComponent { Mesh = new(node.GetProperty("modelPath").GetString()!),
                Material = new(node.TryGetProperty("materialOverride", out var material) ? material.GetString()! : "") },
            _ => new SceneComponent()
        };
        component.Id = handle.Id; component.Kind = kind; component.Owner = owner;
        ReadOptionalTransform(component.Transform, node);
        if (component is CameraComponent camera)
        {
            camera.HorizontalFov = node.GetProperty("fov").GetSingle();
            camera.NearClip = node.GetProperty("near_clip").GetSingle(); camera.FarClip = node.GetProperty("far_clip").GetSingle();
            camera.Activation = active =>
            {
                RequireCallback(context, owner);
                if (!component.IsAlive) throw new InvalidOperationException("Stale component handle.");
                if (active) SetActiveCamera(context, camera);
                else if (context.ActiveCamera == camera) SetActiveCamera(context, null);
            };
            camera.ActiveQuery = () => { RequireCallback(context, owner); return component.IsAlive && context.ActiveCamera == camera; };
        }
        if (component is LightComponent light)
        {
            light.Color = Vector(node.GetProperty("color")); light.Intensity = node.GetProperty("intensity").GetSingle();
            if (light is SpotLightComponent spot)
            { spot.InnerConeAngle = node.GetProperty("coneAngleInner").GetSingle(); spot.OuterConeAngle = node.GetProperty("coneAngleOuter").GetSingle(); }
        }
        ValidateTransform(component.Transform);
        BindComponentIdentity(context, owner, component);
        component.Handle = handle;
        return component;
    }

    private static void RequireComponent(Context context, Instance instance, ComponentHandle handle)
    {
        var owner = instance.Blueprint;
        RequireCallback(context, owner);
        var pendingCreation = context.ComponentCommands.LastOrDefault(c =>
            c.Owner == owner && c.Handle.Id == handle.Id && c.Kind == "add");
        var currentHandle = pendingCreation?.Handle ?? owner.Components.SingleOrDefault(c => c.Id == handle.Id)?.Handle;
        if (handle.Context != context.Id || handle.BlueprintId != owner.Handle.Id || currentHandle != handle ||
            !PendingComponentNodes(context, instance).Any(n => NodeId(n) == handle.Id))
            throw new InvalidOperationException("Stale or foreign component handle.");
    }

    private static void BindComponentOperations(Context context, Instance instance)
    {
        var owner = instance.Blueprint;
        owner.ResolveComponentQuery = handle =>
        {
            RequireCallback(context, owner);
            if (handle.Context != context.Id || handle.BlueprintId != owner.Handle.Id)
                throw new InvalidOperationException("Stale or foreign component context.");
            return owner.Components.SingleOrDefault(c => c.Handle == handle);
        };
        owner.AddComponentRequest = description =>
        {
            RequireCallback(context, owner);
            if (context.ComponentCommands.Count >= 256) throw new InvalidOperationException("Component command budget exceeded.");
            var node = ComponentNode(description);
            if (description.Kind == "mesh" && !context.LoadedMeshes.Contains(
                (description.Mesh.Path ?? "", description.Material.Path ?? "")) && !context.Templates.Values
                .SelectMany(t => t.TryGetProperty("components", out var nodes) ? nodes.EnumerateArray().ToArray() : [])
                .Any(n => n.GetProperty("type").GetString() == "mesh" &&
                    n.GetProperty("modelPath").GetString() == description.Mesh.Path &&
                    (n.TryGetProperty("materialOverride", out var material) ? material.GetString() : "") ==
                    (description.Material.Path ?? "")))
                throw new ArgumentException("Mesh/material combination was not preloaded.");
            var candidate = DynamicComponent(context, owner, node,
                new(context.Id, owner.Handle.Id, description.Id, ++context.NextComponent));
            // 先在值节点上验证全树，错误仍属于当前回调并可撤销整批请求。
            var pending = PendingComponentNodes(context, instance);
            ValidateComponentTree(pending.Append(node));
            ValidateDynamicComponent(candidate);
            context.ComponentCommands.Add(new("add", owner, candidate.Handle, node, null));
            return candidate.Handle;
        };
        owner.RemoveComponentRequest = handle =>
        {
            RequireComponent(context, instance, handle);
            if (context.ComponentCommands.Count >= 256) throw new InvalidOperationException("Component command budget exceeded.");
            context.ComponentCommands.Add(new("remove", owner, handle, default, null));
        };
        owner.AttachComponentRequest = (handle, parent) =>
        {
            RequireComponent(context, instance, handle);
            if (parent != null) RequireComponent(context, instance, parent.Value);
            if (context.ComponentCommands.Count >= 256) throw new InvalidOperationException("Component command budget exceeded.");
            var command = new ComponentCommand("attach", owner, handle, default, parent);
            var pending = PendingComponentNodes(context, instance);
            ValidateComponentTree(ApplyComponentNodes(pending, command));
            context.ComponentCommands.Add(command);
        };
    }

    private static void ValidateDynamicComponent(SceneComponent component)
    {
        if (component is CameraComponent camera &&
            (!float.IsFinite(camera.HorizontalFov) || !float.IsFinite(camera.NearClip) || !float.IsFinite(camera.FarClip) ||
                camera.HorizontalFov <= 0 || camera.HorizontalFov >= 179 || camera.NearClip <= 0 || camera.FarClip <= camera.NearClip))
            throw new ArgumentException("Invalid camera component projection.");
        if (component is LightComponent light &&
            (!Finite(light.Color) || light.Color.X < 0 || light.Color.Y < 0 || light.Color.Z < 0 ||
                light.Color.X > 1 || light.Color.Y > 1 || light.Color.Z > 1 || !float.IsFinite(light.Intensity) ||
                light.Intensity < 0 || light.Intensity > 1e12f))
            throw new ArgumentException("Invalid light component.");
        if (component is SpotLightComponent spot && (!float.IsFinite(spot.InnerConeAngle) ||
            !float.IsFinite(spot.OuterConeAngle) || spot.InnerConeAngle < 0 ||
            spot.InnerConeAngle >= spot.OuterConeAngle || spot.OuterConeAngle >= 90))
            throw new ArgumentException("Invalid spot cone.");
    }

    private static JsonElement[] PendingComponentNodes(Context context, Instance instance)
    {
        var nodes = instance.ComponentNodes.Select(node =>
        {
            var component = instance.Blueprint.Components.Single(c => c.Id == NodeId(node));
            var values = node.EnumerateObject().ToDictionary(p => p.Name, p => (object)p.Value);
            values["position"] = Values(component.Transform.Position); values["rotation"] = Values(component.Transform.Rotation);
            values["scale"] = Values(component.Transform.Scale);
            return JsonSerializer.SerializeToElement(values);
        }).ToArray();
        foreach (var command in context.ComponentCommands.Where(c => c.Owner == instance.Blueprint))
            nodes = ApplyComponentNodes(nodes, command);
        return nodes;
    }

    private static void ValidateComponentPhysics(Instance instance, JsonElement[] nodes)
    {
        bool collisions = nodes.Any(n => n.TryGetProperty("collision", out _));
        if (!instance.Declaration.TryGetProperty("physics", out var physics))
        {
            if (collisions) throw new ArgumentException("Component collision requires compound root body.");
            return;
        }
        string shape = physics.TryGetProperty("shape", out var shapeNode) ? shapeNode.GetString()! : "box";
        if (collisions && shape != "compound") throw new ArgumentException("Component collision requires compound root body.");
        if (shape is not ("compound" or "mesh")) return;
        var byId = nodes.ToDictionary(NodeId, StringComparer.Ordinal);
        string[] colliderIds = shape == "mesh" ? [physics.GetProperty("component").GetString()!] :
            physics.TryGetProperty("shapes", out var shapes) ? shapes.EnumerateArray()
                .Select(s => s.GetProperty("component").GetString()!).ToArray() :
            nodes.Where(n => n.TryGetProperty("collision", out _)).Select(NodeId).ToArray();
        if (colliderIds.Length == 0) throw new ArgumentException("Compound root requires collider components.");
        foreach (string id in colliderIds)
        {
            if (!byId.TryGetValue(id, out var node) || shape == "mesh" && node.GetProperty("type").GetString() != "mesh")
                throw new ArgumentException("Cannot remove a root collision component.");
            if (shape == "mesh") continue;
            Matrix4x4 matrix = Matrix4x4.Identity;
            string current = id;
            while (current != "")
            {
                var transform = new Transform();
                node = byId[current]; ReadOptionalTransform(transform, node);
                matrix *= transform.Matrix;
                current = node.TryGetProperty("parent", out var parent) ? parent.GetString()! : "";
            }
            Vector3 x = new(matrix.M11, matrix.M12, matrix.M13), y = new(matrix.M21, matrix.M22, matrix.M23),
                z = new(matrix.M31, matrix.M32, matrix.M33);
            float scale = x.Length();
            float determinant = matrix.GetDeterminant();
            if (!float.IsFinite(scale) || scale <= 0 || !float.IsFinite(determinant) || determinant <= 0 ||
                !Finite(new(matrix.M41, matrix.M42, matrix.M43)) || MathF.Abs(y.Length() / scale - 1) > 1e-4f ||
                MathF.Abs(z.Length() / scale - 1) > 1e-4f || MathF.Abs(Vector3.Dot(x, y)) > scale * scale * 1e-4f ||
                MathF.Abs(Vector3.Dot(x, z)) > scale * scale * 1e-4f || MathF.Abs(Vector3.Dot(y, z)) > scale * scale * 1e-4f)
                throw new ArgumentException("Collider components require positive uniform scale without shear.");
        }
    }

    private static JsonElement[] ApplyComponentNodes(JsonElement[] source, ComponentCommand command)
    {
        if (command.Kind == "add") return source.Append(command.Node).ToArray();
        if (command.Kind == "attach")
            return source.Select(n => NodeId(n) == command.Handle.Id
                ? WithComponentParent(n, command.Parent?.Id ?? "") : n).ToArray();
        var removed = new HashSet<string>(StringComparer.Ordinal) { command.Handle.Id };
        bool changed;
        do
        {
            changed = false;
            foreach (var node in source)
                if (node.TryGetProperty("parent", out var parent) && removed.Contains(parent.GetString()!) &&
                    removed.Add(NodeId(node))) changed = true;
        } while (changed);
        return source.Where(n => !removed.Contains(NodeId(n))).ToArray();
    }

    private static JsonElement WithComponentParent(JsonElement node, string parent)
    {
        var values = node.EnumerateObject().ToDictionary(p => p.Name, p => (object)p.Value);
        values["parent"] = parent;
        return JsonSerializer.SerializeToElement(values);
    }

    private static void DrainComponentCommands(Context context)
    {
        var commands = context.ComponentCommands.ToArray(); context.ComponentCommands.Clear();
        foreach (var group in commands.GroupBy(c => c.Owner))
        {
            var owner = group.Key;
            var instance = context.Instances.Single(i => i.Blueprint == owner);
            if (instance.Failed || owner.PendingDestroy) continue;
            instance.Dirty = true;
            context.DetailsDirty = true;
            foreach (var command in group)
            {
                instance.ComponentNodes = ApplyComponentNodes(instance.ComponentNodes, command);
                var components = owner.Components.ToList();
                if (command.Kind == "add")
                    components.Add(DynamicComponent(context, owner, command.Node, command.Handle));
                components.RemoveAll(c => !instance.ComponentNodes.Any(n => NodeId(n) == c.Id));
                owner.Components = components.AsReadOnly();
                foreach (var component in components)
                {
                    var node = instance.ComponentNodes.Single(n => NodeId(n) == component.Id);
                    component.Parent = node.TryGetProperty("parent", out var parent) && parent.GetString() != ""
                        ? components.Single(c => c.Id == parent.GetString()) : null;
                }
            }
            if (context.ActiveCamera != null && !context.ActiveCamera.IsAlive) SetActiveCamera(context, null);
        }
    }
}
