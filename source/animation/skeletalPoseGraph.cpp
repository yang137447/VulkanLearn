#include "skeletalPlaybackInternal.h"

#include "animationMontage.h"
#include "animationScriptData.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace VL::Animation
{
namespace
{
bool IsPlayer(SkeletalPlayback::PoseGraphNode::Kind kind)
{
    return kind == SkeletalPlayback::PoseGraphNode::Kind::SequencePlayer ||
        kind == SkeletalPlayback::PoseGraphNode::Kind::BlendSpacePlayer ||
        kind == SkeletalPlayback::PoseGraphNode::Kind::BlendSpace2DPlayer ||
        kind == SkeletalPlayback::PoseGraphNode::Kind::MontagePlayer;
}

bool IsRootPlayer(SkeletalPlayback::PoseGraphNode::Kind kind)
{
    return kind == SkeletalPlayback::PoseGraphNode::Kind::SequencePlayer ||
        kind == SkeletalPlayback::PoseGraphNode::Kind::BlendSpacePlayer ||
        kind == SkeletalPlayback::PoseGraphNode::Kind::BlendSpace2DPlayer ||
        kind == SkeletalPlayback::PoseGraphNode::Kind::MontagePlayer;
}

float GraphSmoothstep(float axis, float from, float to)
{
    const float span = to - from;
    const float alpha = span > 0 ? std::clamp((axis - from) / span, 0.0f, 1.0f) : 0.0f;
    return alpha * alpha * (3 - 2 * alpha);
}

size_t FindClip(const std::vector<Clip>& clips, const std::string& id)
{
    size_t index = 0;
    while (index < clips.size() && clips[index].id != id) ++index;
    return index;
}

std::string LowerAscii(std::string value)
{
    for (char& character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

struct BlendGrid2D
{
    float x[2]{};
    float y[2]{};
    const SkeletalPlayback::PoseGraphSample* corners[2][2]{};
};

BlendGrid2D BuildBlendGrid2D(const std::vector<SkeletalPlayback::PoseGraphSample>& samples,
    const std::string& nodeId)
{
    std::set<float> xs;
    std::set<float> ys;
    for (const auto& sample : samples)
    {
        xs.insert(sample.x);
        ys.insert(sample.y);
    }
    if (xs.size() != 2 || ys.size() != 2)
        throw std::invalid_argument(
            "Pose graph blend space 2D samples must form a 2x2 grid: " + nodeId);
    BlendGrid2D grid;
    std::copy(xs.begin(), xs.end(), grid.x);
    std::copy(ys.begin(), ys.end(), grid.y);
    for (const auto& sample : samples)
    {
        const size_t x = sample.x == grid.x[0] ? 0 : 1;
        const size_t y = sample.y == grid.y[0] ? 0 : 1;
        if (grid.corners[y][x])
            throw std::invalid_argument(
                "Pose graph blend space 2D repeats a grid coordinate: " + nodeId);
        grid.corners[y][x] = &sample;
    }
    for (size_t y = 0; y < 2; ++y)
        for (size_t x = 0; x < 2; ++x)
            if (!grid.corners[y][x])
                throw std::invalid_argument(
                    "Pose graph blend space 2D must cover all four corners: " + nodeId);
    return grid;
}

const Montage& FindPoseGraphMontage(const SkeletalAsset& animation, const std::string& id,
    const std::string& nodeId)
{
    const Montage* montage = animation.FindMontage(id);
    if (!montage)
        throw std::invalid_argument(
            "Pose graph montage node '" + nodeId + "' references unknown montage '" + id + "'");
    return *montage;
}

void ValidateMontageNode(const SkeletalPlayback::PoseGraphNode& node,
    const SkeletalAsset& animation)
{
    if (node.montage.empty() || node.montage.size() > 256)
        throw std::invalid_argument("Pose graph montage name is invalid: " + node.id);
    const Montage& montage = FindPoseGraphMontage(animation, node.montage, node.id);
    if (montage.sections.empty())
        throw std::invalid_argument("Pose graph montage has no sections: " + node.id);
    std::unordered_set<std::string> sections;
    for (const MontageSection& section : montage.sections)
    {
        if (section.name.empty() || !sections.insert(section.name).second)
            throw std::invalid_argument("Pose graph montage section is invalid: " + node.id);
    }
    for (const MontageSection& section : montage.sections)
        if (!section.nextSection.empty() &&
            sections.find(section.nextSection) == sections.end())
            throw std::invalid_argument(
                "Pose graph montage references unknown section '" + section.nextSection +
                "': " + node.id);
    if (!std::isfinite(node.startPosition) || node.startPosition < 0)
        throw std::invalid_argument("Pose graph montage start position is invalid: " + node.id);
    const MontageSection& first = montage.sections.front();
    if (node.startPosition > 0 &&
        (node.startPosition < first.startTime || node.startPosition >= first.endTime))
        throw std::invalid_argument(
            "Pose graph montage start position is outside the first section: " + node.id);
    const auto& clips = animation.clips;
    const auto requireClip = [&](const std::string& clip)
    {
        if (clip.empty() || FindClip(clips, clip) == clips.size())
            throw std::invalid_argument(
                "Pose graph montage requires loaded clip '" + clip + "': " + node.id);
    };
    if (montage.segments.empty())
        requireClip(montage.derivedClipId);
    else
        for (const MontageSegment& segment : montage.segments)
            requireClip(segment.derivedClipId);
}

std::vector<float> ResolveSlotWeights(const std::vector<BlendProfileBone>& bones,
    const std::vector<Joint>& joints, const std::string& nodeId)
{
    // 空 weights 在契约中表示全身覆盖，不是“没有遮罩”。
    if (bones.empty()) return std::vector<float>(joints.size(), 1.0f);
    std::vector<float> weights(joints.size(), 0.0f);
    for (const BlendProfileBone& bone : bones)
    {
        size_t joint = 0;
        while (joint < joints.size() && joints[joint].name != bone.name) ++joint;
        if (joint == joints.size())
            throw std::invalid_argument(
                "Pose graph slot bone is unknown: " + bone.name + " (" + nodeId + ")");
        weights[joint] = std::clamp(bone.weight, 0.0f, 1.0f);
    }
    return weights;
}
} // namespace

size_t SkeletalPlayback::ResolvePoseGraphRoot(size_t entry) const
{
    if (entry >= poseGraphNodes.size()) return static_cast<size_t>(-1);
    std::unordered_set<size_t> visited;
    const auto resolve = [&](const auto& self, size_t index, int depth) -> size_t
    {
        if (index >= poseGraphNodes.size() || depth > 64 || !visited.insert(index).second)
            return static_cast<size_t>(-1);
        const PoseGraphNode& node = poseGraphNodes[index];
        // 图节点显式声明的 rootSource 优先；Output 的旧资产也依赖这个字段。
        if (!node.rootSource.empty())
        {
            const auto source = poseGraphIndex.find(node.rootSource);
            if (source != poseGraphIndex.end())
            {
                const size_t resolved = self(self, source->second, depth + 1);
                if (resolved < poseGraphNodes.size()) return resolved;
            }
        }
        if (IsRootPlayer(node.kind)) return index;
        if (node.kind == PoseGraphNode::Kind::Select && node.inputs.size() == 2)
        {
            const auto value = poseGraphValues.find(node.id);
            const bool second = value != poseGraphValues.end() && value->second >= 0.5f;
            const size_t preferred = self(self, poseGraphIndex.at(node.inputs[second ? 1 : 0]), depth + 1);
            if (preferred < poseGraphNodes.size()) return preferred;
            return self(self, poseGraphIndex.at(node.inputs[second ? 0 : 1]), depth + 1);
        }
        for (const std::string& input : node.inputs)
        {
            const auto found = poseGraphIndex.find(input);
            if (found == poseGraphIndex.end()) continue;
            const size_t resolved = self(self, found->second, depth + 1);
            if (resolved < poseGraphNodes.size()) return resolved;
        }
        return static_cast<size_t>(-1);
    };
    return resolve(resolve, entry, 0);
}

void SkeletalPlayback::ResetPoseGraphRootState(size_t root, float startPosition)
{
    if (root >= poseGraphNodes.size()) throw std::invalid_argument("Pose graph root is invalid");
    const PoseGraphNode& node = poseGraphNodes[root];
    const auto& clips = asset->AnimationAsset().clips;
    PoseGraphPlayerState& state = poseGraphStates.at(root);
    // 上一个一次性状态完成时会把 playing 置 false；入口切换必须重新启动目标
    // 播放器，否则 Fall/Land 会停在 time=0。显式 Pause 仍保持暂停。
    if (!explicitlyPaused) playing = true;
    const auto clipIndex = [&](const std::string& clip)
    {
        const size_t index = FindClip(clips, clip);
        if (index >= clips.size())
            throw std::invalid_argument("Unknown pose graph root clip: " + clip);
        return index;
    };
    const auto clampTime = [&](size_t clip, bool loop)
    {
        const float duration = clips.at(clip).runtime->duration();
        if (duration <= 0) return 0.0f;
        if (loop) return std::clamp(startPosition, 0.0f,
            std::max(0.0f, duration - 1e-4f));
        // 非循环根源必须允许到达 duration，Completed 判定以 rangeEnd 为界；
        // 若停在 duration-1e-4，原生图的一次性状态永远不会报告完成。
        return std::clamp(startPosition, 0.0f, std::max(0.0f, duration));
    };
    if (node.kind == PoseGraphNode::Kind::SequencePlayer)
    {
        selected = clipIndex(node.clip);
        looping = node.loop;
        playbackRangeStart = 0;
        playbackRangeEnd = 0;
        state.time = clampTime(selected, looping);
        time = state.time;
        poseGraphRootPreviousTime = state.time;
        poseGraphRootMontage = false;
        RebuildNotifyEvents();
    }
    else if (node.kind == PoseGraphNode::Kind::BlendSpacePlayer)
    {
        const size_t baseIndex = node.samples.size() >= 3 ? 1 : 0;
        selected = clipIndex(node.samples.at(baseIndex).clip);
        looping = node.loop;
        playbackRangeStart = 0;
        playbackRangeEnd = 0;
        state.time = clampTime(selected, looping);
        state.weightValid = false;
        time = state.time;
        poseGraphRootPreviousTime = state.time;
        poseGraphRootMontage = false;
        RebuildNotifyEvents();
    }
    else if (node.kind == PoseGraphNode::Kind::BlendSpace2DPlayer)
    {
        BuildBlendGrid2D(node.samples, node.id);
        // 契约的 2D 参考片段是声明顺序第一个 sample；它不一定是最小 X/Y 角点。
        selected = clipIndex(node.samples.front().clip);
        looping = node.loop;
        playbackRangeStart = 0;
        playbackRangeEnd = 0;
        state.time = clampTime(selected, looping);
        time = state.time;
        poseGraphRootPreviousTime = state.time;
        poseGraphRootMontage = false;
        RebuildNotifyEvents();
    }
    else if (node.kind == PoseGraphNode::Kind::MontagePlayer)
    {
        const Montage& montage = FindPoseGraphMontage(asset->AnimationAsset(), node.montage, node.id);
        const std::string firstClip = montage.segments.empty()
            ? montage.derivedClipId : montage.segments.front().derivedClipId;
        selected = clipIndex(firstClip);
        looping = false;
        playbackRangeStart = 0;
        playbackRangeEnd = 0;
        state.montageActive = true;
        state.montageCompleted = false;
        state.montageInitialized = false;
        state.montagePlayer.reset();
        state.montageStartPosition = startPosition;
        time = startPosition;
        poseGraphRootPreviousTime = startPosition;
        poseGraphRootMontage = true;
        RebuildNotifyEvents();
    }
    else
    {
        throw std::invalid_argument("Pose graph root must be a player node");
    }
    // 入口切换后根源已经是一个确定的 Player；Tick 的根源反馈分支可以先读取
    // 这个时钟。EvaluatePoseGraph 仍会在推进后刷新 previous time。
    poseGraphRootValid = true;
    poseGraphRoot = root;
}

void SkeletalPlayback::SetPoseGraph(std::vector<PoseGraphNode> nodes)
{
    ClearPoseGraph();
    if (nodes.empty())
        throw std::invalid_argument("Pose graph has no nodes");
    const SkeletalAsset& animation = asset->AnimationAsset();
    const auto& clips = animation.clips;
    poseGraphNodes = std::move(nodes);
    poseGraphIndex.clear();
    poseGraphStates.assign(poseGraphNodes.size(), {});
    size_t outputs = 0;
    std::unordered_map<std::string, size_t> saveProducers;
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
        if (!std::isfinite(node.weightSpeedPerSec) ||
            node.weightSpeedPerSec < 0 || node.weightSpeedPerSec > 100)
            throw std::invalid_argument(
                "Pose graph blend weight speed is invalid: " + node.id);
        if (node.kind == PoseGraphNode::Kind::SequencePlayer)
        {
            if (FindClip(clips, node.clip) == clips.size())
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
                if (FindClip(clips, node.samples[sample].clip) == clips.size())
                    throw std::invalid_argument(
                        "Unknown pose graph sample clip: " + node.samples[sample].clip);
                if (sample > 0 && !(node.samples[sample].threshold >
                    node.samples[sample - 1].threshold))
                    throw std::invalid_argument(
                        "Pose graph sample thresholds must increase: " + node.id);
                if (!std::isfinite(node.samples[sample].rate) ||
                    node.samples[sample].rate <= 0 || node.samples[sample].rate > 4)
                    throw std::invalid_argument(
                        "Pose graph sample rate is invalid: " + node.id);
                if (node.samples[sample].syncGroup.size() > 64)
                    throw std::invalid_argument(
                        "Pose graph sample sync group is too long: " + node.id);
            }
        }
        else if (node.kind == PoseGraphNode::Kind::BlendSpace2DPlayer)
        {
            if (node.samples.size() != 4)
                throw std::invalid_argument(
                    "Pose graph blend space 2D requires four samples: " + node.id);
            for (const PoseGraphSample& sample : node.samples)
            {
                if (FindClip(clips, sample.clip) == clips.size())
                    throw std::invalid_argument(
                        "Unknown pose graph sample clip: " + sample.clip);
                if (!std::isfinite(sample.x) || !std::isfinite(sample.y))
                    throw std::invalid_argument(
                        "Pose graph blend space 2D sample coordinates must be finite: " + node.id);
                if (!std::isfinite(sample.rate) || sample.rate <= 0 || sample.rate > 4)
                    throw std::invalid_argument(
                        "Pose graph sample rate is invalid: " + node.id);
                if (sample.syncGroup.size() > 64)
                    throw std::invalid_argument(
                        "Pose graph sample sync group is too long: " + node.id);
            }
            BuildBlendGrid2D(node.samples, node.id);
        }
        else if (node.kind == PoseGraphNode::Kind::MontagePlayer)
        {
            ValidateMontageNode(node, animation);
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
        if (node.kind == PoseGraphNode::Kind::SaveCachedPose)
        {
            if (!saveProducers.emplace(node.cacheName, index).second)
                throw std::invalid_argument(
                    "Duplicate pose graph cached pose: " + node.cacheName);
        }
        if (node.kind == PoseGraphNode::Kind::Slot &&
            (node.slotName.empty() || node.slotName.size() > 64))
            throw std::invalid_argument("Pose graph slot name is invalid: " + node.id);
        if (node.kind == PoseGraphNode::Kind::MontagePlayer)
            poseGraphStates[index].montageStartPosition = node.startPosition;
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
        size_t expected = 0;
        switch (node.kind)
        {
        case PoseGraphNode::Kind::SequencePlayer:
        case PoseGraphNode::Kind::BlendSpacePlayer:
        case PoseGraphNode::Kind::BlendSpace2DPlayer:
        case PoseGraphNode::Kind::MontagePlayer:
        case PoseGraphNode::Kind::UseCachedPose:
            expected = 0;
            break;
        case PoseGraphNode::Kind::SaveCachedPose:
        case PoseGraphNode::Kind::Output:
            expected = 1;
            break;
        case PoseGraphNode::Kind::Slot:
            if (node.inputs.size() != 1 && node.inputs.size() != 2)
                throw std::invalid_argument("Pose graph Slot requires one or two inputs");
            if (node.inputs.size() == 2)
            {
                const PoseGraphNode& montage = poseGraphNodes.at(
                    poseGraphIndex.at(node.inputs[1]));
                if (montage.kind != PoseGraphNode::Kind::MontagePlayer)
                    throw std::invalid_argument(
                        "Pose graph Slot second input must be a MontagePlayer");
            }
            expected = node.inputs.size();
            break;
        case PoseGraphNode::Kind::Blend:
        case PoseGraphNode::Kind::Select:
        case PoseGraphNode::Kind::ApplyAdditive:
        case PoseGraphNode::Kind::LayeredBoneBlend:
            expected = 2;
            break;
        }
        if (node.inputs.size() != expected)
            throw std::invalid_argument(
                "Pose graph node '" + node.id + "' requires " +
                std::to_string(expected) + " inputs");
    }
    RebuildPoseGraphOrder();
    const PoseGraphNode& output = poseGraphNodes.at(poseGraphOutput);
    if (!output.rootSource.empty() &&
        poseGraphIndex.find(output.rootSource) == poseGraphIndex.end())
        throw std::invalid_argument("Pose graph root source is unknown: " + output.rootSource);
    poseGraphRoot = ResolvePoseGraphRoot(poseGraphOutput);
    if (poseGraphRoot >= poseGraphNodes.size() ||
        !IsRootPlayer(poseGraphNodes[poseGraphRoot].kind))
        throw std::invalid_argument(
            "Pose graph root source must be a player node");
    const PoseGraphNode& root = poseGraphNodes[poseGraphRoot];
    ResetPoseGraphRootState(poseGraphRoot, root.startPosition);
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
    poseGraphEntry = static_cast<size_t>(-1);
    poseGraphValues.clear();
    poseGraphAxisValues.clear();
    poseGraphMontageTargets.clear();
    poseGraphCachedPoses.clear();
    poseGraphNodePoses.clear();
    poseGraphActiveNotifyStates.clear();
    poseGraphRootValid = false;
    poseGraphRootMontage = false;
    poseGraphMontageRootDelta.setZero();
    poseGraphMontageRootYawDelta = 0;
}

void SkeletalPlayback::SetPoseGraphValue(const std::string& id, float value)
{
    const auto found = poseGraphIndex.find(id);
    if (found == poseGraphIndex.end())
        throw std::invalid_argument("Unknown pose graph node: " + id);
    if (!std::isfinite(value))
        throw std::invalid_argument("Pose graph value must be finite");
    const auto previousValue = poseGraphValues.find(id);
    const bool wasPositive = previousValue != poseGraphValues.end() &&
        previousValue->second > 0;
    poseGraphValues[id] = value;
    PoseGraphPlayerState& state = poseGraphStates.at(found->second);
    if (poseGraphNodes[found->second].kind == PoseGraphNode::Kind::BlendSpace2DPlayer)
    {
        // 单值兼容入口只更新 X；已有双轴值时不能把 Y 也覆盖掉。
        auto axis = poseGraphAxisValues.find(id);
        if (axis != poseGraphAxisValues.end())
            axis->second.x() = value;
    }
    if (poseGraphNodes[found->second].kind == PoseGraphNode::Kind::MontagePlayer)
    {
        if (value <= 0)
        {
            state.montageActive = false;
            state.montageCompleted = false;
            state.montageInitialized = false;
            state.montagePlayer.reset();
        }
        else if (!wasPositive)
        {
            // MontagePlayer 的正值是启动边沿，不是“每帧保持运行”的权重。
            // 持续正值不能把已完成的非循环 Montage 自动重播；宿主必须先提交 0
            // 再提交正值，或把节点 Loop 设为 true。
            state.montageActive = true;
            state.montageCompleted = false;
            state.montageInitialized = false;
            state.montagePlayer.reset();
            state.montageStartPosition = poseGraphNodes[found->second].startPosition;
            // 只有根源 Montage 拥有父播放器时钟。Slot overlay 的 base 时钟必须
            // 独立前进，不能被 Montage 的入口时间改写。
            if (found->second == poseGraphRoot)
            {
                // 父播放器可能停在非循环 Montage 的末帧；不重置父 time/范围的话，
                // Tick 的“sampleTime == time”早退会吞掉重播的第一帧。
                time = state.montageStartPosition;
                poseGraphRootPreviousTime = time;
                playbackRangeStart = 0;
                playbackRangeEnd = 0;
                // 一次性 Montage 完成后 Tick 会把 playing 置 false；正权重重新激活时
                // 必须恢复播放，否则下一帧会在入口前直接返回。
                if (!explicitlyPaused) playing = true;
            }
        }
    }
}

void SkeletalPlayback::SetPoseGraphValue(const std::string& id, float valueX, float valueY)
{
    const auto found = poseGraphIndex.find(id);
    if (found == poseGraphIndex.end())
        throw std::invalid_argument("Unknown pose graph node: " + id);
    if (poseGraphNodes[found->second].kind != PoseGraphNode::Kind::BlendSpace2DPlayer)
        throw std::invalid_argument("Pose graph dual-axis value requires a 2D blend space: " + id);
    if (!std::isfinite(valueX) || !std::isfinite(valueY))
        throw std::invalid_argument("Pose graph value must be finite");
    poseGraphAxisValues[id] = Eigen::Vector2f(valueX, valueY);
}

void SkeletalPlayback::SetPoseGraphAxisValue(const std::string& id, const std::string& axis,
    float value)
{
    const auto found = poseGraphIndex.find(id);
    if (found == poseGraphIndex.end())
        throw std::invalid_argument("Unknown pose graph node: " + id);
    if (poseGraphNodes[found->second].kind != PoseGraphNode::Kind::BlendSpace2DPlayer)
        throw std::invalid_argument("Pose graph axis value requires a 2D blend space: " + id);
    if (!std::isfinite(value))
        throw std::invalid_argument("Pose graph value must be finite");
    Eigen::Vector2f& axes = poseGraphAxisValues[id];
    const std::string normalized = LowerAscii(axis);
    if (normalized == "x")
        axes.x() = value;
    else if (normalized == "y")
        axes.y() = value;
    else
        throw std::invalid_argument("Pose graph axis must be X or Y: " + id);
}

void SkeletalPlayback::SetPoseGraphMontageTargets(const std::string& id,
    std::map<std::string, MotionWarpTarget> targets)
{
    const auto found = poseGraphIndex.find(id);
    if (found == poseGraphIndex.end())
        throw std::invalid_argument("Unknown pose graph node: " + id);
    if (poseGraphNodes[found->second].kind != PoseGraphNode::Kind::MontagePlayer)
        throw std::invalid_argument("Pose graph montage targets require a MontagePlayer: " + id);
    PoseGraphPlayerState& state = poseGraphStates.at(found->second);
    if (!state.montageActive || !state.montageInitialized || !state.montagePlayer)
    {
        poseGraphMontageTargets[id] = std::move(targets);
        return;
    }
    const Montage& montage = FindPoseGraphMontage(
        asset->AnimationAsset(), poseGraphNodes[found->second].montage, id);
    const float currentTime = state.montagePlayer->Time();
    try
    {
        auto candidate = std::make_shared<SkeletalPlayback>(asset);
        PlayMontage(*candidate, montage, targets, false,
            poseGraphNodes[found->second].rate, inPlace, currentTime);
        state.montagePlayer = std::move(candidate);
        poseGraphMontageTargets[id] = std::move(targets);
    }
    catch (const std::exception&)
    {
        // 新 targets 不合法或当前时间不能重建时保留旧播放器/旧 targets。
    }
}

void SkeletalPlayback::SetPoseGraphInPlace(bool value)
{
    if (inPlace == value) return;
    inPlace = value;
    for (PoseGraphPlayerState& state : poseGraphStates)
        if (state.montagePlayer && state.montageActive)
            state.montagePlayer->SetInPlace(value);
}

void SkeletalPlayback::RebuildPoseGraphOrder()
{
    poseGraphOrder.clear();
    std::unordered_map<std::string, size_t> saveProducers;
    for (size_t index = 0; index < poseGraphNodes.size(); ++index)
        if (poseGraphNodes[index].kind == PoseGraphNode::Kind::SaveCachedPose)
            saveProducers.emplace(poseGraphNodes[index].cacheName, index);
    for (const PoseGraphNode& node : poseGraphNodes)
        if (node.kind == PoseGraphNode::Kind::UseCachedPose &&
            saveProducers.find(node.cacheName) == saveProducers.end())
            throw std::invalid_argument(
                "Pose graph uses undefined cached pose: " + node.cacheName);
    std::vector<uint8_t> marks(poseGraphNodes.size(), 0);
    std::function<void(size_t)> visit = [&](size_t index)
    {
        if (marks[index] == 1)
            throw std::invalid_argument("Pose graph contains a cycle");
        if (marks[index] == 2) return;
        marks[index] = 1;
        for (const std::string& input : poseGraphNodes[index].inputs)
            visit(poseGraphIndex.at(input));
        // UseCachedPose 没有显式 input；按 cacheName 把 SaveCachedPose 作为
        // 隐式依赖，保证反向声明顺序也在同一帧先 Save 后 Use。
        if (poseGraphNodes[index].kind == PoseGraphNode::Kind::UseCachedPose)
            visit(saveProducers.at(poseGraphNodes[index].cacheName));
        marks[index] = 2;
        poseGraphOrder.push_back(index);
    };
    for (size_t index = 0; index < poseGraphNodes.size(); ++index) visit(index);
}

LocalPose SkeletalPlayback::EvaluatePoseGraph(float deltaSeconds,
    const Eigen::Matrix4f* meshTransform, const Eigen::Vector3f* actorPosition, float actorYaw)
{
    const SkeletalAsset& animation = asset->AnimationAsset();
    const auto& clips = animation.clips;
    const size_t jointCount = asset->mesh.joints.size();
    poseGraphNodePoses.clear();
    poseGraphCachedPoses.clear();
    poseGraphActiveNotifyStates.clear();
    poseGraphRootMontage = false;
    poseGraphMontageRootDelta.setZero();
    poseGraphMontageRootYawDelta = 0;
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
            const size_t clip = FindClip(clips, node.clip);
            const float duration = clips.at(clip).runtime->duration();
            const float previous = state.time;
            state.time += deltaSeconds * node.rate;
            if (node.loop)
                state.time = duration > 0 ? std::fmod(state.time, duration) : 0.0f;
            else
                state.time = std::min(state.time, std::max(0.0f, duration));
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
            const size_t baseClip = FindClip(clips, node.samples[baseIndex].clip);
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
                state.time = std::min(state.time, std::max(0.0f, duration));
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
                const float step = node.weightSpeedPerSec * std::max(0.0f, deltaSeconds);
                state.weight = std::clamp(target, state.weight - step, state.weight + step);
            }
            const size_t firstClip = FindClip(clips, node.samples[first].clip);
            const size_t secondClip = FindClip(clips, node.samples[second].clip);
            LocalPose firstPose = Sample(firstClip, state.time);
            const float phase = duration > 0 ? std::clamp(state.time / duration, 0.0f, 1.0f) : 0;
            const float secondDuration = clips.at(secondClip).runtime->duration();
            const float rateScale = node.samples[first].rate > 0
                ? node.samples[second].rate / node.samples[first].rate : 1.0f;
            float secondTime = phase * secondDuration * rateScale;
            if (!node.samples[second].syncGroup.empty())
                secondTime = MapBlendSpaceMarkerPhase(clips.at(firstClip), state.time,
                    clips.at(secondClip), node.samples[second].syncGroup, secondTime);
            if (node.loop && secondDuration > 0)
                secondTime = std::fmod(secondTime, secondDuration);
            else secondTime = std::min(secondTime,
                std::max(0.0f, secondDuration - 1e-4f));
            LocalPose secondPose = Sample(secondClip, secondTime);
            if (state.weight <= 0) poseGraphNodePoses[node.id] = std::move(firstPose);
            else if (state.weight >= 1) poseGraphNodePoses[node.id] = std::move(secondPose);
            else poseGraphNodePoses[node.id] = Crossfade(firstPose, std::move(secondPose),
                state.weight, asset->mesh);
        }
        else if (node.kind == PoseGraphNode::Kind::BlendSpace2DPlayer)
        {
            const BlendGrid2D grid = BuildBlendGrid2D(node.samples, node.id);
            const auto& referenceSample = node.samples.front();
            const size_t referenceClip = FindClip(clips, referenceSample.clip);
            const float referenceDuration = clips.at(referenceClip).runtime->duration();
            if (index == poseGraphRoot)
            {
                poseGraphRootPreviousTime = state.time;
                poseGraphRootValid = true;
            }
            state.time += deltaSeconds * node.rate;
            if (node.loop)
                state.time = referenceDuration > 0 ?
                    std::fmod(state.time, referenceDuration) : 0.0f;
            else
                state.time = std::min(state.time,
                    std::max(0.0f, referenceDuration));
            const auto found = poseGraphAxisValues.find(node.id);
            const auto scalar = poseGraphValues.find(node.id);
            const Eigen::Vector2f axes = found != poseGraphAxisValues.end() ? found->second :
                Eigen::Vector2f(scalar != poseGraphValues.end() ? scalar->second : 0.0f, 0.0f);
            const float alphaX = std::clamp(
                (axes.x() - grid.x[0]) / (grid.x[1] - grid.x[0]), 0.0f, 1.0f);
            const float alphaY = std::clamp(
                (axes.y() - grid.y[0]) / (grid.y[1] - grid.y[0]), 0.0f, 1.0f);
            const float phase = referenceDuration > 0
                ? std::clamp(state.time / referenceDuration, 0.0f, 1.0f) : 0.0f;
            auto sampleCorner = [&](size_t y, size_t x) -> LocalPose
            {
                const PoseGraphSample& sample = *grid.corners[y][x];
                const size_t clip = FindClip(clips, sample.clip);
                const float duration = clips.at(clip).runtime->duration();
                const float rateScale = referenceSample.rate > 0
                    ? sample.rate / referenceSample.rate : 1.0f;
                float sampleTime = phase * duration * rateScale;
                if (!sample.syncGroup.empty())
                    sampleTime = MapBlendSpaceMarkerPhase(clips.at(referenceClip),
                        state.time, clips.at(clip), sample.syncGroup, sampleTime);
                if (node.loop && duration > 0)
                    sampleTime = std::fmod(sampleTime, duration);
                else
                    sampleTime = std::min(sampleTime,
                        std::max(0.0f, duration - 1e-4f));
                return Sample(clip, sampleTime);
            };
            LocalPose bottom = alphaX <= 0 ? sampleCorner(0, 0) :
                alphaX >= 1 ? sampleCorner(0, 1) :
                Crossfade(sampleCorner(0, 0), sampleCorner(0, 1), alphaX, asset->mesh);
            LocalPose top = alphaX <= 0 ? sampleCorner(1, 0) :
                alphaX >= 1 ? sampleCorner(1, 1) :
                Crossfade(sampleCorner(1, 0), sampleCorner(1, 1), alphaX, asset->mesh);
            poseGraphNodePoses[node.id] = alphaY <= 0 ? std::move(bottom) :
                alphaY >= 1 ? std::move(top) :
                Crossfade(std::move(bottom), std::move(top), alphaY, asset->mesh);
        }
        else if (node.kind == PoseGraphNode::Kind::MontagePlayer)
        {
            if (state.montageCompleted && !node.loop)
            {
                state.montageActive = false;
                state.montageCompleted = false;
                state.montageInitialized = false;
                state.montagePlayer.reset();
            }
            if (!state.montageActive)
            {
                poseGraphNodePoses[node.id] = reference;
                continue;
            }
            const Montage& montage = FindPoseGraphMontage(animation, node.montage, node.id);
            const auto install = [&]
            {
                state.montagePlayer = std::make_shared<SkeletalPlayback>(asset);
                static const std::map<std::string, MotionWarpTarget> emptyTargets;
                const auto targets = poseGraphMontageTargets.find(node.id);
                PlayMontage(*state.montagePlayer, montage,
                    targets != poseGraphMontageTargets.end() ? targets->second : emptyTargets,
                    false, node.rate, inPlace, state.montageStartPosition);
                state.montageInitialized = true;
                state.montageCompleted = false;
            };
            if (!state.montageInitialized) install();
            state.montagePlayer->Tick(deltaSeconds, meshTransform, actorPosition, actorYaw);
            poseGraphNodePoses[node.id] = state.montagePlayer->CurrentLocalPose();
            firedNotifies.insert(firedNotifies.end(),
                state.montagePlayer->FiredNotifies().begin(),
                state.montagePlayer->FiredNotifies().end());
            poseGraphActiveNotifyStates.insert(poseGraphActiveNotifyStates.end(),
                state.montagePlayer->ActiveNotifyStates().begin(),
                state.montagePlayer->ActiveNotifyStates().end());
            if (state.montagePlayer->Completed())
            {
                state.montageCompleted = true;
                if (node.loop)
                {
                    state.montageInitialized = false;
                    state.montageCompleted = false;
                }
            }
            if (index == poseGraphRoot)
            {
                poseGraphRootValid = true;
                poseGraphRootMontage = true;
                poseGraphMontageRootDelta = state.montagePlayer->RootMotionDelta();
                poseGraphMontageRootYawDelta = state.montagePlayer->RootMotionYawDelta();
            }
        }
        else if (node.kind == PoseGraphNode::Kind::Blend)
        {
            const auto value = poseGraphValues.find(node.id);
            const float alpha = value != poseGraphValues.end() ?
                std::clamp(value->second, 0.0f, 1.0f) : node.alpha;
            poseGraphNodePoses[node.id] = Crossfade(pose(node.inputs.at(0)),
                pose(node.inputs.at(1)), alpha, asset->mesh);
        }
        else if (node.kind == PoseGraphNode::Kind::Select)
        {
            const auto value = poseGraphValues.find(node.id);
            const bool second = value != poseGraphValues.end() && value->second >= 0.5f;
            poseGraphNodePoses[node.id] = pose(node.inputs.at(second ? 1 : 0));
        }
        else if (node.kind == PoseGraphNode::Kind::ApplyAdditive)
        {
            const auto value = poseGraphValues.find(node.id);
            const float alpha = value != poseGraphValues.end() ?
                std::clamp(value->second, 0.0f, 1.0f) : node.alpha;
            const PoseGraphNode& additiveNode = poseGraphNodes.at(
                poseGraphIndex.at(node.inputs.at(1)));
            const std::string& additiveClipId = additiveNode.clip;
            const size_t additiveClip = FindClip(clips, additiveClipId);
            if (additiveClip == clips.size())
                throw std::invalid_argument("Pose graph additive clip is unknown");
            const std::string& type = clips.at(additiveClip).sequence.additiveAnimType;
            const LocalPose additiveReference = AdditiveReferencePose(additiveClip);
            if (type == "RotationOffsetMeshSpace")
                poseGraphNodePoses[node.id] = ApplyMeshSpaceRotationAdditive(
                    pose(node.inputs.at(0)), pose(node.inputs.at(1)), additiveReference, alpha,
                    asset->mesh.joints);
            else if (type == "LocalSpaceBase")
                poseGraphNodePoses[node.id] = ApplyLocalSpaceAdditive(
                    pose(node.inputs.at(0)), pose(node.inputs.at(1)), additiveReference, alpha,
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
            LocalPose result = pose(node.inputs.at(0));
            if (node.inputs.size() == 2)
            {
                const auto value = poseGraphValues.find(node.id);
                const float alpha = value != poseGraphValues.end() ?
                    std::clamp(value->second, 0.0f, 1.0f) : node.alpha;
                const size_t montageIndex = poseGraphIndex.at(node.inputs[1]);
                const PoseGraphPlayerState& montageState = poseGraphStates.at(montageIndex);
                if (alpha > 0 && montageState.montageActive)
                {
                    const std::vector<float> weights = ResolveSlotWeights(
                        node.weights, asset->mesh.joints, node.id);
                    result = ApplyLayeredWeights(std::move(result),
                        pose(node.inputs[1]), weights, alpha, jointCount);
                }
            }
            poseGraphNodePoses[node.id] = std::move(result);
        }
        else if (node.kind == PoseGraphNode::Kind::Output)
        {
            poseGraphNodePoses[node.id] = pose(node.inputs.at(0));
        }
    }
    const size_t result = poseGraphEntry < poseGraphNodes.size()
        ? poseGraphEntry : poseGraphOutput;
    return poseGraphNodePoses.at(poseGraphNodes.at(result).id);
}

