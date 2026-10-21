using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text.Json;

namespace VulkanLearn;

// 唯一托管 ABI 是 UTF-8 请求/结果和固定宽度长度；所有异常在此转换为结构化错误。
public static partial class NativeBridge
{
    private sealed partial class Instance(Blueprint blueprint)
    {
        public Blueprint Blueprint = blueprint;
        public bool Started;
        public bool Failed;
        public string Template = "";
        public JsonElement Declaration;
        public JsonElement[] ComponentNodes = [];
        public bool Dirty = true;
        public Dictionary<string, object>? LastExport;
        public JsonElement[]? ExportedComponentNodes;
    }
    private sealed record Command(string Kind, string Id, string Path, Vector3 Position, Vector3 Rotation, Vector3 Scale);
    private sealed partial class Context
    {
        public readonly List<Instance> Instances = [];
        public readonly Dictionary<string, Instance> InstanceIds = new(StringComparer.Ordinal);
        public readonly Time Time = new();
        public readonly Input Input = new();
        public bool Playing;
        public ulong Id;
        public GameplayAssembly Assembly = null!;
        public ulong NextBlueprint;
        public ulong NextComponent;
        public bool Released;
        public bool DetailsDirty = true;
        public readonly List<ComponentCommand> ComponentCommands = [];
        public readonly List<LogicComponentCommand> LogicComponentCommands = [];
        public readonly HashSet<(string, string)> LoadedMeshes = [];
        public readonly Dictionary<string, string[]> AnimationCatalog = new(StringComparer.Ordinal);
        // animation_catalog 的 assets 段（curves/montages）；保留原始节点，绑定组件时再解析。
        public readonly Dictionary<string, JsonElement> AnimationAssets = new(StringComparer.Ordinal);
        public readonly Dictionary<string, JsonElement> Templates = new(StringComparer.Ordinal);
        public readonly List<Command> Commands = [];
        public HashSet<(string, string)> Contacts = [];
        public HashSet<(string, string)> Triggers = [];
        public CameraComponent? ActiveCamera;
        public GameplayScheduler Services;
        public Context() { Services = new(this); }
    }
    private static readonly Dictionary<ulong, Context> Contexts = [];
    private static int ownerThread;
    private static readonly List<string> Messages = [];
    private static readonly List<object> LogEntries = [];
    private static readonly List<object> Diagnostics = [];
    private const int AbiVersion = 1;
    private static Blueprint? callbackBlueprint;
    private static string callbackPhase = "";

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe int Invoke(byte* request, int length, byte* output, int capacity)
    {
        try
        {
            if (ownerThread == 0) ownerThread = Environment.CurrentManagedThreadId;
            if (Environment.CurrentManagedThreadId != ownerThread)
                throw new InvalidOperationException("Script callbacks require the game thread.");
            Messages.Clear();
            LogEntries.Clear();
            Diagnostics.Clear();
            Log.Messages = Messages;
            Log.Entries = LogEntries;
            using var document = JsonDocument.Parse(new ReadOnlySpan<byte>(request, length).ToArray());
            object? result = Dispatch(document.RootElement);
            bool delta = document.RootElement.TryGetProperty("delta", out var incremental) && incremental.GetBoolean();
            return Write(new { ok = true, result, delta, logs = Messages, logEntries = LogEntries, diagnostics = Diagnostics }, output, capacity);
        }
        catch (Exception error)
        {
            var failure = Unwrap(error);
            return Write(new { ok = false, error = failure.ToString(),
                exceptionType = failure.GetType().FullName, action = "RejectRequest" }, output, capacity);
        }
        finally { Log.Messages = null; Log.Entries = null; }
    }

    private static unsafe int Write(object result, byte* output, int capacity)
    {
        byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(result);
        // 不重放有副作用的请求：超出有界结果区时返回错误，由原生宿主停止本上下文。
        if (bytes.Length >= capacity) return -1;
        bytes.CopyTo(new Span<byte>(output, capacity));
        output[bytes.Length] = 0;
        return bytes.Length;
    }

