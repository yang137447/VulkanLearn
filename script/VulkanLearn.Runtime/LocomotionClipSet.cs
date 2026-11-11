namespace VulkanLearn;

// 旧 LocomotionClipSet 兼容映射：供旧测试与未接图的自定义宿主使用。
// Echo 正式路径由资源侧 EchoLocomotionGraph.cs 提供片段名，不再依赖这里。
public sealed class LocomotionClipSet
{
    private static readonly string[] DefaultWalkStarts =
    [
        "Idle_to_Walk_Fwd",
        "Idle_to_Walk_Fwd_Left_90",
        "Idle_to_Walk_Fwd_Left_180",
        "Idle_to_Walk_Fwd_Right_90",
        "Idle_to_Walk_Fwd_Right_180"
    ];

    private static readonly string[] DefaultJogStarts =
    [
        "Idle_To_Jog_Fwd",
        "Idle_to_Jog_Fwd_Left_90",
        "Idle_to_Jog_Fwd_Left_180",
        "Idle_to_Jog_Fwd_Right_90",
        "Idle_to_Jog_Fwd_Right_180"
    ];

    public string Idle { get; set; } = "Idle";
    public string Walk { get; set; } = "Walk_Fwd";
    public string Jog { get; set; } = "Jog_Fwd";
    public string StopWalk { get; set; } = "Walk_Fwd_to_Idle";
    public string StopJog { get; set; } = "Jog_Fwd_to_Idle";
    public string Fall { get; set; } = "Jump_Idle_Fall";
    public string Land { get; set; } = "Jump_Idle_Land";
    public string[] WalkStarts { get; } = (string[])DefaultWalkStarts.Clone();
    public string[] JogStarts { get; } = (string[])DefaultJogStarts.Clone();

    public string LoopClip(LocomotionState state) => state switch
    {
        LocomotionState.Idle => Idle,
        LocomotionState.Walk => Walk,
        LocomotionState.Jog => Jog,
        _ => throw new ArgumentOutOfRangeException(nameof(state))
    };

    // None 返回 null：调用方应继续当前片段，而不是重新提交同一循环。
    public string? TransitionClip(LocomotionTransition transition, LocomotionDirection direction) =>
        transition switch
        {
            LocomotionTransition.None => null,
            LocomotionTransition.StartWalk => StartClip(WalkStarts, direction, LocomotionState.Walk),
            LocomotionTransition.StartJog => StartClip(JogStarts, direction, LocomotionState.Jog),
            LocomotionTransition.StopWalk => StopWalk,
            LocomotionTransition.StopJog => StopJog,
            LocomotionTransition.Fall => Fall,
            LocomotionTransition.Land => Land,
            _ => throw new ArgumentOutOfRangeException(nameof(transition))
        };

    public void SetStart(LocomotionState gait, LocomotionDirection direction, string clip)
    {
        var clips = gait switch
        {
            LocomotionState.Walk => WalkStarts,
            LocomotionState.Jog => JogStarts,
            _ => throw new ArgumentOutOfRangeException(nameof(gait), "Start clips exist only for Walk and Jog.")
        };
        if (string.IsNullOrWhiteSpace(clip))
            throw new ArgumentException("Start clip id is required.", nameof(clip));
        if ((uint)(int)direction >= (uint)clips.Length)
            throw new ArgumentOutOfRangeException(nameof(direction));
        clips[(int)direction] = clip;
    }

    private static string StartClip(string[] clips, LocomotionDirection direction, LocomotionState gait)
    {
        if ((uint)(int)direction >= (uint)clips.Length)
            throw new ArgumentOutOfRangeException(nameof(direction));
        string clip = clips[(int)direction];
        if (string.IsNullOrWhiteSpace(clip))
            throw new InvalidOperationException($"Locomotion {gait} start clip for {direction} is not configured.");
        return clip;
    }
}
