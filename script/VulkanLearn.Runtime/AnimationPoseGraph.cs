namespace VulkanLearn;

// 纯代码 pose-node graph（阶段 A）：不是可视化编辑器，也不替换状态机。
// 它把 UE 风格的节点图（Player/Blend/Additive/Layer/Slot/CachedPose）求值后
// 扁平化到现有原生播放器能消费的一份 AnimationPoseResult。
public enum AnimationPoseNodeKind
{
    SequencePlayer,
    BlendSpacePlayer,
    BlendSpace2DPlayer,
    MontagePlayer,
    Blend,
    Select,
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
    // Pose graph BlendSpacePlayer 的权重插值速度；由 BS_*.json 解析填入。
    public float WeightSpeedPerSec { get; init; } = 5;
    public IReadOnlyList<AnimationGraphBlendSample> BlendSamples { get; init; } =
        Array.Empty<AnimationGraphBlendSample>();
    // BlendSpacePlayer 的 BS_*.json 资产名；宿主解析后填入 BlendSamples。
    public string BlendSpace { get; init; } = "";
    // MontagePlayer 的 Montage id 或资源相对路径。
    public string MontageId { get; init; } = "";

    // Blend / ApplyAdditive / LayeredBoneBlend / Slot
    public float Alpha { get; init; } = 1;
    public IReadOnlyList<AnimationGraphBoneWeight> Weights { get; init; } =
        Array.Empty<AnimationGraphBoneWeight>();
    // LayeredBoneBlend / Slot 的 BP_*.json 资产名；ResolveBlendProfiles 后清空并展开到 Weights。
    public string BlendProfile { get; init; } = "";

    // SaveCachedPose / UseCachedPose / Slot
    public string CacheName { get; init; } = "";
    public string SlotName { get; init; } = "";
    // Output 节点的根位移/notify 来源（Player 节点 id）。
    public string RootSource { get; init; } = "";

    internal AnimationPoseNode WithBlendSamples(IReadOnlyList<AnimationGraphBlendSample> samples,
        AnimationPoseNodeKind? kind = null, float weightSpeedPerSec = 5) => new()
    {
        Id = Id,
        Kind = kind ?? Kind,
        Inputs = Inputs,
        Clip = Clip,
        Loop = Loop,
        Rate = Rate,
        StartPosition = StartPosition,
        WeightSpeedPerSec = weightSpeedPerSec,
        BlendSpace = "",
        BlendSamples = samples,
        MontageId = MontageId,
        Alpha = Alpha,
        Weights = Weights,
        BlendProfile = BlendProfile,
        CacheName = CacheName,
        SlotName = SlotName,
        RootSource = RootSource
    };

    internal AnimationPoseNode WithBlendProfileWeights(
        IReadOnlyList<AnimationGraphBoneWeight> weights) => new()
    {
        Id = Id,
        Kind = Kind,
        Inputs = Inputs,
        Clip = Clip,
        Loop = Loop,
        Rate = Rate,
        StartPosition = StartPosition,
        WeightSpeedPerSec = WeightSpeedPerSec,
        BlendSpace = BlendSpace,
        BlendSamples = BlendSamples,
        MontageId = MontageId,
        Alpha = Alpha,
        Weights = weights,
        BlendProfile = "",
        CacheName = CacheName,
        SlotName = SlotName,
        RootSource = RootSource
    };

    public PoseGraphNodeDefinition ToDefinition() => new(
        Id, Kind.ToString(), Inputs.ToArray(), Clip,
        BlendSamples.Select(sample => new PoseGraphSampleDefinition(
            sample.Clip, sample.Threshold, sample.Rate, sample.SyncGroup,
            sample.X, sample.Y)).ToArray(),
        Rate, Loop, StartPosition < 0 ? 0 : StartPosition, Alpha,
        Weights.Select(weight => new AnimationBoneWeight(weight.Bone, weight.Weight)).ToArray(),
        CacheName, SlotName, RootSource, MontageId, WeightSpeedPerSec);
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
    string SaveCachedPose,
    IReadOnlyList<AnimationGraphBlendSample>? BlendSpaceSamples = null,
    IReadOnlyList<float>? BlendSpaceWeights = null,
    string MontageId = "");