    private static object? Dispatch(JsonElement request)
    {
        if (request.GetProperty("abi").GetInt32() != AbiVersion)
            throw new InvalidOperationException("Script ABI mismatch.");
        string operation = request.GetProperty("op").GetString()!;
        if (operation == "initialize")
            return LoadGameplay(request.GetProperty("assembly").GetString()!);
        if (operation == "unload")
        { UnloadGameplay(request.GetProperty("generation").GetUInt64()); return null; }
        if (operation == "assembly_status")
            return AssemblyStatus(request.TryGetProperty("collect", out var collect) && collect.GetBoolean());
        ulong id = request.GetProperty("context").GetUInt64();
        if (operation == "prepare")
        {
            if (id == 0 || Contexts.ContainsKey(id)) throw new InvalidOperationException("Invalid context identity.");
            var candidate = new Context { Id = id,
                Assembly = Assemblies[request.GetProperty("generation").GetUInt64()] };
            try
            {
                if (request.TryGetProperty("animationCatalog", out var animationCatalog))
                    foreach (var entry in animationCatalog.EnumerateObject())
                        candidate.AnimationCatalog.Add(entry.Name,
                            entry.Value.EnumerateArray().Select(c => c.GetString()!).ToArray());
                if (request.TryGetProperty("animationAssets", out var animationAssets) &&
                    animationAssets.ValueKind == JsonValueKind.Object)
                    foreach (var entry in animationAssets.EnumerateObject())
                        candidate.AnimationAssets[entry.Name] = entry.Value.Clone();
                if (request.TryGetProperty("templates", out var templates))
                    foreach (var template in templates.EnumerateObject())
                        candidate.Templates.Add(template.Name, template.Value.Clone());
                foreach (var node in request.GetProperty("blueprints").EnumerateArray())
                    AddInstance(candidate, CreateInstance(candidate, node));
                Contexts.Add(id, candidate);
            }
            catch { candidate.Instances.Clear(); candidate.InstanceIds.Clear(); throw; }
            return null;
        }
        if (!Contexts.TryGetValue(id, out var context))
            throw new InvalidOperationException($"Stale script context: {id}");
        switch (operation)
        {
            case "animation_catalog": UpdateAnimationCatalog(context, request.GetProperty("catalog"),
                request.TryGetProperty("assets", out var assets) ? assets : default); return null;
            case "migration_capture": return CaptureMigration(context);
            case "migration_restore": return RestoreMigration(context, request.GetProperty("snapshot"));
            case "migration_export": return MigrationExport(context);
            case "set_time": ApplyContextTime(context, request.GetProperty("time")); return null;
            case "runtime_snapshot": return RuntimeSnapshot(context,
                !request.TryGetProperty("details", out var details) || details.GetBoolean());
            case "start":
                context.Playing = true;
                SelectDefaultCamera(context);
                foreach (var instance in context.Instances)
                {
                    instance.Started = instance.Blueprint.Enabled && !instance.Blueprint.PendingDestroy;
                    Callback(context, instance, "BeginPlay", 0, EndPlayReason.Stopped);
                    RunScriptComponents(context, instance, "BeginPlay", 0, EndPlayReason.Stopped);
                }
                DrainCommands(context);
                ResetSpringArmCameras(context);
                return Export(context, DeltaRequested(request));
            case "tick":
                if (!context.Playing) throw new InvalidOperationException("Context is not playing.");
                float dt = BeginFrameTime(context, request);
                if (request.TryGetProperty("animationFeedback", out var animationFeedback))
                    SynchronizeAnimations(context, animationFeedback);
                var input = request.GetProperty("input");
                context.Input.Movement = new(input[0].GetSingle(), input[1].GetSingle());
                context.Input.Vertical = input[2].GetSingle();
                context.Input.Actions.Clear(); context.Input.Axes.Clear();
                if (request.TryGetProperty("inputMap", out var map) && map.TryGetProperty("actions", out var actions))
                {
                    foreach (var action in actions.EnumerateObject())
                        context.Input.Actions[action.Name] = (action.Value.GetProperty("held").GetBoolean(),
                            action.Value.GetProperty("pressed").GetBoolean(), action.Value.GetProperty("released").GetBoolean());
                    foreach (var axis in map.GetProperty("axes").EnumerateObject())
                        context.Input.Axes[axis.Name] = axis.Value.GetSingle();
                }
                if (request.TryGetProperty("states", out var prior)) Synchronize(context, prior);
                if (request.TryGetProperty("assetUnloads", out var assetUnloads)) context.Services.ForgetAssets(assetUnloads);
                if (request.TryGetProperty("assetResults", out var assetResults)) context.Services.CompleteAssets(assetResults);
                if (context.Time.TimeScale > 0) context.Services.RunTimers();
                foreach (var instance in context.Instances)
                {
                    Callback(context, instance, "Tick", dt, EndPlayReason.Stopped);
                    RunScriptComponents(context, instance, "Tick", dt, EndPlayReason.Stopped);
                }
                DrainCommands(context);
                if (!request.TryGetProperty("deferLate", out var deferred) || !deferred.GetBoolean())
                {
                    RunLate(context, dt);
                    QueueSpringArmProbes(context);
                    FlushPendingSpringArms(context);
                }
                return Export(context, DeltaRequested(request));
            case "late":
                Synchronize(context, request.GetProperty("states"));
                var contacts = request.GetProperty("contacts").EnumerateArray()
                    .Select(pair => (pair[0].GetString()!, pair[1].GetString()!)).ToHashSet();
                foreach (var pair in contacts.Except(context.Contacts).OrderBy(p => p.Item1).ThenBy(p => p.Item2))
                    Contact(context, pair, true);
                foreach (var pair in context.Contacts.Except(contacts).OrderBy(p => p.Item1).ThenBy(p => p.Item2))
                    Contact(context, pair, false);
                context.Contacts = contacts;
                if (request.TryGetProperty("triggers", out var triggerNodes))
                {
                    var triggers = triggerNodes.EnumerateArray()
                        .Select(pair => (pair[0].GetString()!, pair[1].GetString()!)).ToHashSet();
                    foreach (var pair in triggers.Except(context.Triggers).OrderBy(p => p.Item1).ThenBy(p => p.Item2))
                        Contact(context, pair, true, true);
                    foreach (var pair in context.Triggers.Except(triggers).OrderBy(p => p.Item1).ThenBy(p => p.Item2))
                        Contact(context, pair, false, true);
                    context.Triggers = triggers;
                }
                if (request.TryGetProperty("rayHits", out var rayHits)) context.Services.CompleteRays(rayHits);
                RunLate(context, context.Time.DeltaTime);
                return Export(context, DeltaRequested(request));
            case "camera_prepare":
                Synchronize(context, request.GetProperty("states"));
                QueueSpringArmProbes(context);
                return Export(context, DeltaRequested(request));
            case "camera_finish":
                context.Services.CompleteRays(request.GetProperty("rayHits"));
                FlushPendingSpringArms(context);
                return Export(context, DeltaRequested(request));
            case "release":
                context.Services.Release();
                foreach (var instance in context.Instances.Where(i => i.Started))
                {
                    Callback(context, instance, "EndPlay", 0, (EndPlayReason)request.GetProperty("reason").GetInt32());
                    RunScriptComponents(context, instance, "EndPlay", 0, (EndPlayReason)request.GetProperty("reason").GetInt32());
                }
                Contexts.Remove(id);
                context.Released = true;
                context.InstanceIds.Clear();
                return null;
            default: throw new ArgumentException($"Unknown script operation: {operation}");
        }
    }

