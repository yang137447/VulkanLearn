using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

// 纯代码 pose-node graph 的前端求值探针：不加载骨骼，只验证扁平化结果与校验。
public sealed class AnimationPoseGraphProbeBlueprint : Blueprint
{
    protected override void BeginPlay()
    {
        var additive = BuildAdditiveGraph().Evaluate(0);
        var cached = BuildCachedGraph().Evaluate(0);
        var blend = BuildBlendGraph().Evaluate(0);
        var space = BuildBlendSpaceGraph().Evaluate(365);
        var layer = BuildLayerGraph().Evaluate(0);
        var slot = BuildSlotGraph().Evaluate(0);

        Transform.Position = new(
            Flag(additive.Clip == "A" && additive.AdditiveClip == "B" &&
                MathF.Abs(additive.AdditiveAlpha - 0.5f) < 1e-4f && additive.SaveCachedPose == "Loco"),
            Flag(cached.UseCachedPose == "Loco" && cached.Clip.Length == 0),
            Flag(blend.Clip == "A" && blend.BlendClip == "B" &&
                MathF.Abs(blend.BlendWeight - 0.4f) < 1e-4f));
        Transform.Rotation = new(
            Flag(space.Clip == "Walk" && space.BlendClip == "Run" &&
                MathF.Abs(space.BlendWeight - 0.5f) < 1e-4f),
            Flag(layer.LayerClip == "B" && layer.LayerWeights.Count == 1 &&
                layer.LayerWeights[0].Bone == "hip" &&
                MathF.Abs(layer.LayerWeights[0].Weight - 0.5f) < 1e-4f),
            Flag(slot.Slot == "DefaultSlot" && slot.SlotWeights.Count == 1));
        Transform.Scale = new(
            Flag(Throws(CycleGraph())),
            Flag(Throws(UnknownInputGraph())),
            Flag(Throws(UnsupportedBlendGraph())));
    }

    private static AnimationPoseGraph BuildAdditiveGraph()
    {
        var graph = new AnimationPoseGraph
        {
            Name = "Additive", OutputNode = "out",
            Nodes = new[]
            {
                Player("a", "A"),
                Player("b", "B", loop: false),
                new AnimationPoseNode { Id = "add", Kind = AnimationPoseNodeKind.ApplyAdditive,
                    Inputs = new[] { "a", "b" }, Alpha = 0.5f },
                new AnimationPoseNode { Id = "save", Kind = AnimationPoseNodeKind.SaveCachedPose,
                    Inputs = new[] { "add" }, CacheName = "Loco" },
                new AnimationPoseNode { Id = "out", Kind = AnimationPoseNodeKind.Output,
                    Inputs = new[] { "save" } }
            }
        };
        return graph;
    }

    private static AnimationPoseGraph BuildCachedGraph()
    {
        var graph = new AnimationPoseGraph
        {
            Name = "Cached", OutputNode = "out",
            Nodes = new[]
            {
                Player("a", "A"),
                new AnimationPoseNode { Id = "save", Kind = AnimationPoseNodeKind.SaveCachedPose,
                    Inputs = new[] { "a" }, CacheName = "Loco" },
                new AnimationPoseNode { Id = "use", Kind = AnimationPoseNodeKind.UseCachedPose,
                    CacheName = "Loco" },
                new AnimationPoseNode { Id = "out", Kind = AnimationPoseNodeKind.Output,
                    Inputs = new[] { "use" } }
            }
        };
        graph.Validate();
        return graph;
    }

    private static AnimationPoseGraph BuildBlendGraph() => BuildTwoPlayerGraph(
        "Blend", AnimationPoseNodeKind.Blend, 0.4f);

    private static AnimationPoseGraph BuildLayerGraph() => BuildTwoPlayerGraph(
        "Layer", AnimationPoseNodeKind.LayeredBoneBlend, 1,
        new[] { new AnimationGraphBoneWeight("hip", 0.5f) });

    private static AnimationPoseGraph BuildSlotGraph() => new()
    {
        Name = "Slot", OutputNode = "out",
        Nodes = new[]
        {
            Player("a", "A"),
            new AnimationPoseNode { Id = "slot", Kind = AnimationPoseNodeKind.Slot,
                Inputs = new[] { "a" }, SlotName = "DefaultSlot", Alpha = 1,
                Weights = new[] { new AnimationGraphBoneWeight("hip", 1) } },
            new AnimationPoseNode { Id = "out", Kind = AnimationPoseNodeKind.Output,
                Inputs = new[] { "slot" } }
        }
    };

