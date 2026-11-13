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
    private bool additiveSubmitted;
    private string lastAdditiveClip = "";
    private float lastAdditiveAlpha;
    private bool lastAdditiveLoop;
    private bool layerSubmitted;
    private string lastLayerClip = "";
    private float lastLayerAlpha;
    private bool lastLayerLoop;
    private string lastLayerMask = "";
    private bool cachedPoseSubmitted;

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
        var playback = Animation.State;
        if (next.UseCachedPose.Length > 0)
        {
            Animation.UseCachedPose(next.UseCachedPose);
            cachedPoseSubmitted = true;
            // 缓存姿态没有自己的片段时钟；离开缓存状态时再显式清除。
            if (blendSubmitted)
            {
                Animation.ClearBlendSpace();
                blendSubmitted = false;
            }
            SubmitAdditive(next);
            SubmitLayer(next);
        }
        else
        {
            if (cachedPoseSubmitted)
            {
                Animation.ClearCachedPose();
                cachedPoseSubmitted = false;
            }
            if (next.Clip.Length > 0 &&
                (next.Changed || !string.Equals(playback.Clip, next.Clip, StringComparison.Ordinal)))
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
            SubmitAdditive(next);
            SubmitLayer(next);
        }
        // UE 的根 SaveCachedPose 保存最终输出；命令在下一帧生效，正好对应
        // “进入 Land 时使用上一帧姿态”的时序。
        if (Graph.SaveCachedPose.Length > 0)
            Animation.CaptureCachedPose(Graph.SaveCachedPose);
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

    // ApplyAdditive 是状态姿态的一部分；权重/片段变化才重新提交，原生保留自己的时钟。
    private void SubmitAdditive(in AnimationGraphDecision next)
    {
        if (next.AdditiveClip.Length == 0)
        {
            if (!additiveSubmitted) return;
            Animation.ClearAdditive();
            additiveSubmitted = false;
            lastAdditiveClip = "";
            return;
        }
        if (additiveSubmitted &&
            string.Equals(lastAdditiveClip, next.AdditiveClip, StringComparison.Ordinal) &&
            MathF.Abs(lastAdditiveAlpha - next.AdditiveAlpha) < 1e-4f &&
            lastAdditiveLoop == next.AdditiveLoop)
            return;
        Animation.SetAdditive(next.AdditiveClip, next.AdditiveAlpha, next.AdditiveLoop);
        additiveSubmitted = true;
        lastAdditiveClip = next.AdditiveClip;
        lastAdditiveAlpha = next.AdditiveAlpha;
        lastAdditiveLoop = next.AdditiveLoop;
    }

    // LayeredBoneBlend：只有 clip/alpha/loop/mask 变化时才重新提交，避免每帧重建 mask。
    private void SubmitLayer(in AnimationGraphDecision next)
    {
        if (next.LayerClip.Length == 0)
        {
            if (!layerSubmitted) return;
            Animation.ClearLayeredPose();
            layerSubmitted = false;
            lastLayerClip = "";
            lastLayerMask = "";
            return;
        }
        string mask = next.LayerMask is { Count: > 0 }
            ? string.Join("|", next.LayerMask) : "";
        if (layerSubmitted &&
            string.Equals(lastLayerClip, next.LayerClip, StringComparison.Ordinal) &&
            MathF.Abs(lastLayerAlpha - next.LayerAlpha) < 1e-4f &&
            lastLayerLoop == next.LayerLoop &&
            string.Equals(lastLayerMask, mask, StringComparison.Ordinal))
            return;
        Animation.SetLayeredPose(next.LayerClip, next.LayerAlpha, next.LayerLoop,
            next.LayerMask);
        layerSubmitted = true;
        lastLayerClip = next.LayerClip;
        lastLayerAlpha = next.LayerAlpha;
        lastLayerLoop = next.LayerLoop;
        lastLayerMask = mask;
    }
}