    private static void AddInstance(Context context, Instance instance)
    {
        // 列表保持作者/生成顺序；索引只负责稳定身份查询，二者在同一个 GT 边界登记。
        if (!context.InstanceIds.TryAdd(instance.Blueprint.Handle.Id, instance))
            throw new ArgumentException("Duplicate Blueprint identity.");
        context.Instances.Add(instance);
        context.DetailsDirty = true;
    }

    private static Instance CreateInstance(Context context, JsonElement node, string template = "",
        string? id = null, Vector3? position = null, Vector3? rotation = null, Vector3? scale = null)
    {
        string className = node.GetProperty("class").GetString()!;
        if (!TryResolveGameplayType(context.Assembly.Types, className, out Type? type))
            throw new InvalidOperationException($"Unknown Blueprint class: {className}");
        var blueprint = (Blueprint)Activator.CreateInstance(type)!;
        blueprint.Handle = new(context.Id, id ?? node.GetProperty("id").GetString()!);
        blueprint.Name = node.GetProperty("name").GetString()!;
        blueprint.Enabled = !node.TryGetProperty("enabled", out var enabled) || enabled.GetBoolean();
        blueprint.Input = context.Input; blueprint.Time = context.Time;
        ReadTransform(blueprint.Transform, node);
        if (position != null) blueprint.Transform.Position = position.Value;
        if (rotation != null) blueprint.Transform.Rotation = rotation.Value;
        if (scale != null) blueprint.Transform.Scale = scale.Value;
        blueprint.ControlRotation = blueprint.Transform.Rotation;
        var instance = new Instance(blueprint)
        {
            Template = template.Length == 0 && node.TryGetProperty("__spawnPath", out var spawnPath)
                ? spawnPath.GetString()! : template,
            Declaration = node.Clone()
        };
        if (node.TryGetProperty("components", out var nodes))
            instance.ComponentNodes = nodes.EnumerateArray().Select(n => n.Clone()).ToArray();
        foreach (var componentNode in instance.ComponentNodes.Where(_ => template.Length == 0))
            if (componentNode.GetProperty("type").GetString() == "mesh")
                context.LoadedMeshes.Add((componentNode.GetProperty("modelPath").GetString()!,
                    componentNode.TryGetProperty("materialOverride", out var material) ? material.GetString()! : ""));
        blueprint.Components = Array.AsReadOnly(instance.ComponentNodes.Select(n =>
        {
            SceneComponent component = n.GetProperty("type").GetString() switch
            {
                "camera" => new CameraComponent(), "pointLight" => new PointLightComponent(),
                "spotLight" => new SpotLightComponent(), "directionalLight" => new DirectionalLightComponent(),
                "springArm" => new SpringArmComponent(),
                "mesh" => new MeshComponent { Mesh = new(n.GetProperty("modelPath").GetString()!),
                    Material = new(n.TryGetProperty("materialOverride", out var material) ? material.GetString()! : "") },
                _ => new SceneComponent()
            };
            component.Id = n.TryGetProperty("id", out var componentId) ? componentId.GetString()! : "root";
            component.Kind = n.GetProperty("type").GetString()!; component.Owner = blueprint;
            if (component is CameraComponent camera)
            {
                if (n.TryGetProperty("fov", out var fov)) camera.HorizontalFov = fov.GetSingle();
                if (n.TryGetProperty("near_clip", out var near)) camera.NearClip = near.GetSingle();
                if (n.TryGetProperty("far_clip", out var far)) camera.FarClip = far.GetSingle();
                if (n.TryGetProperty("priority", out var priority)) camera.Priority = IntValue(priority);
                camera.Activation = active =>
                {
                    RequireCallback(context, blueprint);
                    if (!component.IsAlive) throw new InvalidOperationException("Stale component handle.");
                    if (active) SetActiveCamera(context, camera);
                    else if (context.ActiveCamera == camera) SetActiveCamera(context, null);
                };
                camera.ActiveQuery = () => { RequireCallback(context, blueprint); return component.IsAlive && context.ActiveCamera == camera; };
            }
            if (component is LightComponent light)
            {
                if (n.TryGetProperty("color", out var color)) light.Color = Vector(color);
                if (n.TryGetProperty("intensity", out var intensity)) light.Intensity = intensity.GetSingle();
                if (n.TryGetProperty("visible", out var visible)) light.Visible = visible.GetBoolean();
                if (light is SpotLightComponent spot)
                {
                    if (n.TryGetProperty("coneAngleInner", out var inner)) spot.InnerConeAngle = inner.GetSingle();
                    if (n.TryGetProperty("coneAngleOuter", out var outer)) spot.OuterConeAngle = outer.GetSingle();
                }
            }
            if (component is SpringArmComponent arm) ReadSpringArmNode(arm, n);
            ReadOptionalTransform(component.Transform, n);
            BindComponentIdentity(context, blueprint, component);
            return component;
        }).ToArray());
        for (int index = 0; index < blueprint.Components.Count; index++)
            if (instance.ComponentNodes[index].TryGetProperty("parent", out var parent) && parent.GetString() != "")
                blueprint.Components[index].Parent = blueprint.GetComponent(parent.GetString()!);
        if (node.TryGetProperty("physics", out var physics))
        {
            string motion = physics.TryGetProperty("motion", out var motionNode) ? motionNode.GetString()! : "static";
            if (motion == "character") blueprint.Character = new();
            else blueprint.Body = new() { Motion = motion };
        }
        blueprint.World = new()
        {
            SpawnRequest = (path, p, r, s) => QueueSpawn(context, blueprint, path, p, r, s),
            DestroyRequest = handle => QueueDestroy(context, blueprint, handle),
            AliveQuery = handle => { RequireCallback(context, blueprint); return handle.Context == context.Id &&
                context.InstanceIds.TryGetValue(handle.Id, out var target) && !target.Blueprint.PendingDestroy; }
        };
        BindWorldQueries(context, blueprint);
        BindComponentOperations(context, instance);
        context.Services.Bind(blueprint);
        context.Services.BindHud(blueprint);
        context.Services.BindDebug(blueprint);
        BindScriptComponents(context, instance, node);
        BindExposedProperties(blueprint, node.GetProperty("properties"));
        // OnCreate 不能结构性修改 World；创建失败只丢弃候选或该次 Spawn。
        Vector3 initialScale = blueprint.Transform.Scale;
        RunCreate(context, instance, blueprint.Create);
        CreateScriptComponents(context, instance);
        ValidateBlueprint(blueprint, initialScale);
        return instance;
    }