    private static AnimationPoseGraph BuildTwoPlayerGraph(string name, AnimationPoseNodeKind kind,
        float alpha, IReadOnlyList<AnimationGraphBoneWeight>? weights = null)
    {
        var graph = new AnimationPoseGraph
        {
            Name = name, OutputNode = "out",
            Nodes = new[]
            {
                Player("a", "A"),
                Player("b", "B"),
                new AnimationPoseNode { Id = "op", Kind = kind,
                    Inputs = new[] { "a", "b" }, Alpha = alpha,
                    Weights = weights ?? Array.Empty<AnimationGraphBoneWeight>() },
                new AnimationPoseNode { Id = "out", Kind = AnimationPoseNodeKind.Output,
                    Inputs = new[] { "op" } }
            }
        };
        graph.Validate();
        return graph;
    }

    private static AnimationPoseGraph BuildBlendSpaceGraph()
    {
        var graph = new AnimationPoseGraph
        {
            Name = "BlendSpace", OutputNode = "out",
            Nodes = new[]
            {
                new AnimationPoseNode { Id = "space", Kind = AnimationPoseNodeKind.BlendSpacePlayer,
                    BlendSamples = new[]
                    {
                        new AnimationGraphBlendSample("Walk", 230),
                        new AnimationGraphBlendSample("Run", 500)
                    } },
                new AnimationPoseNode { Id = "out", Kind = AnimationPoseNodeKind.Output,
                    Inputs = new[] { "space" } }
            }
        };
        graph.Validate();
        return graph;
    }

    private static AnimationPoseGraph CycleGraph()
    {
        var graph = new AnimationPoseGraph
        {
            Name = "Cycle", OutputNode = "out",
            Nodes = new[]
            {
                new AnimationPoseNode { Id = "a", Kind = AnimationPoseNodeKind.Blend,
                    Inputs = new[] { "b", "b" } },
                new AnimationPoseNode { Id = "b", Kind = AnimationPoseNodeKind.Blend,
                    Inputs = new[] { "a", "a" } },
                new AnimationPoseNode { Id = "out", Kind = AnimationPoseNodeKind.Output,
                    Inputs = new[] { "a" } }
            }
        };
        return graph;
    }

    private static AnimationPoseGraph UnknownInputGraph()
    {
        var graph = new AnimationPoseGraph
        {
            Name = "Unknown", OutputNode = "out",
            Nodes = new[]
            {
                new AnimationPoseNode { Id = "out", Kind = AnimationPoseNodeKind.Output,
                    Inputs = new[] { "missing" } }
            }
        };
        return graph;
    }

    private static AnimationPoseGraph UnsupportedBlendGraph()
    {
        var graph = new AnimationPoseGraph
        {
            Name = "UnsupportedBlend", OutputNode = "out",
            Nodes = new[]
            {
                new AnimationPoseNode { Id = "space", Kind = AnimationPoseNodeKind.BlendSpacePlayer,
                    BlendSamples = new[]
                    {
                        new AnimationGraphBlendSample("Walk", 230),
                        new AnimationGraphBlendSample("Run", 500)
                    } },
                Player("b", "B"),
                new AnimationPoseNode { Id = "blend", Kind = AnimationPoseNodeKind.Blend,
                    Inputs = new[] { "space", "b" } },
                new AnimationPoseNode { Id = "out", Kind = AnimationPoseNodeKind.Output,
                    Inputs = new[] { "blend" } }
            }
        };
        graph.Validate();
        return graph;
    }

    private static AnimationPoseNode Player(string id, string clip, bool loop = true) => new()
    {
        Id = id, Kind = AnimationPoseNodeKind.SequencePlayer, Clip = clip, Loop = loop
    };

    private static bool Throws(AnimationPoseGraph graph)
    {
        try
        {
            graph.Validate();
            graph.Evaluate(0);
            return false;
        }
        catch (Exception)
        {
            return true;
        }
    }

    // 失败值不能是 0：宿主会拒绝零缩放。
    private static float Flag(bool ok) => ok ? 1 : 2;
}