public sealed class AnimationPoseGraph
{
    public const int MaximumNodes = 128;
    public const int MaximumBlendSamples = 4;

    public string Name { get; init; } = "";
    public string OutputNode { get; init; } = "";
    public IReadOnlyList<AnimationPoseNode> Nodes { get; init; } = Array.Empty<AnimationPoseNode>();

    public void Validate(IReadOnlyCollection<string>? entryNodes = null,
        IReadOnlyCollection<string>? knownCachedPoses = null)
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
        if (!string.IsNullOrWhiteSpace(OutputNode) &&
            (!byId.TryGetValue(OutputNode, out var output) ||
             output.Kind != AnimationPoseNodeKind.Output))
            throw new InvalidOperationException("Pose graph output node is missing.");
        if (string.IsNullOrWhiteSpace(OutputNode) && (entryNodes is null || entryNodes.Count == 0))
            throw new InvalidOperationException(
                "Pose graph requires an Output node or state entry nodes.");
        foreach (var node in Nodes)
            foreach (var input in node.Inputs)
                if (!byId.ContainsKey(input))
                    throw new InvalidOperationException(
                        $"Pose graph node '{node.Id}' references unknown input '{input}'.");
        foreach (var node in Nodes)
            if (node.Kind == AnimationPoseNodeKind.Slot && node.Inputs.Count == 2 &&
                byId[node.Inputs[1]].Kind != AnimationPoseNodeKind.MontagePlayer)
                throw new InvalidOperationException(
                    $"Pose graph Slot '{node.Id}' second input must be a MontagePlayer.");

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
                {
                    // 图级 CachedPoseBranches 也能提供同名缓存（Third Person 的 Locomotion）。
                    if (knownCachedPoses is not null && knownCachedPoses.Contains(byId[id].CacheName))
                    {
                        visiting.Remove(id);
                        visited.Add(id);
                        return;
                    }
                    throw new InvalidOperationException(
                        $"Pose graph uses undefined cached pose '{byId[id].CacheName}'.");
                }
                Visit(producer);
            }
            visiting.Remove(id);
            visited.Add(id);
        }
        var roots = new List<string>();
        if (!string.IsNullOrWhiteSpace(OutputNode)) roots.Add(OutputNode);
        if (entryNodes is not null)
            foreach (var entry in entryNodes)
                if (!string.IsNullOrWhiteSpace(entry) && byId.ContainsKey(entry) && !roots.Contains(entry))
                    roots.Add(entry);
        foreach (var root in roots) Visit(root);
        if (visited.Count != Nodes.Count)
            throw new InvalidOperationException("Pose graph has unreachable nodes.");
    }

    private static void ValidateNode(AnimationPoseNode node, HashSet<string> cacheNames)
    {
        int expected = node.Kind switch
        {
            AnimationPoseNodeKind.SequencePlayer => 0,
            AnimationPoseNodeKind.BlendSpacePlayer => 0,
            AnimationPoseNodeKind.BlendSpace2DPlayer => 0,
            AnimationPoseNodeKind.MontagePlayer => 0,
            AnimationPoseNodeKind.UseCachedPose => 0,
            AnimationPoseNodeKind.Output => 1,
            AnimationPoseNodeKind.SaveCachedPose => 1,
            AnimationPoseNodeKind.Slot => -1,
            AnimationPoseNodeKind.Blend => 2,
            AnimationPoseNodeKind.Select => 2,
            AnimationPoseNodeKind.ApplyAdditive => 2,
            AnimationPoseNodeKind.LayeredBoneBlend => 2,
            _ => throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' has an unsupported kind.")
        };
        if (node.Kind == AnimationPoseNodeKind.Slot)
        {
            if (node.Inputs.Count is not (1 or 2))
                throw new InvalidOperationException(
                    $"Pose graph node '{node.Id}' Slot requires one or two inputs.");
        }
        else if (node.Inputs.Count != expected)
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' requires {expected} inputs.");
        if (!float.IsFinite(node.Rate) || node.Rate <= 0 || node.Rate > 4)
            throw new InvalidOperationException($"Pose graph node '{node.Id}' rate is invalid.");
        if (!float.IsFinite(node.Alpha) || node.Alpha < 0 || node.Alpha > 1)
            throw new InvalidOperationException($"Pose graph node '{node.Id}' alpha is invalid.");
        if (node.BlendProfile.Length > 0)
        {
            if (node.Weights.Count > 0)
                throw new InvalidOperationException(
                    $"Pose graph node '{node.Id}' cannot combine blend profile and weights.");
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' blend profile '{node.BlendProfile}' was not resolved.");
        }
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
                    node.BlendSamples.Count > 3)
                    throw new InvalidOperationException(
                        $"Pose graph node '{node.Id}' requires 2..3 samples.");
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
            case AnimationPoseNodeKind.BlendSpace2DPlayer:
                ValidateBlendSpace2D(node);
                break;
            case AnimationPoseNodeKind.MontagePlayer:
                if (string.IsNullOrWhiteSpace(node.MontageId) ||
                    node.MontageId.Length > 256 ||
                    node.MontageId.Trim().Length != node.MontageId.Length ||
                    node.MontageId.Contains('\0'))
                    throw new InvalidOperationException(
                        $"Pose graph node '{node.Id}' requires a montage id.");
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
            case AnimationPoseNodeKind.Select:
                if (node.Inputs.Count != 2)
                    throw new InvalidOperationException(
                        $"Pose graph node '{node.Id}' Select requires two inputs.");
                break;
            case AnimationPoseNodeKind.ApplyAdditive:
            case AnimationPoseNodeKind.LayeredBoneBlend:
                break;
        }
    }

    private static void ValidateBlendSpace2D(AnimationPoseNode node)
    {
        if (node.BlendSamples is null || node.BlendSamples.Count != 4)
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' requires exactly four 2D samples.");
        var xs = new HashSet<float>();
        var ys = new HashSet<float>();
        var cells = new HashSet<(float X, float Y)>();
        foreach (var sample in node.BlendSamples)
        {
            if (string.IsNullOrWhiteSpace(sample.Clip) ||
                !float.IsFinite(sample.X) || !float.IsFinite(sample.Y) ||
                !float.IsFinite(sample.Rate) || sample.Rate <= 0 || sample.Rate > 4)
                throw new InvalidOperationException(
                    $"Pose graph node '{node.Id}' has invalid 2D blend samples.");
            xs.Add(sample.X);
            ys.Add(sample.Y);
            if (!cells.Add((sample.X, sample.Y)))
                throw new InvalidOperationException(
                    $"Pose graph node '{node.Id}' repeats a 2D grid coordinate.");
        }
        if (xs.Count != 2 || ys.Count != 2 || cells.Count != 4)
            throw new InvalidOperationException(
                $"Pose graph node '{node.Id}' samples must form a 2x2 grid.");
    }

    // entryNode：状态自己的 PoseNode；为空时才回退到图级 Output。
    public AnimationPoseResult Evaluate(float planarSpeed, string entryNode = "",
        IReadOnlyDictionary<string, float>? values = null)
    {
        if (!float.IsFinite(planarSpeed))
            throw new ArgumentOutOfRangeException(nameof(planarSpeed));
        if (!string.IsNullOrWhiteSpace(entryNode) &&
            !Nodes.Any(node => string.Equals(node.Id, entryNode, StringComparison.Ordinal)))
            throw new InvalidOperationException($"Pose graph entry node '{entryNode}' is missing.");
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
                AnimationPoseNodeKind.BlendSpace2DPlayer => EvaluateBlendSpace2D(node, values),
                AnimationPoseNodeKind.MontagePlayer => new PoseValue
                {
                    Rate = node.Rate,
                    Loop = node.Loop,
                    StartPosition = node.StartPosition,
                    MontageId = node.MontageId
                },
                AnimationPoseNodeKind.Blend => EvaluateBlend(node,
                    EvaluateNode(node.Inputs[0]), EvaluateNode(node.Inputs[1]), node.Alpha),
                AnimationPoseNodeKind.Select => EvaluateNode(node.Inputs[
                    values is not null && values.TryGetValue(node.Id, out var select) &&
                    select >= 0.5f ? 1 : 0]),
                AnimationPoseNodeKind.ApplyAdditive => EvaluateAdditive(node,
                    EvaluateNode(node.Inputs[0]), EvaluateNode(node.Inputs[1])),
                AnimationPoseNodeKind.LayeredBoneBlend => EvaluateLayer(node,
                    EvaluateNode(node.Inputs[0]), EvaluateNode(node.Inputs[1])),
                AnimationPoseNodeKind.SaveCachedPose => EvaluateSave(node,
                    EvaluateNode(node.Inputs[0])),
                AnimationPoseNodeKind.UseCachedPose => new PoseValue { UseCachedPose = node.CacheName },
                AnimationPoseNodeKind.Slot => node.Inputs.Count == 2
                    ? EvaluateSlot(node, EvaluateNode(node.Inputs[0]),
                        EvaluateNode(node.Inputs[1]))
                    : EvaluateSlot(node, EvaluateNode(node.Inputs[0])),
                AnimationPoseNodeKind.Output => EvaluateNode(node.Inputs[0]),
                _ => throw new InvalidOperationException($"Unsupported pose node '{id}'.")
            };
            visiting.Remove(id);
            memo[id] = value;
            return value;
        }

        var result = EvaluateNode(!string.IsNullOrWhiteSpace(entryNode) ? entryNode : OutputNode ?? "");
        return new AnimationPoseResult(result.Clip, result.Loop, result.Rate, result.StartPosition,
            result.BlendSamples, result.BlendClip, result.BlendWeight,
            result.AdditiveClip, result.AdditiveAlpha, result.AdditiveLoop,
            result.LayerClip, result.LayerAlpha, result.LayerLoop, result.LayerWeights,
            result.Slot, result.SlotAlpha, result.SlotWeights,
            result.UseCachedPose, result.SaveCachedPose,
            result.BlendSpaceSamples, result.BlendSpaceWeights, result.MontageId);
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

    private static PoseValue EvaluateBlendSpace2D(AnimationPoseNode node,
        IReadOnlyDictionary<string, float>? values)
    {
        var samples = node.BlendSamples;
        var xs = samples.Select(sample => sample.X).Distinct().OrderBy(value => value).ToArray();
        var ys = samples.Select(sample => sample.Y).Distinct().OrderBy(value => value).ToArray();
        float axisX = 0;
        float axisY = 0;
        if (values is not null && values.TryGetValue($"{node.Id}.y", out var y))
            axisY = y;
        if (values is not null && values.TryGetValue(node.Id, out var x))
            axisX = x;
        float alphaX = xs[1] > xs[0] ? Math.Clamp((axisX - xs[0]) / (xs[1] - xs[0]), 0, 1) : 0;
        float alphaY = ys[1] > ys[0] ? Math.Clamp((axisY - ys[0]) / (ys[1] - ys[0]), 0, 1) : 0;
        var weights = new float[4];
        for (int index = 0; index < samples.Count; ++index)
        {
            var sample = samples[index];
            float wx = sample.X == xs[0] ? 1 - alphaX : alphaX;
            float wy = sample.Y == ys[0] ? 1 - alphaY : alphaY;
            weights[index] = wx * wy;
        }
        return new PoseValue
        {
            // 根源/完成反馈固定为声明顺序第一个 sample，与原生一致。
            Clip = samples[0].Clip,
            Loop = node.Loop,
            Rate = node.Rate,
            StartPosition = node.StartPosition,
            BlendSamples = samples,
            BlendSpaceSamples = samples,
            BlendSpaceWeights = weights
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

    private static PoseValue EvaluateSlot(AnimationPoseNode node, PoseValue pose,
        PoseValue montage)
    {
        pose = EvaluateSlot(node, pose);
        pose.MontageId = montage.MontageId;
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
        public IReadOnlyList<AnimationGraphBlendSample>? BlendSpaceSamples;
        public IReadOnlyList<float>? BlendSpaceWeights;
        public string MontageId = "";
        public string UseCachedPose = "";
        public string SaveCachedPose = "";
    }
}