    private static void RequireCallback(Context context, Blueprint owner)
    {
        if (Environment.CurrentManagedThreadId != ownerThread || !context.Playing ||
            callbackBlueprint != owner || owner.Handle.Context != context.Id ||
            callbackPhase == "OnCreate" ||
            !Contexts.TryGetValue(context.Id, out var current) || current != context)
            throw new InvalidOperationException("BlueprintWorld requires a live owning game-thread callback.");
    }
    private static BlueprintHandle QueueSpawn(Context context, Blueprint owner, string path, Vector3 position,
        Vector3? requestedRotation, Vector3? requestedScale)
    {
        RequireCallback(context, owner);
        if (!context.Templates.TryGetValue(path, out var template) || !context.Services.CanSpawnAsset(path))
            throw new ArgumentException($"Spawn asset requires a preloaded template or an active asset lease: {path}");
        Vector3 rotation = requestedRotation ?? Vector(template.GetProperty("rotation"));
        Vector3 scale = requestedScale ?? Vector(template.GetProperty("scale"));
        ValidateTransform(new() { Position = position, Rotation = rotation, Scale = scale });
        if (context.Commands.Count >= 256 || context.Instances.Count + context.Commands.Count >= 4096)
            throw new InvalidOperationException("Blueprint spawn budget exceeded.");
        string id;
        do { id = $"~spawn.{++context.NextBlueprint}"; } while (context.Instances.Any(i => i.Blueprint.Handle.Id == id));
        context.Commands.Add(new("spawn", id, path, position, rotation, scale));
        return new(context.Id, id);
    }
    private static void QueueDestroy(Context context, Blueprint owner, BlueprintHandle handle)
    {
        RequireCallback(context, owner);
        if (handle.Context != context.Id || !context.Instances.Any(i => i.Blueprint.Handle == handle) &&
            !context.Commands.Any(c => c.Kind == "spawn" && c.Id == handle.Id))
            throw new InvalidOperationException("Stale Blueprint handle.");
        if (context.Commands.Count >= 256) throw new InvalidOperationException("Blueprint command budget exceeded.");
        context.Commands.Add(new("destroy", handle.Id, "", default, default, default));
    }
    private static void DrainCommands(Context context)
    {
        DrainComponentCommands(context);
        DrainScriptComponentCommands(context);
        var commands = context.Commands.ToArray();
        context.Commands.Clear();
        foreach (var command in commands)
        {
            if (command.Kind == "destroy")
            {
                var instance = context.Instances.SingleOrDefault(i => i.Blueprint.Handle.Id == command.Id);
                if (instance != null) { instance.Blueprint.PendingDestroy = true; instance.Dirty = true; }
                continue;
            }
            try
            {
                var instance = CreateInstance(context, context.Templates[command.Path], command.Path,
                    command.Id, command.Position, command.Rotation, command.Scale);
                AddInstance(context, instance);
                SelectDefaultCamera(context);
                instance.Started = instance.Blueprint.Enabled && !instance.Blueprint.PendingDestroy;
                Callback(context, instance, "BeginPlay", 0, EndPlayReason.Stopped);
                RunScriptComponents(context, instance, "BeginPlay", 0, EndPlayReason.Stopped);
            }
            catch (Exception error)
            {
                var template = context.Templates[command.Path];
                Diagnostics.Add(new { blueprint = command.Id, id = command.Id,
                    type = template.GetProperty("class").GetString(), context = context.Id,
                    callback = "Spawn", frame = context.Time.FrameIndex, component = "",
                    error = Unwrap(error).ToString(), exceptionType = Unwrap(error).GetType().FullName,
                    action = "RejectSpawn" });
            }
        }
    }
    private static void SelectDefaultCamera(Context context)
    {
        if (context.ActiveCamera != null) return;
        CameraComponent? best = null;
        int bestPriority = int.MinValue;
        foreach (var instance in context.Instances.Where(i => !i.Failed && i.Blueprint.Enabled && !i.Blueprint.PendingDestroy))
            for (int index = 0; index < instance.Blueprint.Components.Count; ++index)
                if (instance.Blueprint.Components[index] is CameraComponent camera &&
                    instance.ComponentNodes[index].TryGetProperty("autoActivate", out var automatic) && automatic.GetBoolean() &&
                    (best == null || camera.Priority > bestPriority))
                { best = camera; bestPriority = camera.Priority; }
        if (best != null) SetActiveCamera(context, best);
    }
    private static void SetActiveCamera(Context context, CameraComponent? camera)
    {
        // 活动镜头影响新旧两个 owner 的导出，即使旧 owner 没有帧回调也要撤销 active。
        if (context.ActiveCamera != null)
            context.InstanceIds[context.ActiveCamera.Owner.Handle.Id].Dirty = true;
        if (camera != null) context.InstanceIds[camera.Owner.Handle.Id].Dirty = true;
        context.ActiveCamera = camera;
    }
    private static void RunLate(Context context, float dt)
    {
        foreach (var instance in context.Instances)
        {
            Callback(context, instance, "LateTick", dt, EndPlayReason.Stopped);
            RunScriptComponents(context, instance, "LateTick", dt, EndPlayReason.Stopped);
        }
        context.Services.RunEvents();
        DrainCommands(context);
        foreach (var instance in context.Instances.Where(i => i.Blueprint.PendingDestroy && i.Started))
        {
            Callback(context, instance, "EndPlay", dt, EndPlayReason.Stopped);
            RunScriptComponents(context, instance, "EndPlay", dt, EndPlayReason.Stopped);
            instance.Started = false;
            context.Services.RemoveOwner(instance.Blueprint);
        }
    }
    private static void Synchronize(Context context, JsonElement states)
    {
        foreach (var state in states.EnumerateArray())
        {
            string id = state.GetProperty("id").GetString()!;
            if (!context.InstanceIds.TryGetValue(id, out var instance))
                throw new ArgumentException("Unknown Blueprint state identity.");
            var blueprint = instance.Blueprint;
            ReadTransform(blueprint.Transform, state);
            if (instance.LastExport is { } baseline)
            {
                SynchronizeVector(baseline, "position", blueprint.Transform.Position);
                SynchronizeVector(baseline, "rotation", blueprint.Transform.Rotation);
                SynchronizeVector(baseline, "scale", blueprint.Transform.Scale);
            }
            if (blueprint.Body != null && state.TryGetProperty("bodyVelocity", out var velocity))
                blueprint.Body.Velocity = Vector(velocity);
            if (blueprint.Character != null && state.TryGetProperty("grounded", out var grounded))
                blueprint.Character.IsGrounded = grounded.GetBoolean();
            if (blueprint.Character != null && state.TryGetProperty("bodyVelocity", out var characterVelocity))
                blueprint.Character.LinearVelocity = Vector(characterVelocity);
        }
    }
    private static void Contact(Context context, (string, string) pair, bool enter, bool trigger = false)
    {
        foreach (var (id, other) in new[] { pair, (pair.Item2, pair.Item1) })
        {
            context.InstanceIds.TryGetValue(id, out var instance);
            if (instance != null) Callback(context, instance, trigger ?
                (enter ? "TriggerEnter" : "TriggerExit") : (enter ? "CollisionEnter" : "CollisionExit"),
                0, EndPlayReason.Stopped, new(context.Id, other));
        }
    }
    private static bool DeltaRequested(JsonElement request) =>
        request.TryGetProperty("delta", out var delta) && delta.GetBoolean();

