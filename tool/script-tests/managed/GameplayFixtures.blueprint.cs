using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

public sealed class PassiveBlueprint : Blueprint { }

public sealed class RuntimeComponentsBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool Fail { get; set; }
    [Expose(InstanceEditable = true)] public bool UnknownMesh { get; set; }
    [Expose(InstanceEditable = true)] public bool Cycle { get; set; }
    private ComponentHandle arm, mesh, camera;
    private SceneComponent? old;
    private int phase;
    protected override void BeginPlay()
    {
        arm = AddComponent(new() { Id = "arm", Position = new(50, 0, 0) });
        mesh = AddComponent(new() { Id = "visual", Kind = "mesh", Parent = "arm",
            Mesh = new(UnknownMesh ? "Common/Meshes/SM_missing.json" : "Common/Meshes/SM_test.json") });
        camera = AddComponent(new() { Id = "lens", Kind = "camera", Parent = "arm" });
        AddComponent(new() { Id = "lamp", Kind = "pointLight", Parent = "arm" });
        if (Components.Count != 0) throw new InvalidOperationException("Structure changed inside callback.");
        if (Fail) throw new InvalidOperationException("component queue rollback");
    }
    protected override void Tick(float dt)
    {
        if (phase++ == 0)
        {
            old = ResolveComponent(mesh)!;
            GetComponent<CameraComponent>("lens").Activate();
            AttachComponent(mesh);
            if (Cycle) AttachComponent(arm, camera);
        }
        else if (phase == 2) RemoveComponent(arm);
        else if (phase == 3)
        {
            RemoveComponent(mesh);
        }
        else if (phase == 4)
        {
            if (ResolveComponent(mesh) != null || old!.IsAlive) throw new InvalidOperationException("Removed component is live.");
            try { old.Transform.Position = Vector3.One; throw new Exception("Stale transform accepted."); }
            catch (InvalidOperationException) { }
            mesh = AddComponent(new() { Id = "visual", Kind = "mesh", Mesh = new("Common/Meshes/SM_test.json") });
        }
    }
}

public sealed class LightBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool Invalid { get; set; }
    [Expose(InstanceEditable = true)] public bool Disable { get; set; }
    [Expose(InstanceEditable = true)] public bool DestroyOnTick { get; set; }
    protected override void Tick(float dt)
    {
        GetComponent("arm").Transform.Rotate(new(0, 90 * dt, 0));
        var light = GetComponent<SpotLightComponent>("lamp");
        light.Color = new(0.2f, 0.4f, 0.6f);
        light.Intensity = 2500;
        light.InnerConeAngle = 10;
        light.OuterConeAngle = Invalid ? 5 : 20;
        if (Disable) Enabled = false;
        if (DestroyOnTick) Destroy();
    }
}

public sealed class CameraBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool Activate { get; set; }
    [Expose(InstanceEditable = true)] public bool InvalidProjection { get; set; }
    [Expose(InstanceEditable = true)] public bool DestroyOnTick { get; set; }
    protected override void Tick(float dt)
    {
        Transform.Translate(new(100 * dt, 0, 0));
        var camera = GetComponent<CameraComponent>("lens");
        if (Activate) camera.Activate();
        if (InvalidProjection) camera.FarClip = camera.NearClip;
        if (DestroyOnTick) Destroy();
    }
}

public sealed class ComponentBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool Fail { get; set; }
    protected override void Tick(float dt)
    {
        GetComponent("arm").Transform.Rotate(new(0, 90 * dt, 0));
        if (Fail) throw new InvalidOperationException("component rollback");
    }
}

