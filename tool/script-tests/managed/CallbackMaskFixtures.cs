using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

public sealed class EmptyFrameBlueprint : Blueprint { }
public sealed class EmptyFrameComponent : Component { }

public abstract class InheritedFrameBlueprintBase : Blueprint
{
    protected override void Tick(float dt) => Transform.Translate(Vector3.UnitX);
    protected override void LateTick(float dt) => Transform.Translate(Vector3.UnitZ);
}
public sealed class InheritedFrameBlueprint : InheritedFrameBlueprintBase { }
public sealed class HiddenFrameBlueprint : InheritedFrameBlueprintBase
{
    private new void Tick(float dt) => throw new InvalidOperationException("hidden-tick");
    private new void LateTick(float dt) => throw new InvalidOperationException("hidden-late");
}

public abstract class InheritedFrameComponentBase : Component
{
    protected override void Tick(float dt) => Transform.Translate(Vector3.UnitX);
    protected override void LateTick(float dt) => Transform.Translate(Vector3.UnitZ);
}
public sealed class InheritedFrameComponent : InheritedFrameComponentBase { }
public sealed class HiddenFrameComponent : InheritedFrameComponentBase
{
    private new void Tick(float dt) => throw new InvalidOperationException("hidden-component-tick");
    private new void LateTick(float dt) => throw new InvalidOperationException("hidden-component-late");
}

public sealed class ExplicitEmptyFrameBlueprint : Blueprint
{
    protected override void Tick(float dt) { }
    protected override void LateTick(float dt) { }
}
public sealed class ExplicitEmptyFrameComponent : Component
{
    protected override void Tick(float dt) { }
    protected override void LateTick(float dt) { }
}

public sealed class FrameServicesBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        Timers.Set(0, () => Transform.Translate(Vector3.UnitX));
        Events.Subscribe("frame-owner", _ => Transform.Translate(Vector3.UnitY));
        Events.Publish("frame-owner");
    }
}
public sealed class FrameServicesComponent : Component
{
    [Expose] public int TimerCalls { get; set; }
    [Expose] public int EventCalls { get; set; }
    protected override void BeginPlay()
    {
        Timers.Set(0, () => ++TimerCalls);
        Events.Subscribe("frame-component", _ => ++EventCalls);
        Events.Publish("frame-component");
    }
}

public sealed class EmptyFrameLifecycleComponent : Component
{
    [Expose] public int Begins { get; set; }
    [Expose] public int Ends { get; set; }
    protected override void BeginPlay()
    {
        ++Begins;
        Timers.Set(100, () => Log.Info("empty-frame-timer"), repeat: true);
        Events.Subscribe("empty-frame", _ => Log.Info("empty-frame-event"));
    }
    protected override void EndPlay(EndPlayReason reason) => ++Ends;
}
public sealed class DisableEmptyFrameComponentBlueprint : Blueprint
{
    protected override void Tick(float dt) => GetScriptComponent("logic").Enabled = false;
}

public abstract class ThrowingFrameBlueprintBase : Blueprint
{
    protected override void Tick(float dt)
    {
        Transform.Translate(Vector3.UnitX);
        Timers.Set(0, () => Log.Info("rolled-back-frame"));
        throw new InvalidOperationException("inherited-frame-failed");
    }
}
public sealed class ThrowingFrameBlueprint : ThrowingFrameBlueprintBase { }
public abstract class ThrowingFrameComponentBase : Component
{
    protected override void Tick(float dt)
    {
        Transform.Translate(Vector3.UnitX);
        Timers.Set(0, () => Log.Info("rolled-back-component-frame"));
        throw new InvalidOperationException("inherited-component-frame-failed");
    }
}
public sealed class ThrowingFrameComponent : ThrowingFrameComponentBase { }
