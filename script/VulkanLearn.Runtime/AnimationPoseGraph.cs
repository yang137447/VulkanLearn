namespace VulkanLearn;

// 纯代码 pose-node graph（阶段 A）：不是可视化编辑器，也不替换状态机。
// 它把 UE 风格的节点图（Player/Blend/Additive/Layer/Slot/CachedPose）求值后
// 扁平化到现有原生播放器能消费的一份 AnimationPoseResult。
public enum AnimationPoseNodeKind
{
    SequencePlayer,
    BlendSpacePlayer,
    Blend,
    ApplyAdditive,
    LayeredBoneBlend,
    SaveCachedPose,
    UseCachedPose,
    Slot,
    Output
}

public sealed class AnimationPoseNode
{
    public string Id { get; init; } = "";
    public AnimationPoseNodeKind Kind { get; init; }
    public IReadOnlyList<string> Inputs { get; init; } = Array.Empty<string>();

    // SequencePlayer / BlendSpacePlayer
    public string Clip { get; init; } = "";
    public bool Loop { get; init; } = true;
    public float Rate { get; init; } = 1;
    public float StartPosition { get; init; } = -1;
    public IReadOnlyList<AnimationGraphBlendSample> BlendSamples { get; init; } =
        Array.Empty<AnimationGraphBlendSample>();

    // Blend / ApplyAdditive / LayeredBoneBlend / Slot
    public float Alpha { get; init; } = 1;
    public IReadOnlyList<AnimationGraphBoneWeight> Weights { get; init; } =
        Array.Empty<AnimationGraphBoneWeight>();

    // SaveCachedPose / UseCachedPose / Slot
    public string CacheName { get; init; } = "";
    public string SlotName { get; init; } = "";
}

// 扁平化结果：字段与 AnimationGraphDecision 的子集一一对应。
public sealed record AnimationPoseResult(
    string Clip,
    bool Loop,
    float Rate,
    float StartPosition,
    IReadOnlyList<AnimationGraphBlendSample> BlendSamples,
    string BlendClip,
    float BlendWeight,
    string AdditiveClip,
    float AdditiveAlpha,
    bool AdditiveLoop,
    string LayerClip,
    float LayerAlpha,
    bool LayerLoop,
    IReadOnlyList<AnimationGraphBoneWeight> LayerWeights,
    string Slot,
    float SlotAlpha,
    IReadOnlyList<AnimationGraphBoneWeight> SlotWeights,
    string UseCachedPose,
    string SaveCachedPose);

public sealed class AnimationPoseGraph
{
    public const int MaximumNodes = 128;
    public const int MaximumBlendSamples = 3;

    public string Name { get; init; } = "";
    public string OutputNode { get; init; } = "";
    public IReadOnlyList<AnimationPoseNode> Nodes { get; init; } = Array.Empty<AnimationPoseNode>();

    public void Validate()
    {
        if (string.IsNullOrWhiteSpace(Name))
            throw new InvalidOperationException("Pose graph name is required.");
        if (Nodes is null || Nodes.Count == 0 || Nodes.Count > MaximumNodes)
            throw new InvalidOperationException("Pose graph node budget exceeded.");

        var byId = new Dictionary<string, AnimationPoseNode>(StringComparer.Ordinal);
        var cacheNames = new HashSet<string>(StringComparer.Ordinal);
        var cacheProducers = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var node in Nodes)
        {
            if (node is null) throw new InvalidOperationException("Pose graph has a null node.");
            if (string.IsNullOrWhiteSpace(node.Id) || node.Id.Trim().Length != node.Id.Length ||
                node.Id.Length > 128 || node.Id.Contains('\0'))
                throw new InvalidOperationException("Pose graph node id is invalid.");
            if (!byId.TryAdd(node.Id, node))
                throw new InvalidOperationException($"Duplicate pose graph node '{node.Id}'.");
            if (node.Inputs is null)
                throw new InvalidOperationException($"Pose graph node '{node.Id}' inputs cannot be null.");
            ValidateNode(node, cacheNames);
            if (node.Kind == AnimationPoseNodeKind.SaveCachedPose)
                cacheProducers[node.CacheName] = node.Id;
        }
        if (string.IsNullOrWhiteSpace(OutputNode) || !byId.TryGetValue(OutputNode, out var output) ||
            output.Kind != AnimationPoseNodeKind.Output)
            throw new InvalidOperationException("Pose graph output node is missing.");
        foreach (var node in Nodes)
            foreach (var input in node.Inputs)
                if (!byId.ContainsKey(input))
                    throw new InvalidOperationException(
                        $"Pose graph node '{node.Id}' references unknown input '{input}'.");