    private static bool SameRuntimeExport(object? before, object? after)
    {
        if (ReferenceEquals(before, after)) return true;
        if (before is float[] floats && after is float[] values)
            return floats.AsSpan().SequenceEqual(values);
        if (before is Dictionary<string, object> left && after is Dictionary<string, object> right)
        {
            if (left.Count != right.Count) return false;
            foreach (var field in left)
                if (!right.TryGetValue(field.Key, out var value) || !SameRuntimeExport(field.Value, value)) return false;
            return true;
        }
        if (before is Array a && after is Array b)
        {
            if (a.Length != b.Length) return false;
            for (int index = 0; index < a.Length; ++index)
                if (!SameRuntimeExport(a.GetValue(index), b.GetValue(index))) return false;
            return true;
        }
        return Equals(before, after);
    }

    private static void Callback(Context context, Instance instance, string phase, float dt, EndPlayReason reason,
        BlueprintHandle other = default, Action? custom = null, Component? scriptComponent = null)
    {
        Blueprint blueprint = instance.Blueprint;
        if (phase != "EndPlay" && (instance.Failed || !blueprint.Enabled || blueprint.PendingDestroy)) return;
        if (custom == null && !HasFrameCallback(scriptComponent?.GetType() ?? blueprint.GetType(), phase)) return;
        instance.Dirty = true;
        Vector3 position = blueprint.Transform.Position, rotation = blueprint.Transform.Rotation, scale = blueprint.Transform.Scale;
        var controlRotation = blueprint.ControlRotation;
        var arms = blueprint.Components.OfType<SpringArmComponent>().Select(c => (c, c.CaptureSettings())).ToArray();
        var components = blueprint.Components.Count == 0 ? Array.Empty<(Vector3, Vector3, Vector3)>() :
            blueprint.Components.Select(c => (c.Transform.Position, c.Transform.Rotation, c.Transform.Scale)).ToArray();
        var cameras = blueprint.Components.Count == 0 ? Array.Empty<(CameraComponent, float, float, float, int)>() :
            blueprint.Components.OfType<CameraComponent>().Select(c => (c, c.HorizontalFov, c.NearClip, c.FarClip, c.Priority)).ToArray();
        var lights = blueprint.Components.Count == 0 ? Array.Empty<(LightComponent, Vector3, float, bool, float?, float?)>() :
            blueprint.Components.OfType<LightComponent>().Select(c =>
            (c, c.Color, c.Intensity, c.Visible, (c as SpotLightComponent)?.InnerConeAngle,
                (c as SpotLightComponent)?.OuterConeAngle)).ToArray();
        var activeCamera = context.ActiveCamera;
        var commands = context.Commands.Count;
        var componentCommands = context.ComponentCommands.Count;
        var logicCommands = context.LogicComponentCommands.Count;
        var enabled = blueprint.Enabled;
        var scriptEnabled = blueprint.ScriptComponents.Count == 0 ? Array.Empty<(Component Component, bool Enabled)>() :
            blueprint.ScriptComponents.Select(c => (Component: c, c.Enabled)).ToArray();
        var destroyed = blueprint.PendingDestroy;
        var velocity = blueprint.Body?.Velocity ?? default;
        var impulse = blueprint.Body?.Impulse ?? default;
        var velocityChanged = blueprint.Body?.VelocityChanged ?? false;
        var walkVelocity = blueprint.Character?.WalkVelocity ?? default;
        var rootMotionVelocity = blueprint.Character?.RootMotionVelocity ?? default;
        var rootMotionActive = blueprint.Character?.RootMotionActive ?? false;
        var jump = blueprint.Character?.JumpRequested ?? false;
        var animationCommands = blueprint.Components.OfType<MeshComponent>()
            .Select(c => (c.Animation, c.Animation.Pending)).ToArray();
        callbackBlueprint = blueprint;
        callbackComponent = scriptComponent;
        callbackPhase = phase;
        long callbackStart = scriptComponent == null ? BeginCallbackTiming(instance, phase) :
            System.Diagnostics.Stopwatch.GetTimestamp();
        Exception? callbackError = null;
        try
        {
            if (scriptComponent != null) InvokeScriptComponent(scriptComponent, phase, dt, reason, custom);
            else switch (phase)
            {
                case "BeginPlay": blueprint.Begin(); break;
                case "Tick": blueprint.Update(dt); break;
                case "LateTick": blueprint.UpdateLate(dt); break;
                case "EndPlay": blueprint.End(reason); break;
                case "CollisionEnter": blueprint.Collision(other, true); break;
                case "CollisionExit": blueprint.Collision(other, false); break;
                case "TriggerEnter": blueprint.Trigger(other, true); break;
                case "TriggerExit": blueprint.Trigger(other, false); break;
                default: custom?.Invoke(); break;
            }
            ValidateBlueprint(blueprint, scale);
            // primitive/无刚体的作者碰撞已验证；只有复合形状或新结构请求需要重建候选父链。
            bool complexPhysics = instance.Declaration.TryGetProperty("physics", out var physics) &&
                physics.TryGetProperty("shape", out var shape) && shape.GetString() is "compound" or "mesh";
            if (complexPhysics || context.ComponentCommands.Count != componentCommands)
                ValidateComponentPhysics(instance, PendingComponentNodes(context, instance));
            context.Services.Commit();
            if (blueprint.PendingDestroy || phase == "EndPlay")
            {
                if (scriptComponent == null || blueprint.PendingDestroy) context.Services.RemoveOwner(blueprint);
                else context.Services.RemoveComponent(scriptComponent);
            }
        }
        catch (Exception error)
        {
            callbackError = error;
            if (scriptComponent == null) RecordCallbackException(instance, phase, error);
            blueprint.Transform.Position = position; blueprint.Transform.Rotation = rotation; blueprint.Transform.Scale = scale;
            blueprint.ControlRotation = controlRotation;
            foreach (var (arm, settings) in arms) arm.RestoreSettings(settings);
            for (int index = 0; index < components.Length; index++)
            {
                var transform = blueprint.Components[index].Transform;
                (transform.Position, transform.Rotation, transform.Scale) = components[index];
            }
            blueprint.PendingDestroy = destroyed;
            blueprint.Enabled = enabled;
            foreach (var (component, wasEnabled) in scriptEnabled) component.RestoreEnabled(wasEnabled);
            foreach (var (camera, fov, near, far, priority) in cameras)
            { camera.HorizontalFov = fov; camera.NearClip = near; camera.FarClip = far; camera.Priority = priority; }
            foreach (var (light, color, intensity, visible, inner, outer) in lights)
            {
                light.Color = color; light.Intensity = intensity; light.Visible = visible;
                if (light is SpotLightComponent spot)
                { spot.InnerConeAngle = inner!.Value; spot.OuterConeAngle = outer!.Value; }
            }
            context.ActiveCamera = activeCamera;
            if (blueprint.Body is { } body) { body.Velocity = velocity; body.Impulse = impulse; body.VelocityChanged = velocityChanged; }
            if (blueprint.Character is { } character) { character.WalkVelocity = walkVelocity; character.RootMotionVelocity = rootMotionVelocity; character.RootMotionActive = rootMotionActive; character.JumpRequested = jump; }
            foreach (var (animation, pending) in animationCommands) animation.Pending = pending;
            context.Services.Rollback();
            if (scriptComponent == null) context.Services.RemoveOwner(blueprint);
            else context.Services.RemoveComponent(scriptComponent);
            context.Commands.RemoveRange(commands, context.Commands.Count - commands);
            context.ComponentCommands.RemoveRange(componentCommands, context.ComponentCommands.Count - componentCommands);
            context.LogicComponentCommands.RemoveRange(logicCommands, context.LogicComponentCommands.Count - logicCommands);
            if (scriptComponent == null) instance.Failed = true;
            else scriptComponent.Failed = true;
            Diagnostics.Add(new { blueprint = blueprint.Name, id = blueprint.Handle.Id, type = blueprint.GetType().FullName,
                context = blueprint.Handle.Context, callback = phase, frame = blueprint.Time.FrameIndex,
                component = scriptComponent?.Id ?? "", error = Unwrap(error).ToString(),
                exceptionType = Unwrap(error).GetType().FullName,
                action = scriptComponent == null ? "DisableBlueprint" : "DisableComponent" });
        }
        finally
        {
            // 回调耗时明细可定频刷新；故障、生命周期和启用变化必须立即发布。
            if (callbackError != null || phase is "BeginPlay" or "EndPlay" ||
                enabled != blueprint.Enabled || destroyed != blueprint.PendingDestroy ||
                scriptEnabled.Any(value => value.Component.Enabled != value.Enabled))
                context.DetailsDirty = true;
            if (scriptComponent == null) EndCallbackTiming(instance, phase, callbackStart);
            else RecordComponentTiming(instance, scriptComponent.Id, scriptComponent.GetType().FullName!,
                phase, callbackStart, callbackError);
            callbackBlueprint = null; callbackComponent = null; callbackPhase = "";
        }
    }

