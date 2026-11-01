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

// World 全局数据通道探针：BeginPlay 读取 native 预置通道，写入/清除后
// 在后续 tick 验证 native 回传；OnReload 验证热重载没有丢失 World 状态。
public sealed class ParticleDataBlueprint : Blueprint
{
    private static int endPlayCount;
    private static bool lastEndPlayReadSucceeded;

    // 热重载迁移会保存 [Migrate] 值属性，避免帧相位回到 0 重放旧断言。
    [Migrate] public int Phase { get; set; }

    private static void Expect<T>(Action action, string label) where T : Exception
    {
        try
        {
            action();
        }
        catch (T)
        {
            return;
        }
        throw new InvalidOperationException($"{label} did not reject invalid input.");
    }

    protected override void BeginPlay()
    {
        if (endPlayCount > 0 && !lastEndPlayReadSucceeded)
            throw new InvalidOperationException(
                "Particle data channels were not readable during EndPlay.");
        var data = Particles.Data;
        // Seed 由 native World 在 Play 前预置；start 请求必须先把通道交给托管侧。
        if (data.Get("Seed") is { } seed && seed != 1.25f)
            throw new InvalidOperationException("Particle data channel Seed was not delivered.");
        data.Set("Wind", 2.5f);
        data.Set("Heat", -4.0f);
        if (data.Get("Wind") != 2.5f || data.Get("Heat") != -4.0f)
            throw new InvalidOperationException("Particle data channel local write was not visible.");
        if (data.Get("Missing") != null)
            throw new InvalidOperationException("Missing particle data channel returned a value.");

        Expect<ArgumentException>(() => data.Set("", 1.0f), "Empty data channel key");
        Expect<ArgumentException>(() => data.Set(new string('k', 65), 1.0f), "Oversized data channel key");
        Expect<ArgumentException>(() => data.Set("bad\0key", 1.0f), "NUL data channel key");
        Expect<ArgumentOutOfRangeException>(() => data.Set("Nan", float.NaN), "NaN data channel value");
        Expect<ArgumentOutOfRangeException>(() => data.Set("Huge", 1.000001e6f), "Oversized data channel value");
        var cjkName = new string('中', 21);
        data.Set(cjkName, 1.0f);
        if (data.Get(cjkName) != 1.0f)
            throw new InvalidOperationException("UTF-8 data channel key was not round-tripped.");
        data.Clear(cjkName);
        Expect<ArgumentException>(
            () => data.Set(new string('中', 22), 1.0f),
            "Oversized UTF-8 data channel key");
        data.Clear("Missing");
    }

    protected override void OnReload()
    {
        if (Particles.Data.Get("Wind") is not { } wind || wind != 3.5f)
            throw new InvalidOperationException("Particle data channels were lost across live reload.");
    }

    protected override void EndPlay(EndPlayReason reason)
    {
        ++endPlayCount;
        lastEndPlayReadSucceeded = false;
        lastEndPlayReadSucceeded =
            Particles.Data.Get("Wind") is { } wind && wind == 3.5f;
        if (!lastEndPlayReadSucceeded)
            throw new InvalidOperationException(
                "Particle data channels were cleared before EndPlay.");
    }

    protected override void Tick(float dt)
    {
        if (Phase++ == 0)
        {
            if (Particles.Data.Get("Wind") != 2.5f ||
                Particles.Data.Get("Heat") != -4.0f)
                throw new InvalidOperationException("Particle data channels did not round-trip through native.");
            Particles.Data.Clear("Seed");
            Particles.Data.Set("Wind", 3.5f);
            if (Particles.Data.Get("Seed") != null ||
                Particles.Data.Get("Wind") != 3.5f)
                throw new InvalidOperationException("Particle data channel update was not visible.");
        }
        else if (Phase == 2)
        {
            if (Particles.Data.Get("Wind") != 3.5f ||
                Particles.Data.Get("Heat") != -4.0f ||
                Particles.Data.Get("Seed") != null)
                throw new InvalidOperationException("Particle data channels drifted after a native frame.");
        }
    }
}
