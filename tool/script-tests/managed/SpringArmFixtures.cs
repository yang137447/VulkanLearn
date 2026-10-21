using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

// SpringArm 平滑测试夹具：只沿 +Z 匀速移动 owner，不依赖物理/输入。
public sealed class SpringArmMoverBlueprint : Blueprint
{
    protected override void Tick(float dt) => Transform.Position += new Vector3(0, 0, 200 * dt);
}

// 模拟正式控制器把玩家视角写入 ControlRotation；SpringArm 只读取。
public sealed class SpringArmControlBlueprint : Blueprint
{
    protected override void BeginPlay() => ControlRotation = new Vector3(0, 60, 0);
}

public sealed class SpringArmLateMoverBlueprint : Blueprint
{
    protected override void LateTick(float dt) => Transform.Position += new Vector3(200 * dt, 0, 0);
}

public sealed class SpringArmBodyMoverBlueprint : Blueprint
{
    protected override void BeginPlay() => Body!.LinearVelocity = new(200, 0, 0);
}

public sealed class SpringArmRotatorBlueprint : Blueprint
{
    protected override void Tick(float dt) => ControlRotation += new Vector3(0, 90 * dt, 0);
}

public sealed class SpringArmMovingWallBlueprint : Blueprint
{
    private int frames;
    protected override void Tick(float dt)
    {
        if (++frames == 2) Transform.Position += new Vector3(0, 0, 500);
    }
}

public sealed class SpringArmMutationBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool Fail { get; set; }
    protected override void Tick(float dt)
    {
        var arm = GetComponent<SpringArmComponent>("arm");
        arm.TargetArmLength = 180;
        arm.TargetOffset = new Vector3(0, 20, 0);
        arm.CameraLagSpeed = 0;
        ControlRotation = new Vector3(0, 45, 0);
        if (Fail) throw new InvalidOperationException("SpringArm settings rollback");
    }
}

public sealed class SpringArmDiagnosticsBlueprint : Blueprint
{
    protected override void LateTick(float dt)
    {
        var arm = GetComponent<SpringArmComponent>("arm");
        GetComponent("probe").Transform.Position = new(arm.GetUnfixedCameraPosition().Z,
            arm.IsCollisionFixApplied() ? 1 : 0, arm.GetTargetRotation().Y);
    }
}

public sealed class SpringArmTeleportBlueprint : Blueprint
{
    private int frame;
    protected override void Tick(float dt)
    {
        if (++frame != 2) return;
        Transform.Position += new Vector3(0, 0, 100);
        GetComponent<SpringArmComponent>("arm").ResetCameraLag();
    }
}

// 对照原生组件空间：非均匀缩放和多层旋转产生 shear，不能靠世界 TRS 分解蒙混通过。
public sealed class SpringArmSpaceProbeBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var component = GetComponent("camera");
        GetComponent("probe").Transform.Position = Vector3.Transform(new Vector3(3, 5, 7), component.WorldMatrix);
    }
}