public sealed class SpawnerBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public string Asset { get; set; } = "Common/Blueprints/BP_child.json";
    [Expose(InstanceEditable = true)] public int Count { get; set; } = 1;
    [Expose(InstanceEditable = true)] public bool FailAfterSpawn { get; set; }
    [Expose(InstanceEditable = true)] public bool DestroySpawn { get; set; }
    [Expose(InstanceEditable = true)] public bool SpawnDuringTick { get; set; }
    [Expose(InstanceEditable = true)] public bool StaleHandle { get; set; }
    [Expose(InstanceEditable = true)] public float SpawnScale { get; set; } = 1;
    private readonly List<BlueprintHandle> children = [];
    private bool ticked;
    private void SpawnChildren()
    {
        for (int index = 0; index < Count; ++index)
            children.Add(World.Spawn(Asset, new(index * 100, 300, 0), scale: new Vector3(SpawnScale)));
        if (FailAfterSpawn) throw new InvalidOperationException("spawn rollback");
    }
    protected override void BeginPlay() { if (!SpawnDuringTick) SpawnChildren(); }
    protected override void Tick(float dt)
    {
        if (ticked) return;
        ticked = true;
        if (SpawnDuringTick) SpawnChildren();
        Transform.Position = new(children.Count(c => World.IsAlive(c)), 0, 0);
        if (DestroySpawn) foreach (var child in children) World.Destroy(child);
        if (StaleHandle) World.Destroy(new(Handle.Context + 1, Handle.Id));
    }
}

public sealed class PhysicsBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public float ImpulseX { get; set; }
    [Expose(InstanceEditable = true)] public bool Resize { get; set; }
    private int enters, exits;
    protected override void BeginPlay() => Body!.AddImpulse(new(ImpulseX, 0, 0));
    protected override void Tick(float dt)
    {
        if (Resize) Transform.Scale *= 2;
    }
    protected override void LateTick(float dt)
    {
        // 使用组件局部位置回显碰撞次数，不干涉刚体根位姿。
        if (Components.Count > 0) Components[0].Transform.Position = new(enters, exits, Body!.LinearVelocity.Y);
    }
    protected override void OnCollisionEnter(BlueprintHandle other) => enters++;
    protected override void OnCollisionExit(BlueprintHandle other) => exits++;
}

public sealed class PhysicsProbeBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool RayInLate { get; set; }
    [Expose(InstanceEditable = true)] public bool IncludeTriggers { get; set; }
    [Expose(InstanceEditable = true)] public bool FailRay { get; set; }
    private bool requested;
    private int callbacks, enters, exits, collisions;
    private void Query()
    {
        if (requested) return;
        requested = true;
        Physics.Raycast(Vector3.Zero, new(1000, 0, 0), hit =>
        {
            callbacks++;
            GetComponent("probe").Transform.Position = hit is { } found
                ? new(found.Distance, found.Normal.X, World.GetBlueprint(found.Blueprint).Handle == found.Blueprint ? 1 : 0)
                : new(-1, 0, 0);
            if (FailRay) throw new InvalidOperationException("ray callback rollback");
        }, IncludeTriggers);
    }
    protected override void Tick(float dt) { if (!RayInLate) Query(); }
    protected override void LateTick(float dt)
    {
        if (RayInLate) Query();
        GetComponent("timing").Transform.Position = new(callbacks, Time.FrameIndex, 0);
        GetComponent("events").Transform.Position = new(enters, exits, collisions);
    }
    protected override void OnTriggerEnter(BlueprintHandle other) => enters++;
    protected override void OnTriggerExit(BlueprintHandle other) => exits++;
    protected override void OnCollisionEnter(BlueprintHandle other) => collisions++;
}

public sealed class CharacterBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool InvalidMove { get; set; }
    protected override void Tick(float dt) => Character!.Move(new(100, InvalidMove ? 1 : 0, 0));
    protected override void LateTick(float dt) =>
        GetComponent("probe").Transform.Position = new(Character!.IsGrounded ? 1 : 0, 0, 0);
}

public sealed class TimerBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool Repeat { get; set; }
    [Expose(InstanceEditable = true)] public bool Cancel { get; set; }
    [Expose(InstanceEditable = true)] public bool Fail { get; set; }
    [Expose(InstanceEditable = true)] public bool DestroyOnTick { get; set; }
    protected override void BeginPlay()
    {
        var timer = Timers.Set(0.1, () => Transform.Translate(new(1, 0, 0)), Repeat);
        if (Cancel) Timers.Cancel(timer);
        if (Fail) throw new InvalidOperationException("timer rollback");
    }
    protected override void Tick(float dt) { if (DestroyOnTick) Destroy(); }
}

