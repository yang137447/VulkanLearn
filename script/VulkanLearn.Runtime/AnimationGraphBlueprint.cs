namespace VulkanLearn;

// 通用数据驱动 AnimGraph 宿主：资源侧只提供图定义与逐帧输入，本类负责
// 求值、状态迁移和把最终 clip 请求提交给唯一原生播放器。
public abstract class AnimationGraphBlueprint : AnimationBlueprint
{
    [Expose(InstanceEditable = true)] public bool InPlace { get; set; } = true;

    private AnimationGraphRuntime? runtime;
    private AnimationGraphDecision decision;
    private bool blendSubmitted;
    private string lastBlendClip = "";
    private float lastBlendWeight;

    public AnimationGraphDefinition Graph { get; private set; } = null!;
    public AnimationGraphRuntime GraphRuntime => runtime ??
        throw new InvalidOperationException("Animation graph is not initialized.");
    public AnimationGraphDecision GraphDecision => decision;

    protected abstract AnimationGraphDefinition CreateGraph();

    protected override void OnAnimationCreate()
    {
        Graph = CreateGraph().Validate();
        runtime = new AnimationGraphRuntime(Graph);
        OnGraphCreated();
    }

    protected virtual void OnGraphCreated() { }

    // 角色在 LateTick 提交本帧冻结输入；覆盖写，不排队。
    protected void EvaluateGraph(in AnimationGraphInput input)
    {
        if (runtime is null)
            throw new InvalidOperationException("Animation graph is not initialized.");
        decision = runtime.Update(input);
        SubmitDecision(decision);
    }

    protected virtual void SubmitDecision(in AnimationGraphDecision next)
    {
        if (string.IsNullOrEmpty(next.Clip)) return;
        var playback = Animation.State;
        if (next.Changed || !string.Equals(playback.Clip, next.Clip, StringComparison.Ordinal))
        {
            if (next.Crossfade || next.StartPosition > 0)
            {
                Animation.PlayTransition(next.Clip, next.StartPosition, next.Loop,
                    next.BlendSeconds, InPlace, 1, next.Crossfade && next.Inertialization,
                    next.Crossfade ? next.SyncGroup : "",
                    next.Crossfade ? next.SyncPhase : -1);
            }
            else
            {
                Animation.Play(next.Clip, next.Loop, 1, InPlace);
            }
            // 新播放段需要重新声明混合源；原生 TransitionTo/Play 已把它清空。
            blendSubmitted = false;
        }
        SubmitBlendSpace(next);
    }

    // 权重变化只提交混合候选，不重启 A 的时钟；离开混合状态时显式清除原生混合源。
    private void SubmitBlendSpace(in AnimationGraphDecision next)
    {
        if (next.BlendClip.Length == 0)
        {
            if (!blendSubmitted) return;
            Animation.ClearBlendSpace();
            blendSubmitted = false;
            lastBlendClip = "";
            return;
        }
        if (blendSubmitted && string.Equals(lastBlendClip, next.BlendClip, StringComparison.Ordinal) &&
            MathF.Abs(lastBlendWeight - next.BlendWeight) < 1e-4f)
            return;
        Animation.SetBlendSpace(next.Clip, next.BlendClip, next.BlendWeight);
        blendSubmitted = true;
        lastBlendClip = next.BlendClip;
        lastBlendWeight = next.BlendWeight;
    }
}
