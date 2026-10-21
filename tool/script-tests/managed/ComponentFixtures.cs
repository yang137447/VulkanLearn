using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

public sealed class CounterComponent : Component
{
    [Expose(InstanceEditable = true)] public float Speed { get; set; } = 10;
    [Expose] public int Ticks { get; set; }
    [Expose] public int Begins { get; set; }
    [Expose] public int Ends { get; set; }
    [Expose(InstanceEditable = true)] public bool FailTick { get; set; }
    [Expose(InstanceEditable = true)] public bool FailCreate { get; set; }
    [Expose(InstanceEditable = true)] public bool FailTimer { get; set; }
    [Expose(InstanceEditable = true)] public Quaternion Orientation { get; set; } = Quaternion.Identity;
    protected override void OnCreate()
    {
        if (FailCreate) throw new InvalidOperationException("component-create-failed");
    }
    protected override void BeginPlay()
    {
        Begins++;
        Timers.Set(0, () =>
        {
            if (FailTimer)
            {
                Transform.Translate(new(100, 0, 0));
                throw new InvalidOperationException("component-timer-failed");
            }
            Log.Info($"component-timer:{Id}");
        });
        Timers.Set(100, () => Log.Info($"component-repeat:{Id}"), repeat: true);
        Events.Subscribe("ComponentPulse", _ => Log.Info($"component-event:{Id}"));
    }
    protected override void Tick(float dt)
    {
        Ticks++;
        Transform.Translate(new(Speed * dt, 0, 0));
        if (FailTick)
        {
            Timers.Set(0, () => Log.Info("failed-component-timer"));
            throw new InvalidOperationException("component-tick-failed");
        }
    }
    protected override void EndPlay(EndPlayReason reason) { Ends++; Log.Info($"component-end:{Id}"); }
}

public sealed class ComponentControllerBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public int Mode { get; set; }
    private int frame;
    private ScriptComponentHandle prior;
    private Component? removed;
    protected override void Tick(float dt)
    {
        frame++;
        if (Mode == 1)
        {
            var counter = GetScriptComponent<CounterComponent>("counter");
            if (frame == 1) counter.Enabled = false;
            if (frame == 2) counter.Enabled = true;
        }
        if (Mode == 2)
        {
            if (frame == 1) prior = AddScriptComponent<CounterComponent>("runtime");
            if (frame == 2)
            {
                removed = ResolveScriptComponent(prior);
                RemoveScriptComponent(prior);
            }
            if (frame == 3)
            {
                if (removed!.IsAlive || ResolveScriptComponent(prior) != null)
                    throw new InvalidOperationException("removed component alive");
                try { removed.Enabled = true; throw new InvalidOperationException("stale setter accepted"); }
                catch (InvalidOperationException error) when (error.Message == "Stale script component.") { }
                var next = AddScriptComponent<CounterComponent>("runtime");
                if (next == prior) throw new InvalidOperationException("handle reused");
            }
        }
        if (Mode == 3 && frame == 1)
        {
            AddScriptComponent<CounterComponent>("rolled-back");
            throw new InvalidOperationException("component-command-rollback");
        }
        if (Mode == 4 && frame == 2) Destroy();
        if (Mode == 5 && frame == 2) throw new InvalidOperationException("owner-failed");
    }
}