    private static void ValidateBlueprint(Blueprint blueprint, Vector3 priorScale)
    {
        ValidateTransform(blueprint.Transform);
        foreach (var component in blueprint.Components)
        {
            ValidateTransform(component.Transform);
            ValidateDynamicComponent(component);
        }
        if (blueprint.Body != null || blueprint.Character != null)
        {
            if (blueprint.Transform.Scale != priorScale || priorScale.X <= 0 ||
                priorScale.X != priorScale.Y || priorScale.X != priorScale.Z)
                throw new ArgumentException("Rigid body scale is fixed and must be positive uniform.");
            if (blueprint.Body is { } body && (!Finite(body.Velocity) || !Finite(body.Impulse)))
                throw new ArgumentException("Rigid body values must be finite.");
            if (blueprint.Character is { } character && (!Finite(character.WalkVelocity) || character.WalkVelocity.Y != 0 ||
                !Finite(character.RootMotionVelocity) || character.RootMotionVelocity.LengthSquared() > 4_000_000 ||
                blueprint.Transform.Rotation.X != 0 || blueprint.Transform.Rotation.Z != 0))
                throw new ArgumentException("Character requires horizontal walk velocity, bounded root motion and yaw-only rotation.");
        }
    }
    private static bool Finite(Vector3 v) => float.IsFinite(v.X) && float.IsFinite(v.Y) && float.IsFinite(v.Z);
    // 编辑器数字输入会序列化成 3.0；整数语义字段（priority/collisionMask）仍需接受整值 double。
    private static int IntValue(JsonElement value) => (int)Math.Round(value.GetDouble());
    private static uint UIntValue(JsonElement value) => (uint)Math.Round(value.GetDouble());