void SkeletalPlayback::SetPoseGraphEntry(const std::string& id, float startPosition,
    float blendSeconds)
{
    if (!poseGraphActive)
        throw std::invalid_argument("Pose graph is not active");
    if (!std::isfinite(startPosition) || startPosition < 0 ||
        !std::isfinite(blendSeconds) || blendSeconds < 0)
        throw std::invalid_argument("Pose graph entry timing is invalid");
    size_t entry = static_cast<size_t>(-1);
    if (!id.empty())
    {
        const auto found = poseGraphIndex.find(id);
        if (found == poseGraphIndex.end())
            throw std::invalid_argument("Unknown pose graph entry node: " + id);
        entry = found->second;
    }
    const size_t previousEntry = poseGraphEntry;
    poseGraphEntry = entry;
    const size_t entryNode = entry < poseGraphNodes.size() ? entry : poseGraphOutput;
    const size_t nextRoot = ResolvePoseGraphRoot(entryNode);
    if (nextRoot >= poseGraphNodes.size() ||
        !IsRootPlayer(poseGraphNodes[nextRoot].kind))
        throw std::invalid_argument("Pose graph entry has no root player");
    if (nextRoot == poseGraphRoot && previousEntry == entry) return;
    // 入口切换是一个新的播放段。冻结当前已发布姿态作为交叉淡入源；随后重置
    // 目标根源时钟，避免后台播放器沿用旧状态的相位。
    if (blendSeconds > 0 && !displayedLocals.empty())
    {
        blendSource = displayedLocals;
        blendDuration = blendSeconds;
        blendElapsed = 0;
        inertialization.Clear();
        ClearSyncSourceClock();
    }
    else
    {
        blendSource.clear();
        blendDuration = 0;
        blendElapsed = 0;
        inertialization.Clear();
        ClearSyncSourceClock();
    }
    ResetPoseGraphRootState(nextRoot, startPosition);
}
} // namespace VL::Animation
