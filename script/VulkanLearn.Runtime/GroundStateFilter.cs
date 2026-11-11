namespace VulkanLearn;

// 物理接地标志的去抖：Jolt CharacterVirtual 的 IsGrounded 在起跳/落地瞬间会
// 抖动 1–2 帧，直接把原始值喂给动画图会让 Airborne↔Land 反复重播。
// 每个渲染帧只能调用一次 Update（通常在物理步之后），计数按帧而不是按调用次数。
public sealed class GroundStateFilter
{
    private readonly int confirmFrames;
    private int groundedFrames;
    private int airborneFrames;

    public GroundStateFilter(bool initialGrounded = true, int confirmFrames = 2)
    {
        if (confirmFrames < 1 || confirmFrames > 60)
            throw new ArgumentOutOfRangeException(nameof(confirmFrames));
        Stable = initialGrounded;
        this.confirmFrames = confirmFrames;
    }

    public bool Stable { get; private set; }

    // 原始值连续 confirmFrames 帧一致才翻转；中途被打断则重新计数。
    public bool Update(bool rawGrounded)
    {
        groundedFrames = rawGrounded ? groundedFrames + 1 : 0;
        airborneFrames = rawGrounded ? 0 : airborneFrames + 1;
        if (airborneFrames >= confirmFrames) Stable = false;
        else if (groundedFrames >= confirmFrames) Stable = true;
        return Stable;
    }

    // 传送、重生或 Play 重新构造实例后复位，不继承上一段状态。
    public void Reset(bool grounded = true)
    {
        Stable = grounded;
        groundedFrames = 0;
        airborneFrames = 0;
    }
}