    private static PropertyInfo[] Properties(Type type) => Accessors(type).Exposed;
    private static object[] DescribeProperties(Type type)
    {
        var instance = Activator.CreateInstance(type)!;
        return Properties(type).Select(p => DescribeData(PropertySchema(p.PropertyType, type.FullName + "." + p.Name), p.GetValue(instance),
            type.FullName + "." + p.Name, p.Name, p.GetCustomAttribute<ExposeAttribute>()!.InstanceEditable)).ToArray();
    }
    private static object PropertyValue(object? value, Type type, string path) =>
        WriteData(PropertySchema(type, path), value, path, false, new(), 0, new(ReferenceEqualityComparer.Instance));
    private static void ReadTransform(Transform transform, JsonElement node)
    {
        transform.Position = Vector(node.GetProperty("position"));
        transform.Rotation = Vector(node.GetProperty("rotation"));
        transform.Scale = Vector(node.GetProperty("scale"));
    }
    private static void ReadOptionalTransform(Transform transform, JsonElement node)
    {
        if (node.TryGetProperty("position", out var p)) transform.Position = Vector(p);
        if (node.TryGetProperty("rotation", out var r)) transform.Rotation = Vector(r);
        if (node.TryGetProperty("scale", out var s)) transform.Scale = Vector(s);
    }
    private static Vector3 Vector(JsonElement value) => new(value[0].GetSingle(), value[1].GetSingle(), value[2].GetSingle());
    private static float[] Values(Vector3 value) => [value.X, value.Y, value.Z];
    private static void ValidateTransform(Transform transform)
    {
        if (!Finite(transform.Position) || !Finite(transform.Rotation) || !Finite(transform.Scale))
            throw new ArgumentException("Transform must be finite.");
        if (Math.Abs(transform.Scale.X) < 1e-6f || Math.Abs(transform.Scale.Y) < 1e-6f ||
            Math.Abs(transform.Scale.Z) < 1e-6f) throw new ArgumentException("Transform scale must be non-zero.");
    }
    private static Exception Unwrap(Exception error) =>
        error is TargetInvocationException { InnerException: not null } invocation ? invocation.InnerException! : error;
}
