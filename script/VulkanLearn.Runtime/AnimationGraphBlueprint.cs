namespace VulkanLearn;

// 通用数据驱动 AnimGraph 宿主：资源侧只提供图定义与逐帧输入，本类负责
// 求值、状态迁移和把最终 clip 请求提交给唯一原生播放器。
public abstract class AnimationGraphBlueprint : AnimationBlueprint
{
    [Expose(InstanceEditable = true)] public bool InPlace { get; set; } = true;

    private AnimationGraphRuntime? runtime;
    private AnimationGraphDecision decision;
    private bool blendSubmitted;
    private bool slotMaskSubmitted;
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
    private string lastSlotMask = "";
    private float lastSlotAlpha = 1;
    private bool cachedPoseSubmitted;
    private bool cachedPoseBranchesSubmitted;
    private readonly Dictionary<string, float> cachedPoseBranchAxes = new(StringComparer.Ordinal);

    public AnimationGraphDefinition Graph { get; private set; } = null!;
    public AnimationGraphRuntime GraphRuntime
    {
        get
        {
            EnsureGraphPrepared();
            return runtime!;
        }
    }
    public AnimationGraphDecision GraphDecision => decision;

    protected abstract AnimationGraphDefinition CreateGraph();

    protected override void OnAnimationCreate()
    {
        // 组件在 World prepare 阶段创建，而 animation catalog 要到 start 才下发：
        // 引用 BS_*.json 的图在这里可能还拿不到资产，先延迟到首次求值/首次访问时解析。
        Graph = CreateGraph();
        TryPrepareGraph();
    }

    protected virtual void OnGraphCreated() { }

    private bool TryPrepareGraph()
    {
        if (runtime is not null) return true;
        if (HasBlendSpaceReferences(Graph) && Animation.BlendSpaces.Count == 0) return false;
        if (HasBlendProfileReferences(Graph) && Animation.BlendProfiles.Count == 0) return false;
        Graph = Graph.ResolveBlendSpaces(Animation.BlendSpaces)
            .ResolveBlendProfiles(Animation.BlendProfiles).Validate();
        runtime = new AnimationGraphRuntime(Graph);
        OnGraphCreated();
        return true;
    }

    private void EnsureGraphPrepared()
    {
        if (TryPrepareGraph()) return;
        throw new InvalidOperationException(
            "Animation graph references blend space assets but the animation catalog " +
            "has not been bound yet.");
    }

    private static bool HasBlendSpaceReferences(AnimationGraphDefinition graph)
    {
        foreach (var machine in graph.StateMachines)
            foreach (var state in machine.States)
                if (!string.IsNullOrWhiteSpace(state.BlendSpace)) return true;
        return false;
    }

    private static bool HasBlendProfileReferences(AnimationGraphDefinition graph)
    {
        foreach (var machine in graph.StateMachines)
            foreach (var state in machine.States)
                if (!string.IsNullOrWhiteSpace(state.LayerBlendProfile) ||
                    !string.IsNullOrWhiteSpace(state.SlotBlendProfile))
                    return true;
        return false;
    }

    // 角色在 LateTick 提交本帧冻结输入；覆盖写，不排队。
    protected void EvaluateGraph(in AnimationGraphInput input)
    {
        EnsureGraphPrepared();
        SubmitCachedPoseBranches();
        SubmitCachedPoseBranchAxes(input.PlanarSpeed);
        decision = runtime!.Update(input);
        SubmitDecision(decision);
    }

    // 命名分支只在图就绪后提交一次；轴值按平面速度逐帧下发（变化才发）。
    private void SubmitCachedPoseBranches()
    {
        if (cachedPoseBranchesSubmitted || Graph.CachedPoseBranches.Count == 0) return;
        var definitions = Graph.CachedPoseBranches.Select(branch =>
            new CachedPoseBranchDefinition(branch.Name,
                branch.BlendSamples.Count > 0
                    ? branch.BlendSamples
                        .Select(sample => new CachedPoseBranchSample(sample.Clip, sample.Threshold))
                        .ToArray()
                    : new[] { new CachedPoseBranchSample(branch.Clip, 0) },
                branch.Rate, branch.Loop)).ToArray();
        Animation.SetCachedPoseBranches(definitions);
        cachedPoseBranchesSubmitted = true;
    }

    private void SubmitCachedPoseBranchAxes(float axis)
    {
        foreach (var branch in Graph.CachedPoseBranches)
        {
            if (branch.BlendSamples.Count <= 1) continue;
            if (cachedPoseBranchAxes.TryGetValue(branch.Name, out float last) &&
                MathF.Abs(last - axis) < 1e-4f) continue;
            Animation.SetCachedPoseBranchAxis(branch.Name, axis);
            cachedPoseBranchAxes[branch.Name] = axis;
        }
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
        SubmitSlotMask(next);
    }

    // Slot mask 只在状态声明 mask 时生效；离开该状态后清空，后续 PlayMontage
    // 回到整身覆盖。
    private void SubmitSlotMask(in AnimationGraphDecision next)
    {
        string mask = next.Slot.Length > 0 && next.SlotMask is { Count: > 0 }
            ? string.Join("|", next.SlotMask)
            : next.Slot.Length > 0 && next.SlotWeights is { Count: > 0 }
                ? string.Join("|", next.SlotWeights.Select(w => $"{w.Bone}:{w.Weight:F3}"))
                : "";
        if (mask.Length == 0)
        {
            if (!slotMaskSubmitted) return;
            Animation.ClearMontageSlotMask();
            slotMaskSubmitted = false;
            lastSlotMask = "";
            return;
        }
        if (slotMaskSubmitted && string.Equals(lastSlotMask, mask, StringComparison.Ordinal) &&
            MathF.Abs(lastSlotAlpha - next.SlotAlpha) < 1e-4f)
            return;
        if (next.SlotWeights is { Count: > 0 })
            Animation.SetMontageSlotWeights(next.SlotWeights
                .Select(w => new AnimationBoneWeight(w.Bone, w.Weight)).ToArray(), next.SlotAlpha);
        else
            Animation.SetMontageSlotMask(next.SlotMask, next.SlotAlpha);
        slotMaskSubmitted = true;
        lastSlotMask = mask;
        lastSlotAlpha = next.SlotAlpha;
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
            ? string.Join("|", next.LayerMask)
            : next.LayerWeights is { Count: > 0 }
                ? string.Join("|", next.LayerWeights.Select(w => $"{w.Bone}:{w.Weight:F3}"))
                : "";
        if (layerSubmitted &&
            string.Equals(lastLayerClip, next.LayerClip, StringComparison.Ordinal) &&
            MathF.Abs(lastLayerAlpha - next.LayerAlpha) < 1e-4f &&
            lastLayerLoop == next.LayerLoop &&
            string.Equals(lastLayerMask, mask, StringComparison.Ordinal))
            return;
        if (next.LayerWeights is { Count: > 0 })
            Animation.SetLayeredPose(next.LayerClip, next.LayerAlpha, next.LayerLoop,
                next.LayerWeights.Select(w => new AnimationBoneWeight(w.Bone, w.Weight)).ToArray());
        else
            Animation.SetLayeredPose(next.LayerClip, next.LayerAlpha, next.LayerLoop,
                next.LayerMask);
        layerSubmitted = true;
        lastLayerClip = next.LayerClip;
        lastLayerAlpha = next.LayerAlpha;
        lastLayerLoop = next.LayerLoop;
        lastLayerMask = mask;
    }
}
