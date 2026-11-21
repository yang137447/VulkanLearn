using System.Numerics;
using System.Text.Json;
using VulkanLearn;

namespace MotionBricksVerification;

public sealed class Probe : Blueprint
{
    [Expose(InstanceEditable = true)] public string Stage { get; set; } = "walk";
    private AnimationPlayback animation = null!;
    private float elapsed;
    private float lastTrace;
    private bool started;

    protected override void OnCreate()
    {
        animation = GetComponent<MeshComponent>("visual").Animation;
    }

    protected override void Tick(float dt)
    {
        var feedback = animation.State;
        started |= feedback.RootMotion.LengthSquared() > 1e-6f || Stage == "idle";
        if (started) elapsed += dt;
        Vector2 move = Stage switch
        {
            "right" => new(1, 0),
            "left" => new(-1, 0),
            "back" => new(0, -1),
            "idle" => Vector2.Zero,
            "stop" when elapsed > 4 => Vector2.Zero,
            _ => new(0, 1)
        };
        animation.SetMotionBricksTarget(move, Stage == "run" ? 1.8f : 1);
        var state = animation.State;
        float rootDt = state.RootMotionDeltaSeconds;
        if (rootDt > 1e-5f)
            Character!.ConsumeRootMotion(new Vector3(state.RootMotion.X, 0,
                state.RootMotion.Z) / rootDt);
        else
            Character!.ClearRootMotion();
        Transform.Rotation = new(0, Transform.Rotation.Y +
            state.RootMotionYaw * 180 / MathF.PI, 0);
        if (elapsed - lastTrace >= 0.1f)
        {
            lastTrace = elapsed;
            var velocity = Character!.LinearVelocity;
            var position = Transform.Position;
            Log.Info("MOTIONBRICKS_TRACE " + JsonSerializer.Serialize(new
            {
                stage = Stage, elapsed, grounded = Character.IsGrounded,
                position = new[] { position.X, position.Y, position.Z },
                velocity = new[] { velocity.X, velocity.Y, velocity.Z },
                yaw = Transform.Rotation.Y,
                root = new[] { state.RootMotion.X, state.RootMotion.Y, state.RootMotion.Z }
            }));
        }
    }
}