public sealed class MessageBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool Sender { get; set; }
    [Expose(InstanceEditable = true)] public bool Fail { get; set; }
    [Expose(InstanceEditable = true)] public bool Targeted { get; set; }
    private bool sent;
    protected override void BeginPlay() => Events.Subscribe("ping", message =>
    {
        if (message.Payload != "payload" || !World.IsAlive(message.Sender))
            throw new InvalidOperationException("message identity");
        Transform.Translate(new(0, 1, 0));
    });
    protected override void Tick(float dt)
    {
        if (!Sender || sent) return;
        sent = true;
        var target = World.Find("receiver") ?? throw new InvalidOperationException("missing target");
        Transform.Position = new(World.FindAll("ScriptFixtures.MessageBlueprint").Count, 0,
            World.GetBlueprint(target).Position.X);
        if (Targeted) Events.Send(target, "ping", "payload");
        else Events.Publish("ping", "payload");
        if (Fail) throw new InvalidOperationException("event rollback");
    }
}

public sealed class NamedInputBlueprint : Blueprint
{
    protected override void Tick(float dt) => Transform.Position =
        new(Input.GetAxis("Custom"), Input.WasPressed("Use") ? 1 : 0, Input.IsPressed("Use") ? 1 : 0);
}

public sealed class AsyncAssetsBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool FailRequest { get; set; }
    [Expose(InstanceEditable = true)] public bool FailCompletion { get; set; }
    [Expose(InstanceEditable = true)] public bool CancelOnTick { get; set; }
    [Expose(InstanceEditable = true)] public bool ReloadAfterCancel { get; set; }
    [Expose(InstanceEditable = true)] public bool UnloadOnComplete { get; set; }
    [Expose(InstanceEditable = true)] public int DestroyOwnerFrame { get; set; }
    [Expose(InstanceEditable = true)] public int DestroySpawnFrame { get; set; }
    [Expose(InstanceEditable = true)] public int LoadCount { get; set; } = 1;
    private readonly List<AssetLoadHandle> requests = [];
    private readonly List<BlueprintHandle> children = [];
    private bool reloaded;
    protected override void BeginPlay()
    {
        for (int index = 0; index < LoadCount; ++index)
            requests.Add(Assets.LoadBlueprintAsync("Common/Blueprints/BP_dynamic.json", result =>
            {
                if (result.Success)
                {
                    children.Add(World.Spawn(result.Asset, new(0, 200, 0)));
                    Transform.Translate(new(1, 0, 0));
                    if (UnloadOnComplete) Assets.Unload(result.Lease!.Value);
                }
                else Transform.Translate(new(0, 1, 0));
                if (FailCompletion) throw new InvalidOperationException("asset completion rollback");
            }));
        if (FailRequest) throw new InvalidOperationException("asset request rollback");
    }
    protected override void Tick(float dt)
    {
        if (CancelOnTick)
        {
            foreach (var request in requests) Assets.Cancel(request);
            if (ReloadAfterCancel && !reloaded)
            {
                reloaded = true;
                Assets.LoadBlueprintAsync("Common/Blueprints/BP_dynamic.json", _ => { });
            }
        }
        if (DestroyOwnerFrame > 0 && Time.FrameIndex >= (ulong)DestroyOwnerFrame) Destroy();
        if (DestroySpawnFrame > 0 && Time.FrameIndex >= (ulong)DestroySpawnFrame)
            foreach (var child in children) if (World.IsAlive(child)) World.Destroy(child);
    }
}

public sealed class ColliderMutatorBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool RemoveCollider { get; set; }
    [Expose(InstanceEditable = true)] public bool Nonuniform { get; set; }
    protected override void Tick(float dt)
    {
        Transform.Translate(new(10, 0, 0));
        var collider = GetComponent("collider");
        if (RemoveCollider) RemoveComponent(collider.Handle);
        if (Nonuniform) collider.Transform.Scale = new(1, 2, 1);
    }
}