        // 环检测 + 可达性：所有节点都必须能被 Output 用到，避免静默死节点。
        var visiting = new HashSet<string>(StringComparer.Ordinal);
        var visited = new HashSet<string>(StringComparer.Ordinal);
        void Visit(string id)
        {
            if (visited.Contains(id)) return;
            if (!visiting.Add(id))
                throw new InvalidOperationException($"Pose graph has a cycle at '{id}'.");
            foreach (var input in byId[id].Inputs) Visit(input);
            // UseCachedPose 通过名字依赖 SaveCachedPose：把生产者纳入可达性/环检测。
            if (byId[id].Kind == AnimationPoseNodeKind.UseCachedPose)
            {
                if (!cacheProducers.TryGetValue(byId[id].CacheName, out var producer))
                    throw new InvalidOperationException(
                        $"Pose graph uses undefined cached pose '{byId[id].CacheName}'.");
                Visit(producer);
            }
            visiting.Remove(id);
            visited.Add(id);
        }
        Visit(OutputNode);
        if (visited.Count != Nodes.Count)
            throw new InvalidOperationException("Pose graph has unreachable nodes.");
    }

    private static void ValidateNode(AnimationPoseNode node, HashSet<string> cacheNames)
    {
        int expected = node.Kind switch
        {
            AnimationPoseNodeKind.SequencePlayer => 0,
            AnimationPoseNodeKind.BlendSpacePlayer => 0,
            AnimationPoseNodeKind.UseCachedPose => 0,
            AnimationPoseNodeKind.Output => 1,
            AnimationPoseNodeKind.SaveCachedPose => 1,
            AnimationPoseNodeKind.Slot => 1,
            AnimationPoseNodeKind.Blend => 2,
            AnimationPoseNodeKind.ApplyAdditive => 2,
            AnimationPoseNodeKind.LayeredBoneBlend => 2,
            _ => throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' has an unsupported kind.")
        };
        if (node.Inputs.Count != expected)
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' requires {expected} inputs.");
        if (!float.IsFinite(node.Rate) || node.Rate <= 0 || node.Rate > 4)
            throw new InvalidOperationException($"Pose graph node '{node.Id}' rate is invalid.");
        if (!float.IsFinite(node.Alpha) || node.Alpha < 0 || node.Alpha > 1)
            throw new InvalidOperationException($"Pose graph node '{node.Id}' alpha is invalid.");
        foreach (var weight in node.Weights)
        {
            if (string.IsNullOrWhiteSpace(weight.Bone) || weight.Bone.Length > 128 ||
                !float.IsFinite(weight.Weight) || weight.Weight < 0 || weight.Weight > 1)
                throw new InvalidOperationException(
                    $"Pose graph node '{node.Id}' has an invalid bone weight.");
        }
        switch (node.Kind)
        {
            case AnimationPoseNodeKind.SequencePlayer:
                if (string.IsNullOrWhiteSpace(node.Clip) || node.Clip.Length > 128 ||
                    node.Clip.Trim().Length != node.Clip.Length)
                    throw new InvalidOperationException(
                        $"Pose graph node '{node.Id}' requires a sequence clip.");
                break;
            case AnimationPoseNodeKind.BlendSpacePlayer:
                if (node.BlendSamples is null || node.BlendSamples.Count < 2 ||
                    node.BlendSamples.Count > MaximumBlendSamples)
                    throw new InvalidOperationException(
                        $"Pose graph node '{node.Id}' requires 2..{MaximumBlendSamples} samples.");
                float previous = float.NegativeInfinity;
                foreach (var sample in node.BlendSamples)
                {
                    if (string.IsNullOrWhiteSpace(sample.Clip) ||
                        !float.IsFinite(sample.Threshold) || sample.Threshold <= previous)
                        throw new InvalidOperationException(
                            $"Pose graph node '{node.Id}' has invalid blend samples.");
                    previous = sample.Threshold;
                }
                break;
            case AnimationPoseNodeKind.SaveCachedPose:
            case AnimationPoseNodeKind.UseCachedPose:
                if (string.IsNullOrWhiteSpace(node.CacheName) || node.CacheName.Length > 128 ||
                    node.CacheName.Trim().Length != node.CacheName.Length)
                    throw new InvalidOperationException(
                        $"Pose graph node '{node.Id}' requires a cache name.");
                if (node.Kind == AnimationPoseNodeKind.SaveCachedPose &&
                    !cacheNames.Add(node.CacheName))
                    throw new InvalidOperationException(
                        $"Pose graph has duplicate cached pose '{node.CacheName}'.");
                break;
            case AnimationPoseNodeKind.Slot:
                if (string.IsNullOrWhiteSpace(node.SlotName) || node.SlotName.Length > 64 ||
                    node.SlotName.Trim().Length != node.SlotName.Length)
                    throw new InvalidOperationException(
                        $"Pose graph node '{node.Id}' requires a slot name.");
                break;
            case AnimationPoseNodeKind.Blend:
            case AnimationPoseNodeKind.ApplyAdditive:
            case AnimationPoseNodeKind.LayeredBoneBlend:
                break;
        }
    }

    public AnimationPoseResult Evaluate(float planarSpeed)
    {
        if (!float.IsFinite(planarSpeed))
            throw new ArgumentOutOfRangeException(nameof(planarSpeed));
        var byId = Nodes.ToDictionary(node => node.Id, StringComparer.Ordinal);
        var memo = new Dictionary<string, PoseValue>(StringComparer.Ordinal);
        var visiting = new HashSet<string>(StringComparer.Ordinal);

        PoseValue EvaluateNode(string id)
        {
            if (memo.TryGetValue(id, out var cached)) return cached;
            if (!visiting.Add(id))
                throw new InvalidOperationException($"Pose graph cycle at '{id}'.");
            var node = byId[id];
            PoseValue value = node.Kind switch
            {
                AnimationPoseNodeKind.SequencePlayer => new PoseValue
                {
                    Clip = node.Clip,
                    Loop = node.Loop,
                    Rate = node.Rate,
                    StartPosition = node.StartPosition
                },
                AnimationPoseNodeKind.BlendSpacePlayer => EvaluateBlendSpace(node, planarSpeed),
                AnimationPoseNodeKind.Blend => EvaluateBlend(node,
                    EvaluateNode(node.Inputs[0]), EvaluateNode(node.Inputs[1]), node.Alpha),
                AnimationPoseNodeKind.ApplyAdditive => EvaluateAdditive(node,
                    EvaluateNode(node.Inputs[0]), EvaluateNode(node.Inputs[1])),
                AnimationPoseNodeKind.LayeredBoneBlend => EvaluateLayer(node,
                    EvaluateNode(node.Inputs[0]), EvaluateNode(node.Inputs[1])),
                AnimationPoseNodeKind.SaveCachedPose => EvaluateSave(node,
                    EvaluateNode(node.Inputs[0])),
                AnimationPoseNodeKind.UseCachedPose => new PoseValue { UseCachedPose = node.CacheName },
                AnimationPoseNodeKind.Slot => EvaluateSlot(node, EvaluateNode(node.Inputs[0])),
                AnimationPoseNodeKind.Output => EvaluateNode(node.Inputs[0]),
                _ => throw new InvalidOperationException($"Unsupported pose node '{id}'.")
            };
            visiting.Remove(id);
            memo[id] = value;
            return value;
        }

        var result = EvaluateNode(OutputNode ?? "");
        return new AnimationPoseResult(result.Clip, result.Loop, result.Rate, result.StartPosition,
            result.BlendSamples, result.BlendClip, result.BlendWeight,
            result.AdditiveClip, result.AdditiveAlpha, result.AdditiveLoop,
            result.LayerClip, result.LayerAlpha, result.LayerLoop, result.LayerWeights,
            result.Slot, result.SlotAlpha, result.SlotWeights,
            result.UseCachedPose, result.SaveCachedPose);
    }

    private static PoseValue EvaluateBlendSpace(AnimationPoseNode node, float axis)
    {
        var samples = node.BlendSamples;
        int baseIndex = samples.Count >= 3 ? 1 : 0;
        string clip = samples[baseIndex].Clip;
        string blendClip;
        float target;
        if (baseIndex == 0)
        {
            blendClip = samples[1].Clip;
            target = Smoothstep(axis, samples[0].Threshold, samples[1].Threshold);
        }
        else if (axis <= samples[1].Threshold)
        {
            blendClip = samples[0].Clip;
            target = 1 - Smoothstep(axis, samples[0].Threshold, samples[1].Threshold);
        }
        else
        {
            blendClip = samples[2].Clip;
            target = Smoothstep(axis, samples[1].Threshold, samples[2].Threshold);
        }
        return new PoseValue
        {
            Clip = clip,
            Loop = node.Loop,
            Rate = node.Rate,
            StartPosition = node.StartPosition,
            BlendSamples = samples,
            BlendClip = blendClip,
            BlendWeight = target
        };
    }

    private static float Smoothstep(float axis, float from, float to)
    {
        float span = to - from;
        float alpha = span > 0 ? Math.Clamp((axis - from) / span, 0, 1) : 0;
        return alpha * alpha * (3 - 2 * alpha);
    }

    private static PoseValue EvaluateBlend(AnimationPoseNode node, PoseValue basePose,
        PoseValue overlay, float alpha)
    {
        if (basePose.BlendSamples.Count > 0 || overlay.BlendSamples.Count > 0 ||
            basePose.UseCachedPose.Length > 0 || overlay.UseCachedPose.Length > 0)
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' Blend only supports two single-clip players.");
        if (basePose.BlendClip.Length > 0)
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' Blend input is already blended.");
        basePose = basePose.Clone();
        basePose.BlendClip = overlay.Clip;
        basePose.BlendWeight = alpha;
        return basePose;
    }

    private static PoseValue EvaluateAdditive(AnimationPoseNode node, PoseValue basePose,
        PoseValue additive)
    {
        if (additive.Clip.Length == 0 || additive.BlendSamples.Count > 0 ||
            additive.UseCachedPose.Length > 0)
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' additive input must be a single-clip player.");
        if (basePose.AdditiveClip.Length > 0)
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' has more than one additive layer.");
        basePose = basePose.Clone();
        basePose.AdditiveClip = additive.Clip;
        basePose.AdditiveAlpha = node.Alpha;
        basePose.AdditiveLoop = additive.Loop;
        return basePose;
    }

    private static PoseValue EvaluateLayer(AnimationPoseNode node, PoseValue basePose,
        PoseValue layer)
    {
        if (layer.Clip.Length == 0 || layer.BlendSamples.Count > 0 ||
            layer.UseCachedPose.Length > 0)
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' layer input must be a single-clip player.");
        if (basePose.LayerClip.Length > 0)
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' has more than one layered blend.");
        if (node.Weights.Count == 0)
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' layered blend requires bone weights.");
        basePose = basePose.Clone();
        basePose.LayerClip = layer.Clip;
        basePose.LayerAlpha = node.Alpha;
        basePose.LayerLoop = layer.Loop;
        basePose.LayerWeights = node.Weights;
        return basePose;
    }

    private static PoseValue EvaluateSave(AnimationPoseNode node, PoseValue pose)
    {
        pose = pose.Clone();
        pose.SaveCachedPose = node.CacheName;
        return pose;
    }

    private static PoseValue EvaluateSlot(AnimationPoseNode node, PoseValue pose)
    {
        pose = pose.Clone();
        pose.Slot = node.SlotName;
        pose.SlotAlpha = node.Alpha;
        pose.SlotWeights = node.Weights;
        return pose;
    }

    private sealed class PoseValue
    {
        public PoseValue Clone() => (PoseValue)MemberwiseClone();
        public string Clip = "";
        public bool Loop = true;
        public float Rate = 1;
        public float StartPosition = -1;
        public IReadOnlyList<AnimationGraphBlendSample> BlendSamples =
            Array.Empty<AnimationGraphBlendSample>();
        public string BlendClip = "";
        public float BlendWeight;
        public string AdditiveClip = "";
        public float AdditiveAlpha;
        public bool AdditiveLoop;
        public string LayerClip = "";
        public float LayerAlpha;
        public bool LayerLoop;
        public IReadOnlyList<AnimationGraphBoneWeight> LayerWeights =
            Array.Empty<AnimationGraphBoneWeight>();
        public string Slot = "";
        public float SlotAlpha = 1;
        public IReadOnlyList<AnimationGraphBoneWeight> SlotWeights =
            Array.Empty<AnimationGraphBoneWeight>();
        public string UseCachedPose = "";
        public string SaveCachedPose = "";
    }
}
