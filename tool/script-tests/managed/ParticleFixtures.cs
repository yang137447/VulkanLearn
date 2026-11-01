using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

// C# 粒子 API 的端到端探针：BeginPlay spawn，随后按帧验证覆盖、reset 和 destroy。
public sealed class ParticleProbeBlueprint : Blueprint
{
    private ParticleSystem? spawned;
    private int phase;

    protected override void BeginPlay()
    {
        spawned = Particles.Spawn(
            "Maps/SC_particle_demo/PS_sparks.json",
            new Vector3(0, 100, 0));
    }

    protected override void Tick(float dt)
    {
        if (spawned == null)
            throw new InvalidOperationException("Particle spawn did not produce a handle.");
        if (phase++ == 0)
        {
            spawned.SetVisible(false);
            spawned.SetSpawnRateScale(2);
            spawned.SetColorTint(new Vector4(1, 0.5f, 0.25f, 1));
            spawned.SetEmitterEnabled(0, false);
        }
        else if (phase == 2)
        {
            spawned.ResetOverrides();
        }
        else if (phase == 3)
        {
            spawned.Destroy();
        }
        else if (phase == 4 && spawned.IsAlive)
        {
            throw new InvalidOperationException("Particle remained live after Destroy.");
        }
    }
}

// 覆盖 emitter 级生命周期、预算/用户参数、附着、遥测与事件桥。
public sealed class ParticleApiBlueprint : Blueprint
{
    private ParticleSystem? spawned;
    private ParticleEmitterHandle? extraEmitter;
    private ParticleEventSubscription? subscription;
    private int eventCount;
    private int phase;

    protected override void BeginPlay()
    {
        spawned = Particles.Spawn(
            "Maps/SC_particle_demo/PS_sparks.json",
            new Vector3(0, 100, 0));
        subscription = spawned.Subscribe(_ => ++eventCount);
    }

    protected override void Tick(float dt)
    {
        if (spawned == null || subscription == null)
            throw new InvalidOperationException("Particle API fixture did not initialize.");
        if (phase++ == 0)
        {
            extraEmitter = spawned.SpawnEmitter(0, "runtime.extra");
            spawned.SetEmitterBudget(512, 0.5f);
            spawned.SetUserParameter("Wind", 2.5f);
            spawned.AttachTo("mesh", "mesh|Anchor",
                localPosition: new Vector3(0, 25, 0));
            if (spawned.Telemetry == null)
                throw new InvalidOperationException("Particle telemetry was not delivered.");
        }
        else if (phase == 2)
        {
            if (eventCount != 1)
                throw new InvalidOperationException("Particle event bridge did not deliver one event.");
            spawned.RestartEmitter(extraEmitter!.Value);
            spawned.SetEmitterEnabled(extraEmitter!.Value, false);
        }
        else if (phase == 3)
        {
            spawned.DestroyEmitter(extraEmitter!.Value);
            spawned.Detach();
            spawned.ClearUserParameter("Wind");
            spawned.SetEmitterBudget(0, 1);
            spawned.Destroy();
            subscription.Dispose();
            subscription = null;
        }
    }
}
