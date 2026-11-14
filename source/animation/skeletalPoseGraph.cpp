#include "skeletalPlaybackInternal.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_set>

namespace VL::Animation
{
namespace
{
bool IsPlayer(SkeletalPlayback::PoseGraphNode::Kind kind)
{
    return kind == SkeletalPlayback::PoseGraphNode::Kind::SequencePlayer ||
        kind == SkeletalPlayback::PoseGraphNode::Kind::BlendSpacePlayer;
}

float GraphSmoothstep(float axis, float from, float to)
{
    const float span = to - from;
    const float alpha = span > 0 ? std::clamp((axis - from) / span, 0.0f, 1.0f) : 0.0f;
    return alpha * alpha * (3 - 2 * alpha);
}
} // namespace

void SkeletalPlayback::SetPoseGraph(std::vector<PoseGraphNode> nodes)
{
    ClearPoseGraph();
    if (nodes.empty())
        throw std::invalid_argument("Pose graph has no nodes");
    const auto& clips = asset->AnimationAsset().clips;
    poseGraphNodes = std::move(nodes);
    poseGraphIndex.clear();
    poseGraphStates.assign(poseGraphNodes.size(), {});
    size_t outputs = 0;
    for (size_t index = 0; index < poseGraphNodes.size(); ++index)
    {
        const PoseGraphNode& node = poseGraphNodes[index];
        if (node.id.empty() || node.id.size() > 128)
            throw std::invalid_argument("Pose graph node id is invalid");
        if (!poseGraphIndex.emplace(node.id, index).second)
            throw std::invalid_argument("Duplicate pose graph node: " + node.id);
        if (node.kind == PoseGraphNode::Kind::Output && ++outputs > 1)
            throw std::invalid_argument("Pose graph must have exactly one Output");
        if (!std::isfinite(node.rate) || node.rate <= 0 || node.rate > 4)
            throw std::invalid_argument("Pose graph node rate is invalid: " + node.id);
        if (node.kind == PoseGraphNode::Kind::SequencePlayer)
        {
            size_t clip = 0;
            while (clip < clips.size() && clips[clip].id != node.clip) ++clip;
            if (clip == clips.size())
                throw std::invalid_argument("Unknown pose graph clip: " + node.clip);
            if (!std::isfinite(node.startPosition) || node.startPosition < 0)
                throw std::invalid_argument("Pose graph start position is invalid: " + node.id);
        }
        else if (node.kind == PoseGraphNode::Kind::BlendSpacePlayer)
        {
            if (node.samples.size() < 2 || node.samples.size() > 3)
                throw std::invalid_argument("Pose graph blend space requires 2..3 samples: " + node.id);
            for (size_t sample = 0; sample < node.samples.size(); ++sample)
            {
                size_t clip = 0;
                while (clip < clips.size() && clips[clip].id != node.samples[sample].clip)
                    ++clip;
                if (clip == clips.size())
                    throw std::invalid_argument(
                        "Unknown pose graph sample clip: " + node.samples[sample].clip);
                if (sample > 0 && !(node.samples[sample].threshold >
                    node.samples[sample - 1].threshold))
                    throw std::invalid_argument(
                        "Pose graph sample thresholds must increase: " + node.id);
            }
        }
        else
        {
            if (!std::isfinite(node.alpha) || node.alpha < 0 || node.alpha > 1)
                throw std::invalid_argument("Pose graph alpha is invalid: " + node.id);
        }
        if ((node.kind == PoseGraphNode::Kind::SaveCachedPose ||
             node.kind == PoseGraphNode::Kind::UseCachedPose) &&
            (node.cacheName.empty() || node.cacheName.size() > 128))
            throw std::invalid_argument("Pose graph cache name is invalid: " + node.id);
    }
    if (outputs != 1)
        throw std::invalid_argument("Pose graph must have exactly one Output");
    for (size_t index = 0; index < poseGraphNodes.size(); ++index)
    {
        const PoseGraphNode& node = poseGraphNodes[index];
        if (node.kind == PoseGraphNode::Kind::Output) poseGraphOutput = index;
        for (const std::string& input : node.inputs)
            if (poseGraphIndex.find(input) == poseGraphIndex.end())
                throw std::invalid_argument(
                    "Pose graph node '" + node.id + "' references unknown input '" + input + "'");
        if (node.kind == PoseGraphNode::Kind::Output)
        {
            if (node.inputs.size() != 1)
                throw std::invalid_argument("Pose graph Output requires one input");
        }
    }
    RebuildPoseGraphOrder();
    const PoseGraphNode& output = poseGraphNodes.at(poseGraphOutput);
    if (!output.rootSource.empty() &&
        poseGraphIndex.find(output.rootSource) == poseGraphIndex.end())
        throw std::invalid_argument("Pose graph root source is unknown: " + output.rootSource);
    poseGraphRoot = static_cast<size_t>(-1);
    if (!output.rootSource.empty())
        poseGraphRoot = poseGraphIndex.at(output.rootSource);
    else
        for (size_t index = 0; index < poseGraphNodes.size(); ++index)
            if (poseGraphNodes[index].kind == PoseGraphNode::Kind::SequencePlayer)
            {
                poseGraphRoot = index;
                break;
            }
    if (poseGraphRoot >= poseGraphNodes.size() ||
        poseGraphNodes[poseGraphRoot].kind != PoseGraphNode::Kind::SequencePlayer)
        throw std::invalid_argument("Pose graph root source must be a SequencePlayer");
    // 根片段同时作为旧 notify/根位移通道的来源。
    size_t rootClip = 0;
    const std::string& rootId = poseGraphNodes[poseGraphRoot].clip;
    while (rootClip < clips.size() && clips[rootClip].id != rootId) ++rootClip;
    selected = rootClip;
    time = poseGraphNodes[poseGraphRoot].startPosition;
    looping = poseGraphNodes[poseGraphRoot].loop;
    playbackRangeStart = 0;
    playbackRangeEnd = 0;
    poseGraphStates[poseGraphRoot].time = time;
    RebuildNotifyEvents();
    poseGraphActive = true;
}

void SkeletalPlayback::ClearPoseGraph()
{
    poseGraphActive = false;
    poseGraphNodes.clear();
    poseGraphIndex.clear();
    poseGraphOrder.clear();
    poseGraphStates.clear();
    poseGraphOutput = static_cast<size_t>(-1);
    poseGraphRoot = static_cast<size_t>(-1);
    poseGraphValues.clear();
    poseGraphCachedPoses.clear();
    poseGraphNodePoses.clear();
    poseGraphRootValid = false;
}

void SkeletalPlayback::SetPoseGraphValue(const std::string& id, float value)
{
    if (poseGraphIndex.find(id) == poseGraphIndex.end())
        throw std::invalid_argument("Unknown pose graph node: " + id);
    if (!std::isfinite(value))
        throw std::invalid_argument("Pose graph value must be finite");
    poseGraphValues[id] = value;
}

void SkeletalPlayback::RebuildPoseGraphOrder()
{
    poseGraphOrder.clear();
    std::vector<uint8_t> marks(poseGraphNodes.size(), 0);
    std::function<void(size_t)> visit = [&](size_t index)
    {
        if (marks[index] == 1)
            throw std::invalid_argument("Pose graph contains a cycle");
        if (marks[index] == 2) return;
        marks[index] = 1;
        for (const std::string& input : poseGraphNodes[index].inputs)
            visit(poseGraphIndex.at(input));
        marks[index] = 2;
        poseGraphOrder.push_back(index);
    };
    for (size_t index = 0; index < poseGraphNodes.size(); ++index) visit(index);
}

LocalPose SkeletalPlayback::EvaluatePoseGraph(float deltaSeconds)
{
    const auto& clips = asset->AnimationAsset().clips;
    const size_t jointCount = asset->mesh.joints.size();
    poseGraphNodePoses.clear();
    const LocalPose reference = [&]
    {
        const auto rest = asset->mesh.runtime->joint_rest_poses();
        return LocalPose(rest.begin(), rest.end());
    }();
    poseGraphRootValid = false;
    for (size_t index : poseGraphOrder)
    {
        const PoseGraphNode& node = poseGraphNodes[index];
        PoseGraphPlayerState& state = poseGraphStates[index];
        auto pose = [&](const std::string& id) -> const LocalPose&
        {
            const auto found = poseGraphNodePoses.find(id);
            if (found == poseGraphNodePoses.end())
                throw std::runtime_error("Pose graph input was not evaluated: " + id);
            return found->second;
        };
        if (node.kind == PoseGraphNode::Kind::SequencePlayer)
        {
            size_t clip = 0;
            while (clip < clips.size() && clips[clip].id != node.clip) ++clip;
            const float duration = clips.at(clip).runtime->duration();
            const float previous = state.time;
            state.time += deltaSeconds * node.rate;
            if (node.loop)
                state.time = duration > 0 ? std::fmod(state.time, duration) : 0.0f;
            else
                state.time = std::min(state.time, std::max(0.0f, duration - 1e-4f));
            if (index == poseGraphRoot)
            {
                poseGraphRootPreviousTime = previous;
                poseGraphRootValid = true;
            }
            poseGraphNodePoses[node.id] = Sample(clip, state.time);
        }
        else if (node.kind == PoseGraphNode::Kind::BlendSpacePlayer)
        {
            const size_t baseIndex = node.samples.size() >= 3 ? 1 : 0;
            size_t baseClip = 0;
            while (baseClip < clips.size() && clips[baseClip].id != node.samples[baseIndex].clip)
                ++baseClip;
            const float duration = clips.at(baseClip).runtime->duration();
            if (index == poseGraphRoot)
            {
                poseGraphRootPreviousTime = state.time;
                poseGraphRootValid = true;
            }
            state.time += deltaSeconds * node.rate;
            if (node.loop)
                state.time = duration > 0 ? std::fmod(state.time, duration) : 0.0f;
            else
                state.time = std::min(state.time, std::max(0.0f, duration - 1e-4f));
            const auto value = poseGraphValues.find(node.id);
            const float axis = value != poseGraphValues.end() ? value->second : 0.0f;
            size_t first = baseIndex;
            size_t second = baseIndex;
            float target = 0;
            if (node.samples.size() == 1) first = second = 0;
            else if (baseIndex == 0)
            {
                first = 0; second = 1;
                target = GraphSmoothstep(axis, node.samples[0].threshold,
                    node.samples[1].threshold);
            }
            else if (axis <= node.samples[1].threshold)
            {
                first = 1; second = 0;
                target = 1 - GraphSmoothstep(axis, node.samples[0].threshold,
                    node.samples[1].threshold);
            }
            else
            {
                first = 1; second = 2;
                target = GraphSmoothstep(axis, node.samples[1].threshold,
                    node.samples[2].threshold);
            }
            if (!state.weightValid) { state.weight = target; state.weightValid = true; }
            else
            {
                const float step = 5.0f * std::max(0.0f, deltaSeconds);
                state.weight = std::clamp(target, state.weight - step, state.weight + step);
            }
            size_t firstClip = 0, secondClip = 0;
            while (firstClip < clips.size() && clips[firstClip].id != node.samples[first].clip)
                ++firstClip;
            while (secondClip < clips.size() && clips[secondClip].id != node.samples[second].clip)
                ++secondClip;
            LocalPose firstPose = Sample(firstClip, state.time);
            const float phase = duration > 0 ? std::clamp(state.time / duration, 0.0f, 1.0f) : 0;
            const float secondDuration = clips.at(secondClip).runtime->duration();
            LocalPose secondPose = Sample(secondClip, phase * secondDuration);
            if (state.weight <= 0) poseGraphNodePoses[node.id] = std::move(firstPose);
            else if (state.weight >= 1) poseGraphNodePoses[node.id] = std::move(secondPose);
            else poseGraphNodePoses[node.id] = Crossfade(firstPose, std::move(secondPose),
                state.weight, asset->mesh);
        }
        else if (node.kind == PoseGraphNode::Kind::Blend)
        {
            const auto value = poseGraphValues.find(node.id);
            const float alpha = value != poseGraphValues.end() ?
                std::clamp(value->second, 0.0f, 1.0f) : node.alpha;
            poseGraphNodePoses[node.id] = Crossfade(pose(node.inputs.at(0)),
                pose(node.inputs.at(1)), alpha, asset->mesh);
        }
        else if (node.kind == PoseGraphNode::Kind::ApplyAdditive)
        {
            const auto value = poseGraphValues.find(node.id);
            const float alpha = value != poseGraphValues.end() ?
                std::clamp(value->second, 0.0f, 1.0f) : node.alpha;
            const PoseGraphNode& additiveNode = poseGraphNodes.at(
                poseGraphIndex.at(node.inputs.at(1)));
            const std::string& additiveClipId = additiveNode.clip;
            size_t additiveClip = 0;
            while (additiveClip < clips.size() && clips[additiveClip].id != additiveClipId)
                ++additiveClip;
            if (additiveClip == clips.size())
                throw std::invalid_argument("Pose graph additive clip is unknown");
            const std::string& type = clips.at(additiveClip).sequence.additiveAnimType;
            if (type == "RotationOffsetMeshSpace")
                poseGraphNodePoses[node.id] = ApplyMeshSpaceRotationAdditive(
                    pose(node.inputs.at(0)), pose(node.inputs.at(1)), reference, alpha,
                    asset->mesh.joints);
            else if (type == "LocalSpaceBase")
                poseGraphNodePoses[node.id] = ApplyLocalSpaceAdditive(
                    pose(node.inputs.at(0)), pose(node.inputs.at(1)), reference, alpha,
                    jointCount);
            else
                throw std::invalid_argument(
                    "Pose graph additive clip must be LocalSpaceBase or RotationOffsetMeshSpace");
        }
        else if (node.kind == PoseGraphNode::Kind::LayeredBoneBlend)
        {
            std::vector<float> weights(jointCount, 0.0f);
            for (const BlendProfileBone& bone : node.weights)
            {
                size_t joint = 0;
                while (joint < jointCount && asset->mesh.joints[joint].name != bone.name)
                    ++joint;
                if (joint == jointCount)
                    throw std::invalid_argument("Pose graph layer bone is unknown: " + bone.name);
                weights[joint] = std::clamp(bone.weight, 0.0f, 1.0f);
            }
            const auto value = poseGraphValues.find(node.id);
            const float alpha = value != poseGraphValues.end() ?
                std::clamp(value->second, 0.0f, 1.0f) : node.alpha;
            poseGraphNodePoses[node.id] = ApplyLayeredWeights(pose(node.inputs.at(0)),
                pose(node.inputs.at(1)), weights, alpha, jointCount);
        }
        else if (node.kind == PoseGraphNode::Kind::SaveCachedPose)
            poseGraphCachedPoses[node.cacheName] = pose(node.inputs.at(0));
        else if (node.kind == PoseGraphNode::Kind::UseCachedPose)
        {
            const auto found = poseGraphCachedPoses.find(node.cacheName);
            if (found == poseGraphCachedPoses.end())
                throw std::runtime_error("Pose graph cached pose is missing: " + node.cacheName);
            poseGraphNodePoses[node.id] = found->second;
        }
        else if (node.kind == PoseGraphNode::Kind::Slot)
        {
            // Montage slot overlay 仍由宿主播放层承担；图内 Slot 只作为插入点。
            poseGraphNodePoses[node.id] = pose(node.inputs.at(0));
        }
        else if (node.kind == PoseGraphNode::Kind::Output)
        {
            poseGraphNodePoses[node.id] = pose(node.inputs.at(0));
        }
    }
    return poseGraphNodePoses.at(poseGraphNodes.at(poseGraphOutput).id);
}
} // namespace VL::Animation
