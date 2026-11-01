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
