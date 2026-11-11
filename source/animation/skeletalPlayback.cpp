#include "skeletalPlayback.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/offline/raw_animation_utils.h>

namespace VL::Animation
{
const Montage* SkeletalAsset::FindMontage(std::string_view idOrPath) const
{
    for (const Montage& montage : AnimationAsset().montages)
    {
        if (montage.id == idOrPath || montage.sourcePath == idOrPath)
        {
            return &montage;
        }
    }
    return nullptr;
}

namespace
{
std::atomic<uint64_t> nextPlaybackIdentity{1};

float ReadLane(ozz::math::SimdFloat4 vector, size_t lane)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    return values[lane];
}

Eigen::Vector3f RootTranslation(const Clip& clip, float time)
{
    return SampleRootMotionTranslation(clip, time);
}

Eigen::Quaternionf RootRotation(const Clip& clip, float time)
{
    ozz::math::Transform root;
    if (clip.raw.tracks.empty() ||
        !ozz::animation::offline::SampleTrack(clip.raw.tracks[0], time, &root))
        return Eigen::Quaternionf::Identity();
    return Eigen::Quaternionf(root.rotation.w, root.rotation.x, root.rotation.y,
        root.rotation.z).normalized();
}

float YawFromQuaternion(const Eigen::Quaternionf& q)
{
    return std::atan2(2.0f * (q.w() * q.y() + q.x() * q.z()),
        1.0f - 2.0f * (q.y() * q.y() + q.x() * q.x()));
}

void WriteLane(ozz::math::SimdFloat4& vector, size_t lane, float value)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    values[lane] = value;
    vector = ozz::math::simd_float4::LoadPtrU(values);
}

std::vector<JointKinematics> ReadKinematics(const LocalPose& locals, size_t jointCount)
{
    std::vector<JointKinematics> result(jointCount);
    for (size_t joint = 0; joint < jointCount; ++joint)
    {
        const auto& source = locals[joint / 4];
        const size_t lane = joint % 4;
        auto& output = result[joint];
        output.translation = {ReadLane(source.translation.x, lane), ReadLane(source.translation.y, lane),
            ReadLane(source.translation.z, lane)};
        output.rotation = Eigen::Quaternionf(ReadLane(source.rotation.w, lane), ReadLane(source.rotation.x, lane),
            ReadLane(source.rotation.y, lane), ReadLane(source.rotation.z, lane)).normalized();
        output.scale = {ReadLane(source.scale.x, lane), ReadLane(source.scale.y, lane), ReadLane(source.scale.z, lane)};
    }
    return result;
}

LocalPose WriteKinematics(LocalPose result, const std::vector<JointKinematics>& joints)
{
    for (size_t joint = 0; joint < joints.size(); ++joint)
    {
        auto& target = result[joint / 4];
        const size_t lane = joint % 4;
        const auto& value = joints[joint];
        WriteLane(target.translation.x, lane, value.translation.x());
        WriteLane(target.translation.y, lane, value.translation.y());
        WriteLane(target.translation.z, lane, value.translation.z());
        WriteLane(target.rotation.x, lane, value.rotation.x());
        WriteLane(target.rotation.y, lane, value.rotation.y());
        WriteLane(target.rotation.z, lane, value.rotation.z());
        WriteLane(target.rotation.w, lane, value.rotation.w());
        WriteLane(target.scale.x, lane, value.scale.x());
        WriteLane(target.scale.y, lane, value.scale.y());
        WriteLane(target.scale.z, lane, value.scale.z());
    }
    return result;
}

std::vector<JointKinematics> MeasureKinematics(const LocalPose& pose, const LocalPose& future,
    size_t jointCount, float step)
{
    auto result = ReadKinematics(pose, jointCount);
    const auto next = ReadKinematics(future, jointCount);
    for (size_t joint = 0; joint < jointCount; ++joint)
    {
        result[joint].linearVelocity = (next[joint].translation - result[joint].translation) / step;
        result[joint].angularVelocity = RotationLog(next[joint].rotation * result[joint].rotation.conjugate()) / step;
        result[joint].logScaleVelocity =
            (next[joint].scale.array().log() - result[joint].scale.array().log()).matrix() / step;
    }
    return result;
}

LocalPose Crossfade(const LocalPose& source, LocalPose target, float alpha, const SkeletonMesh& mesh)
{
    LocalPose mixed(target.size());
    ozz::animation::BlendingJob::Layer layers[2];
    layers[0].transform = ozz::make_span(source);
    layers[0].weight = 1 - alpha;
    layers[1].transform = ozz::make_span(target);
    layers[1].weight = alpha;
    ozz::animation::BlendingJob blend;
    blend.layers = ozz::make_span(layers);
    blend.rest_pose = mesh.runtime->joint_rest_poses();
    blend.output = ozz::make_span(mixed);
    if (!blend.Run()) throw std::runtime_error("Animation local pose blending failed");
    return mixed;
}
}
namespace
{
// 把绑定空间 AABB 的 8 个角点用关节 palette 矩阵变换后并入并集。
// 保守性来自“盒子包含顶点 + 仿射变换保持包含关系”，不依赖权重分布。
void MergeJointBounds(const Eigen::Matrix4f& matrix, const JointBindBounds& bounds,
    Eigen::Vector3f& min, Eigen::Vector3f& max)
{
    const Eigen::Matrix3f linear = matrix.block<3, 3>(0, 0);
    const Eigen::Vector3f translation = matrix.block<3, 1>(0, 3);
    for (int corner = 0; corner < 8; ++corner)
    {
        const Eigen::Vector3f point(
            (corner & 1) ? bounds.max.x() : bounds.min.x(),
            (corner & 2) ? bounds.max.y() : bounds.min.y(),
            (corner & 4) ? bounds.max.z() : bounds.min.z());
        const Eigen::Vector3f transformed = linear * point + translation;
        min = min.cwiseMin(transformed);
        max = max.cwiseMax(transformed);
    }
}

// 权重和偏差修正：LBS 的精确位置是 Σ w_i M_i p，而 Σ w_i 只保证在
// [1-e,1+e]（导入器按 WeightTolerance 校验）。把凸组合按该区间绕原点缩放后
// 取包围盒，保证权重未严格归一化时精确结果仍在盒内。
void ExpandForWeightSum(Eigen::Vector3f& min, Eigen::Vector3f& max, float error)
{
    if (!(error > 0)) return;
    const float low = 1.0f - error;
    const float high = 1.0f + error;
    min = min.cwiseMin(min * low).cwiseMin(min * high);
    max = max.cwiseMax(max * low).cwiseMax(max * high);
}

// 全骨架 bind→pose：与 section 无关，跨 section 的代理权重可以直接使用。
// section 的 inverseBind 只对该 section 实际引用的关节有效；其余关节在
// SkinPose::matrices 里退化为绝对 model 矩阵。这里用 restModel 的逆重建。
void FillBindToPoseMatrices(SkinPose& pose,
    const std::vector<Eigen::Matrix4f>& models, const std::vector<Joint>* joints)
{
    if (joints == nullptr || joints->size() != models.size())
        return;
    pose.bindToPoseMatrices.reserve(models.size());
    for (size_t joint = 0; joint < models.size(); ++joint)
    {
        const Eigen::Matrix4f bindInverse = joints->at(joint).restModel.inverse();
        const Eigen::Matrix4f bindToPose = models[joint] * bindInverse;
        if (!bindToPose.allFinite())
            throw std::runtime_error("Skin pose has a non-finite bind-to-pose matrix");
        pose.bindToPoseMatrices.push_back(bindToPose);
    }
}
}

Eigen::Matrix4f BlendSkinMatrix(const SkinSection& section, size_t vertex,
    const std::vector<Eigen::Matrix4f>& palette)
{
    Eigen::Matrix4f blended = Eigen::Matrix4f::Zero();
    for (uint32_t i = section.influenceOffsets.at(vertex);
         i < section.influenceOffsets.at(vertex + 1); ++i)
    {
        const auto& influence = section.influences.at(i);
        blended += influence.weight * palette.at(influence.joint);
    }
    return blended;
}

std::shared_ptr<const SkinPose> BuildSkinPoseReference(const SkinSection& section,
    const std::vector<Eigen::Matrix4f>& models, uint64_t discontinuity,
    uint64_t playbackIdentity, const std::vector<Joint>* joints)
{
    if (models.size() != section.inverseBinds.size() || section.vertices.empty())
        throw std::invalid_argument("Skin pose requires a matching skeleton and nonempty section");
    auto pose = std::make_shared<SkinPose>();
    pose->discontinuity = discontinuity;
    pose->playbackIdentity = playbackIdentity;
    for (size_t i = 0; i < models.size(); ++i)
        pose->matrices.push_back(models[i] * section.inverseBinds[i]);
    FillBindToPoseMatrices(*pose, models, joints);
    pose->boundsMin = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    pose->boundsMax = -pose->boundsMin;
    for (size_t vertex = 0; vertex < section.vertices.size(); ++vertex)
    {
        // 只累加仿射的 3x4，避免 Debug 下逐顶点 Eigen block/表达式构造。
        // 布局仍为列优先，权重次序与参考 BlendSkinMatrix/GPU 完全相同。
        float affine[12]{};
        for (uint32_t i = section.influenceOffsets[vertex];
             i < section.influenceOffsets[vertex + 1]; ++i)
        {
            const auto& influence = section.influences[i];
            const float* matrix = pose->matrices[influence.joint].data();
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 3; ++row)
                    affine[column * 3 + row] += influence.weight * matrix[column * 4 + row];
        }
        const auto& source = section.vertices[vertex].position;
        Eigen::Vector3f position;
        for (int row = 0; row < 3; ++row)
            position[row] = affine[row] * source.x() + affine[3 + row] * source.y() +
                affine[6 + row] * source.z() + affine[9 + row];
        const float determinant =
            affine[0] * (affine[4] * affine[8] - affine[7] * affine[5]) -
            affine[3] * (affine[1] * affine[8] - affine[7] * affine[2]) +
            affine[6] * (affine[1] * affine[5] - affine[4] * affine[2]);
        // LBS 的线性部分可能在极端姿态退化；在 CPU 入口拒绝，shader 不做逐顶点修补。
        if (!position.allFinite() || !std::isfinite(determinant) || std::abs(determinant) < 1.0e-8f)
            throw std::runtime_error("Skin pose has a singular or nonfinite blended transform");
        pose->boundsMin = pose->boundsMin.cwiseMin(position);
        pose->boundsMax = pose->boundsMax.cwiseMax(position);
    }
    return pose;
}

std::shared_ptr<const SkinPose> BuildSkinPose(const SkinSection& section,
    const std::vector<Eigen::Matrix4f>& models, uint64_t discontinuity,
    uint64_t playbackIdentity, const std::vector<Joint>* joints)
{
    if (models.size() != section.inverseBinds.size() || section.vertices.empty())
        throw std::invalid_argument("Skin pose requires a matching skeleton and nonempty section");
    // 直接构造的 section（测试/离线工具）没有预计算数据：退回逐顶点精确参考路径，
    // 行为与引入快路径前完全一致。
    if (section.jointBindBounds.size() != section.inverseBinds.size())
        return BuildSkinPoseReference(section, models, discontinuity,
            playbackIdentity, joints);
    auto pose = std::make_shared<SkinPose>();
    pose->discontinuity = discontinuity;
    pose->playbackIdentity = playbackIdentity;
    pose->matrices.reserve(models.size());
    FillBindToPoseMatrices(*pose, models, joints);
    Eigen::Vector3f min = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    Eigen::Vector3f max = -min;
    bool hasGeometry = false;
    for (size_t joint = 0; joint < models.size(); ++joint)
    {
        const Eigen::Matrix4f matrix = models[joint] * section.inverseBinds[joint];
        pose->matrices.push_back(matrix);
        const JointBindBounds& bounds = section.jointBindBounds[joint];
        if (bounds.vertexCount == 0) continue;
        // 快路径不再逐顶点混合，改为逐关节检查 palette 仿射行列式：旧路径对
        // 完全归属该关节的顶点会算出同一个矩阵，非有限/奇异仍在这里被拒绝。
        const float determinant = matrix.block<3, 3>(0, 0).determinant();
        if (!matrix.allFinite() || !std::isfinite(determinant) || std::abs(determinant) < 1.0e-8f)
            throw std::runtime_error("Skin pose has a singular or nonfinite blended transform");
        MergeJointBounds(matrix, bounds, min, max);
        hasGeometry = true;
    }
    // 有顶点却没有任何影响引用属于资产契约破坏；导入器已拒绝，这里保留明确失败。
    if (!hasGeometry)
        throw std::runtime_error("Skin pose has vertices without joint influences");
    ExpandForWeightSum(min, max, section.jointBindWeightSumError);
    pose->boundsMin = min;
    pose->boundsMax = max;
    return pose;
}

std::vector<uint32_t> PackSkinInfluences(const SkinSection& section)
{
    const size_t header = section.influenceOffsets.size();
    if (header != section.vertices.size() + 1 ||
        header + section.influences.size() * 2 > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("Invalid skin CSR size");
    std::vector<uint32_t> words;
    words.reserve(header + section.influences.size() * 2);
    for (uint32_t offset : section.influenceOffsets)
        words.push_back(static_cast<uint32_t>(header) + 2 * offset);
    for (const auto& influence : section.influences)
    {
        uint32_t bits;
        static_assert(sizeof(bits) == sizeof(influence.weight));
        std::memcpy(&bits, &influence.weight, sizeof(bits));
        words.push_back(influence.joint);
        words.push_back(bits);
    }
    return words;
}

SkeletalPlayback::SkeletalPlayback(std::shared_ptr<const SkeletalAsset> input)
    : asset(std::move(input))
{
    if (!asset || asset->mesh.sections.empty() || asset->mesh.joints.empty() || !asset->mesh.runtime)
        throw std::invalid_argument("Playback requires a skeletal mesh");
    // loader 和直接构造资产的调用方共用此边界：动作源/profile 必须成对，
    // 且不能把多层重定向或目标自带片段藏在播放器后面。
    if (static_cast<bool>(asset->animationSource) != static_cast<bool>(asset->retargetProfile) ||
        (asset->IsRetargeted() &&
            (asset->animationSource->IsRetargeted() ||
                asset->animationSource->clips.empty() || !asset->animationSource->mesh.runtime ||
                !asset->clips.empty() || !asset->montages.empty())))
        throw std::invalid_argument("Retargeted playback requires a valid animation source and profile");
    if (asset->IsRetargeted())
        compiledRetarget = std::make_shared<const CompiledRetarget>(
            asset->animationSource->mesh, asset->mesh, *asset->retargetProfile);
    playbackIdentity = nextPlaybackIdentity.fetch_add(1);
    displayedLocals = Sample(selected, time);
    poses = Evaluate(displayedLocals, discontinuity, jointModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    RebuildNotifyEvents();
}

float SkeletalPlayback::ClipDuration() const
{
    const auto& animation = asset->AnimationAsset();
    return animation.clips.empty() ? 0.0f : animation.clips.at(selected).runtime->duration();
}

float SkeletalPlayback::PlaybackRangeEnd() const
{
    // 多段 Montage 的合法上界是 Montage 时间轴长度，不是当前 clip 的 1.333 s。
    // rangeEnd 仍是“未设置”哨兵；section 推进只写内部范围，不改变段映射。
    if (!montageSegments.empty())
    {
        const float timelineEnd = std::max(0.0f, montageTimelineLength);
        if (playbackRangeEnd <= 0)
            return timelineEnd;
        return std::min(playbackRangeEnd, timelineEnd);
    }
    const float clipEnd = ClipDuration();
    // end==0 是“未显式设置”的哨兵：范围末尾跟随当前片段，切片段后无需重新设置。
    if (playbackRangeEnd <= 0) return clipEnd;
    return std::min(playbackRangeEnd, clipEnd);
}

float SkeletalPlayback::MontageSourceTime(const MontageSegment& segment,
    float timeline, float clipDuration) const
{
    const float clipStart = std::clamp(segment.clipStartTime, 0.0f,
        std::max(0.0f, clipDuration));
    const float declaredEnd = segment.clipEndTime > 0 ?
        segment.clipEndTime : clipDuration;
    const float clipEnd = std::max(clipStart,
        std::min(declaredEnd, std::max(clipStart, clipDuration)));
    const float local = (timeline - segment.startTime) * segment.rateScale;
    if (!segment.loop)
        return std::clamp(clipStart + local, clipStart, clipEnd);

    const float span = clipEnd - clipStart;
    if (!(span > 0) || !std::isfinite(local))
        return clipStart;
    float wrapped = std::fmod(local, span);
    if (wrapped < 0) wrapped += span;
    return clipStart + wrapped;
}

SkeletalPlayback::MontageSample SkeletalPlayback::SampleMontageTimeline(
    float timeline, bool atEnd) const
{
    MontageSample result;
    if (montageSegments.empty() || montageSegmentClips.size() != montageSegments.size())
    {
        result.clip = selected;
        result.clipTime = timeline;
        return result;
    }

    constexpr float tolerance = 1e-5f;
    size_t segmentIndex = montageSegments.size() - 1;
    for (size_t index = 0; index < montageSegments.size(); ++index)
    {
        const MontageSegment& segment = montageSegments[index];
        if (timeline < segment.endTime - tolerance)
        {
            segmentIndex = index;
            break;
        }
        if ((atEnd || index + 1 == montageSegments.size()) &&
            timeline <= segment.endTime + tolerance)
        {
            segmentIndex = index;
            break;
        }
    }
    const MontageSegment& segment = montageSegments[segmentIndex];
    result.segment = segmentIndex;
    result.clip = montageSegmentClips[segmentIndex];
    const float duration = asset->AnimationAsset().clips.at(result.clip).runtime->duration();
    result.clipTime = MontageSourceTime(segment, timeline, duration);
    return result;
}

Eigen::Vector3f SkeletalPlayback::MontageRootDelta(float from, float to) const
{
    if (!(to > from) || montageSegments.empty())
        return Eigen::Vector3f::Zero();

    Eigen::Vector3f total = Eigen::Vector3f::Zero();
    float cursor = from;
    for (size_t index = 0; index < montageSegments.size() && cursor < to; ++index)
    {
        const MontageSegment& segment = montageSegments[index];
        const float begin = std::max(cursor, segment.startTime);
        const float end = std::min(to, segment.endTime);
        if (!(end > begin)) continue;
        const Clip& clip = asset->AnimationAsset().clips.at(montageSegmentClips[index]);
        const float duration = clip.runtime->duration();
        total += RootTranslation(clip, MontageSourceTime(segment, end, duration)) -
            RootTranslation(clip, MontageSourceTime(segment, begin, duration));
        cursor = end;
    }
    return total;
}

void SkeletalPlayback::ClearMontageSegments()
{
    montageSegments.clear();
    montageSegmentClips.clear();
    montageSegment = 0;
    montageTimelineLength = 0;
}

float SkeletalPlayback::Duration() const
{
    // 播放长度按范围计算；time/notify 仍是 clip-local 绝对时间，所以 Completed 比较
    // PlaybackRangeEnd()，而不是 Duration()。
    return std::max(0.0f, PlaybackRangeEnd() - playbackRangeStart);
}

std::optional<float> SkeletalPlayback::SampleCurve(std::string_view name, float seconds) const
{
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty()) return std::nullopt;
    // 曲线归当前播放片段所有；Montage 只改变播放范围，不复制 Sequence 曲线。
    return SampleFloatCurve(animation.clips.at(selected).sequence, name, seconds);
}

std::optional<float> SkeletalPlayback::SampleCurve(std::string_view name) const
{
    if (!montageSegments.empty())
    {
        const MontageSample sample = SampleMontageTimeline(time);
        return SampleCurve(name, sample.clipTime);
    }
    return SampleCurve(name, time);
}

Eigen::Vector3f SkeletalPlayback::RootMotionTotal() const
{
    if (!montageSegments.empty())
        return MontageRootDelta(PlaybackRangeStart(), PlaybackRangeEnd());
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty()) return Eigen::Vector3f::Zero();
    const auto& clip = animation.clips.at(selected);
    // 根位移积分同样限制在播放范围：Montage 截断后不能把 section 之后的根轨迹算进来，
    // 否则 subtractRemainingRootMotion 会消费到范围外的位移。
    return RootTranslation(clip, PlaybackRangeEnd()) - RootTranslation(clip, playbackRangeStart);
}

Eigen::Vector3f SkeletalPlayback::RootMotionRemaining() const
{
    if (!montageSegments.empty())
        return MontageRootDelta(std::min(time, PlaybackRangeEnd()), PlaybackRangeEnd());
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty()) return Eigen::Vector3f::Zero();
    const auto& clip = animation.clips.at(selected);
    return RootTranslation(clip, PlaybackRangeEnd()) -
        RootTranslation(clip, std::min(time, PlaybackRangeEnd()));
}

void SkeletalPlayback::RebuildNotifyEvents()
{
    notifyEvents.clear();
    const auto& animation = asset->AnimationAsset();
    if (!montageSegments.empty())
    {
        // 多段资产把每段 sidecar 的 notify 映射到 Montage 时间轴。这样切 clip 时
        // 事件表不需要重建，段边界也不会因为 clip 本地时间归零而重复/漏发。
        for (size_t segmentIndex = 0; segmentIndex < montageSegments.size(); ++segmentIndex)
        {
            const MontageSegment& segment = montageSegments[segmentIndex];
            const Clip& clip = animation.clips.at(montageSegmentClips.at(segmentIndex));
            const float clipDuration = clip.runtime->duration();
            const float clipStart = std::clamp(segment.clipStartTime, 0.0f,
                std::max(0.0f, clipDuration));
            const float clipEnd = std::max(clipStart, std::min(
                segment.clipEndTime > 0 ? segment.clipEndTime : clipDuration,
                std::max(clipStart, clipDuration)));
            const float sourceSpan = clipEnd - clipStart;
            const float timelineSpan = segment.endTime - segment.startTime;
            const float rateScale = segment.rateScale;
            const size_t loops = segment.loop && sourceSpan > 1e-5f && rateScale > 0 ?
                static_cast<size_t>(std::ceil(timelineSpan * rateScale / sourceSpan)) : 1;
            // 段内循环只重复 sidecar 事件；上限防止恶意短 clip 生成无限表。
            for (size_t loop = 0; loop < std::min<size_t>(loops, 64); ++loop)
            {
                const float loopOffset = static_cast<float>(loop) * sourceSpan / rateScale;
                for (const auto& notify : clip.sequence.notifies)
                {
                    const float sourceTime = notify.time;
                    if (sourceTime < clipStart - 1e-5f ||
                        sourceTime > clipEnd + 1e-5f)
                        continue;
                    NotifyEvent event;
                    event.name = notify.name;
                    event.track = notify.track;
                    event.kind = notify.duration > 0 ? "notifyState" : "notify";
                    event.time = segment.startTime + loopOffset +
                        (sourceTime - clipStart) / rateScale;
                    event.duration = notify.duration / rateScale;
                    if (event.time < segment.startTime - 1e-5f ||
                        event.time > segment.endTime + 1e-5f)
                        continue;
                    event.segmentIndex = static_cast<int>(segmentIndex);
                    event.segmentId = segment.id;
                    notifyEvents.push_back(std::move(event));
                }
            }
        }
    }
    else if (!animation.clips.empty())
    {
        // Sequence notify 没有 kind 字段：duration==0 是 UE 瞬时 notify，duration>0 是
        // notifyState。time/duration 已经由解析器保留作者原值（可越过片段末尾），这里不裁剪。
        for (const auto& notify : animation.clips.at(selected).sequence.notifies)
        {
            NotifyEvent event;
            event.name = notify.name;
            event.track = notify.track;
            event.kind = notify.duration > 0 ? "notifyState" : "notify";
            event.time = notify.time;
            event.duration = notify.duration;
            notifyEvents.push_back(std::move(event));
        }
    }
    // Montage JSON 与 sidecar 是两份数据：Montage 提供 warp/PoseSearch 等 notifyState，
    // sidecar 提供 Footstep 等序列级瞬时 notify；播放 Montage 时两者都要执行，不能互相覆盖。
    for (const auto& notify : montageNotifies)
    {
        NotifyEvent event;
        event.name = notify.name;
        event.track = notify.track;
        // Montage 的 notify 即使带 duration 也不执行区间；notifyState 只有 duration>0 才有区间语义。
        event.kind = notify.kind == "notifyState" && notify.duration > 0 ? "notifyState" : "notify";
        event.time = notify.time;
        event.duration = event.kind == "notifyState" ? notify.duration : 0.0f;
        if (!montageSegments.empty())
        {
            const MontageSample sample = SampleMontageTimeline(notify.time);
            event.segmentIndex = static_cast<int>(sample.segment);
            event.segmentId = montageSegments.at(sample.segment).id;
        }
        notifyEvents.push_back(std::move(event));
    }
    // 运行时事件表按时间排序；同刻事件用 stable_sort 保留作者顺序（Sequence 在前）。
    std::stable_sort(notifyEvents.begin(), notifyEvents.end(),
        [](const NotifyEvent& left, const NotifyEvent& right) { return left.time < right.time; });
    notifyActive.assign(notifyEvents.size(), 0);
    RefreshActiveNotifyStates();
}

void SkeletalPlayback::RefreshActiveNotifyStates()
{
    activeNotifyStates.clear();
    for (size_t index = 0; index < notifyEvents.size(); ++index)
    {
        if (!notifyActive[index]) continue;
        ActiveNotifyState state;
        state.name = notifyEvents[index].name;
        state.track = notifyEvents[index].track;
        state.startTime = notifyEvents[index].time;
        state.endTime = notifyEvents[index].EndTime();
        activeNotifyStates.push_back(std::move(state));
    }
}

void SkeletalPlayback::PushFired(const NotifyEvent& notify, const char* phase, float eventTime,
    bool interrupted)
{
    FiredNotify event;
    event.name = notify.name;
    event.track = notify.track;
    event.kind = notify.kind;
    event.phase = phase;
    event.time = eventTime;
    // startTime/duration 始终是作者原值，不因播放范围裁剪而改写；回调方据此还原完整区间。
    event.startTime = notify.time;
    event.duration = notify.duration;
    event.interrupted = interrupted;
    event.segmentIndex = notify.segmentIndex;
    event.segmentId = notify.segmentId;
    firedNotifies.push_back(std::move(event));
}

void SkeletalPlayback::ScanNotifySegment(double from, double to, bool includeTerminalEnd)
{
    if (!(to >= from)) return;
    struct Point
    {
        double time = 0;
        size_t index = 0;
        bool end = false;
    };
    std::vector<Point> points;
    points.reserve(notifyEvents.size() * 2);
    for (size_t index = 0; index < notifyEvents.size(); ++index)
    {
        const NotifyEvent& notify = notifyEvents[index];
        const double start = static_cast<double>(notify.time);
        if (!notify.IsState())
        {
            // [from,to) 半开：正好落在当前时刻的事件留给下一 tick，避免两 tick 重复触发；
            // 一次性播放到达末尾的那一 tick 例外，允许 time==to 的瞬时 notify。
            // 循环尾部以 rangeEnd 为上界（to 不含），因此 time==rangeEnd 的瞬时 notify
            // 在循环播放中不触发；需要它在最后一次循环结束时补发时用一次性播放。
            const bool inSegment = start >= from &&
                (start < to || (includeTerminalEnd && start <= to));
            if (inSegment) points.push_back({start, index, false});
            continue;
        }
        // notifyState 的 begin：区间起点进入扫描窗口。terminal 上不开启新区间，
        // 否则播放范围末尾会补出一个立刻中断的零长度状态。
        const bool beginsHere = start >= from && start < to;
        if (beginsHere) points.push_back({start, index, false});
        const double end = static_cast<double>(notify.EndTime());
        // end 含上界：到达作者 EndTime 的那一 tick 结束，不需要再等下一 tick。
        // 若“起点和终点同一 tick 内”（大 delta 跨过整个区间），begin/end 都会进点表，
        // 按时间排序后依次执行，回调方仍能看到完整的 begin→end。
        if ((notifyActive[index] || beginsHere) && end <= to)
            points.push_back({std::max(end, from), index, true});
    }
    std::stable_sort(points.begin(), points.end(),
        [](const Point& left, const Point& right) { return left.time < right.time; });
    for (const Point& point : points)
    {
        const NotifyEvent& notify = notifyEvents[point.index];
        if (point.end)
        {
            if (!notifyActive[point.index]) continue;
            notifyActive[point.index] = 0;
            PushFired(notify, "end", static_cast<float>(point.time), false);
        }
        else if (!notify.IsState())
        {
            PushFired(notify, "instant", static_cast<float>(point.time), false);
        }
        else if (!notifyActive[point.index])
        {
            notifyActive[point.index] = 1;
            PushFired(notify, "begin", static_cast<float>(point.time), false);
        }
    }
}

void SkeletalPlayback::EndActiveNotifyStates(float atTime, bool interrupted, bool deferToNextTick)
{
    for (size_t index = 0; index < notifyEvents.size(); ++index)
    {
        if (!notifyActive[index]) continue;
        notifyActive[index] = 0;
        PushFired(notifyEvents[index], "end", atTime, interrupted);
        // 控制面事件可能在本帧 Tick 之前产生；复制一份留到下一 Tick 的事件窗口，
        // 否则同帧 Tick 重建 firedNotifies 时会把还没被反馈读取的 end 丢掉。
        if (deferToNextTick) pendingNotifies.push_back(firedNotifies.back());
    }
    RefreshActiveNotifyStates();
}

void SkeletalPlayback::SetMontageNotifies(std::vector<MontageNotify> notifies,
    std::string id, float length)
{
    // 换事件源前先结束旧源的活动区间；旧 notify 表仍是解释 active 下标的唯一依据。
    EndActiveNotifyStates(time, true, true);
    montageNotifies = std::move(notifies);
    montageId = std::move(id);
    montageLength = length;
    RebuildNotifyEvents();
}

void SkeletalPlayback::SetPlaybackRange(float start, float end)
{
    SetPlaybackRangeInternal(start, end);
    // section 链依赖范围；直接改范围时停止旧链。多段映射是独立的时间轴身份，
    // 由 SetMontageSections 内部的 range 写入保留，不能在这里清掉。
    montageSections.clear();
    montageSection = 0;
}

void SkeletalPlayback::SetPlaybackRangeInternal(float start, float end)
{
    const float clipDuration = montageSegments.empty() ?
        ClipDuration() : montageTimelineLength;
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || !(end > start))
        throw std::invalid_argument("Playback range must be finite and increasing from a nonnegative start");
    // 播放范围不能延长资源：end 越过片段末尾时按片段末尾 clamp（Montage section 的
    // 导出误差也走这条路径），clamp 后为空的范围仍拒绝。
    const float clampedEnd = std::min(end, clipDuration);
    if (!(clampedEnd > start))
        throw std::invalid_argument("Playback range is empty inside the clip");
    // 只影响后续 tick 的播放长度、根位移积分与 notify 有效域，不重采样当前姿态；
    // 非 0 起点也不移动时钟，调用方需要随后 Seek(start) 对齐（见头文件注释）。
    playbackRangeStart = start;
    playbackRangeEnd = clampedEnd;
}

void SkeletalPlayback::SetMontageSections(std::vector<MontageSection> sections,
    std::optional<float> entryStartTime)
{
    if (sections.empty())
        throw std::invalid_argument("Montage playback requires at least one section");
    if (looping)
        throw std::invalid_argument("Montage section playback requires one-shot playback");
    // 入口时间只截短 section 0：起点不能早于作者起点（缺省 = 作者起点）。越界拦截在
    // PlayMontage 装配期完成（诊断带 montage id）；这里只保证范围本身合法。
    const float entry = std::max(sections.front().startTime,
        entryStartTime.value_or(sections.front().startTime));
    // 先按第一段装配范围（同时清掉旧链），再安装链；两段之间没有可失败操作。
    SetPlaybackRange(entry, sections.front().endTime);
    montageSections = std::move(sections);
    montageSection = 0;
}

void SkeletalPlayback::SetMontageSegments(std::vector<MontageSegment> segments,
    float length)
{
    if (segments.empty())
    {
        ClearMontageSegments();
        RebuildNotifyEvents();
        return;
    }
    if (!std::isfinite(length) || length <= 0)
        throw std::invalid_argument("Montage segment timeline length must be positive");

    std::vector<size_t> clipIndices;
    clipIndices.reserve(segments.size());
    float previousEnd = 0;
    for (size_t index = 0; index < segments.size(); ++index)
    {
        const MontageSegment& segment = segments[index];
        if (segment.derivedClipId.empty())
            throw std::invalid_argument("Montage segment '" + segment.id +
                "' has no derived clip id");
        if (!std::isfinite(segment.startTime) || !std::isfinite(segment.endTime) ||
            segment.startTime < 0 || !(segment.endTime > segment.startTime) ||
            segment.endTime > length + 1e-3f ||
            std::abs(segment.startTime - previousEnd) > 1e-3f)
            throw std::invalid_argument("Montage segment timeline is not contiguous at '" +
                segment.id + "'");
        if (!std::isfinite(segment.rateScale) || segment.rateScale <= 0 ||
            segment.rateScale > 4)
            throw std::invalid_argument("Montage segment '" + segment.id +
                "' has an invalid rateScale");
        size_t clip = 0;
        const auto& clips = asset->AnimationAsset().clips;
        while (clip < clips.size() && clips[clip].id != segment.derivedClipId)
            ++clip;
        if (clip == clips.size() || clips[clip].sequence.id != segment.derivedClipId)
            throw std::invalid_argument("Montage segment '" + segment.id +
                "' requires a loaded sidecar clip '" + segment.derivedClipId + "'");
        clipIndices.push_back(clip);
        previousEnd = segment.endTime;
    }
    if (std::abs(previousEnd - length) > 1e-3f)
        throw std::invalid_argument(
            "Montage segments must cover the complete timeline");

    montageSegments = std::move(segments);
    montageSegmentClips = std::move(clipIndices);
    montageTimelineLength = length;
    const MontageSample sample = SampleMontageTimeline(std::clamp(time, 0.0f, length));
    montageSegment = sample.segment;
    selected = sample.clip;
    RebuildNotifyEvents();
}

std::string SkeletalPlayback::MontageSectionName() const
{
    return montageSections.empty() ? std::string() :
        montageSections.at(montageSection).name;
}

std::string SkeletalPlayback::MontageSegmentId() const
{
    return montageSegments.empty() ? std::string() :
        montageSegments.at(montageSegment).id;
}

int SkeletalPlayback::MontageNextSectionIndex() const
{
    if (montageSections.empty()) return -1;
    const std::string& next = montageSections.at(montageSection).nextSection;
    if (next.empty()) return -1;
    for (size_t index = 0; index < montageSections.size(); ++index)
        if (montageSections[index].name == next) return static_cast<int>(index);
    // 未命中引用已在 PlayMontage 的 ValidateMontageExecution 拒绝；运行期不抛异常，
    // 退化为“没有下一段”的停止语义。
    return -1;
}

void SkeletalPlayback::Stop()
{
    // Stop 与暂停不同：活动 notifyState 必须补 end(interrupted)，让 C# 清理窗口状态。
    // 时钟/姿态保留，下一次 Play 会重置；暂停（SetPlaying(false)）保留活动区间继续。
    EndActiveNotifyStates(time, true, true);
    playing = false;
    explicitlyPaused = false;
    ClearSyncSourceClock();
    syncDecisionValid = false;
    syncLeader.clear();
    syncPendingValid = false;
    syncPendingHasTime = false;
}

std::vector<std::shared_ptr<const SkinPose>> SkeletalPlayback::Evaluate(
    const LocalPose& locals, uint64_t epoch,
    std::shared_ptr<const std::vector<Eigen::Matrix4f>>& outputModels) const
{
    auto models = LocalToModels(asset->mesh, locals);
    // 所有 section 完成后才发布时钟、选择与姿态，异常时保持同一个旧状态。
    std::vector<std::shared_ptr<const SkinPose>> next;
    for (const auto& section : asset->mesh.sections)
        next.push_back(BuildSkinPose(section, models, epoch, playbackIdentity,
            &asset->mesh.joints));
    outputModels = std::make_shared<const std::vector<Eigen::Matrix4f>>(std::move(models));
    return next;
}

LocalPose SkeletalPlayback::Sample(size_t clipIndex, float sampleTime) const
{
    const auto rest = asset->mesh.runtime->joint_rest_poses();
    const auto& animation = asset->AnimationAsset();
    LocalPose locals;
    if (animation.clips.empty())
    {
        locals = LocalPose(rest.begin(), rest.end());
    }
    else
    {
        locals = SampleLocals(animation.mesh, animation.clips.at(clipIndex), sampleTime);
        // 先在 source skeleton 上采样，再转换模型空间旋转增量；普通资产复用
        // 原采样结果，target 骨长/未映射骨骼不被动作源的比例污染。
        if (compiledRetarget)
            locals = compiledRetarget->Apply(locals);
    }
    if (inPlace)
    {
        // 根是唯一的骨架入口；只替换 lane 0 的完整绑定 TRS，等价于旧模型空间
        // restRoot * inverse(sampleRoot) 校正，并保留 pelvis 相对运动。混合前移除，
        // 避免原根位移/缩放污染交叉混合；Actor/Jolt 始终独占胶囊位移。
        auto& root = locals.front();
        const auto& bind = rest.front();
        root.translation.x = ozz::math::SetX(root.translation.x, bind.translation.x);
        root.translation.y = ozz::math::SetX(root.translation.y, bind.translation.y);
        root.translation.z = ozz::math::SetX(root.translation.z, bind.translation.z);
        root.rotation.x = ozz::math::SetX(root.rotation.x, bind.rotation.x);
        root.rotation.y = ozz::math::SetX(root.rotation.y, bind.rotation.y);
        root.rotation.z = ozz::math::SetX(root.rotation.z, bind.rotation.z);
        root.rotation.w = ozz::math::SetX(root.rotation.w, bind.rotation.w);
        root.scale.x = ozz::math::SetX(root.scale.x, bind.scale.x);
        root.scale.y = ozz::math::SetX(root.scale.y, bind.scale.y);
        root.scale.z = ozz::math::SetX(root.scale.z, bind.scale.z);
    }
    return locals;
}

LocalPose SkeletalPlayback::PreviewPose(float aheadSeconds) const
{
    if (!playing || Duration() == 0) return displayedLocals;
    const float rangeStart = PlaybackRangeStart();
    const float rangeEnd = PlaybackRangeEnd();
    const double next = double(time) + double(aheadSeconds) * rate;
    float sampleTime;
    if (looping && next >= rangeEnd)
    {
        // 预览与 Tick 用同一个循环域 [rangeStart,rangeEnd)：回绕点不是片段的 0。
        sampleTime = float(double(rangeStart) +
            std::fmod(next - double(rangeStart), double(rangeEnd - rangeStart)));
    }
    else sampleTime = float(std::min(next, double(rangeEnd)));
    size_t sampleClip = selected;
    float clipSampleTime = sampleTime;
    if (!montageSegments.empty())
    {
        const MontageSample sample = SampleMontageTimeline(sampleTime, sampleTime >= rangeEnd);
        sampleClip = sample.clip;
        clipSampleTime = sample.clipTime;
    }
    auto locals = Sample(sampleClip, clipSampleTime);
    const float elapsed = std::min(blendElapsed + aheadSeconds, blendDuration);
    if (!inertialization.Empty() && elapsed < blendDuration)
    {
        const auto target = ReadKinematics(locals, asset->mesh.joints.size());
        locals = WriteKinematics(std::move(locals), inertialization.Evaluate(target, elapsed));
    }
    else if (!blendSource.empty() && elapsed < blendDuration)
        locals = Crossfade(blendSource, std::move(locals), elapsed / blendDuration, asset->mesh);
    return locals;
}

void SkeletalPlayback::Tick(float deltaSeconds, const Eigen::Matrix4f* meshTransform,
    const Eigen::Vector3f* actorPosition, float actorYaw)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0)
        throw std::invalid_argument("Playback delta must be finite and nonnegative");
    rootMotionDelta.setZero();
    rootMotionYawDelta = 0;
    // 同步组对齐先于本 tick 的时间推进应用：跳变本身不产生根位移，
    // 本帧根轨迹从对齐后的 time 到采样点，等价于 UE 的 follower 校正。
    bool syncJumped = false;
    if (syncPendingValid)
    {
        if (syncPendingHasTime && playing && deltaSeconds > 0)
        {
            const float rangeStart = PlaybackRangeStart();
            const float rangeEnd = PlaybackRangeEnd();
            float target = syncPendingTime;
            if (looping && rangeEnd > rangeStart)
            {
                target = rangeStart + std::fmod(target - rangeStart, rangeEnd - rangeStart);
                if (target < rangeStart) target += rangeEnd - rangeStart;
            }
            else
                target = std::clamp(target, rangeStart, std::max(rangeStart, rangeEnd - 1e-4f));
            if (std::abs(target - time) > 1e-5f)
            {
                // 相位跳变是硬跳变：递增 epoch，避免把跨相位差算成根位移。
                time = target;
                ++discontinuity;
                syncJumped = true;
            }
        }
        rate = std::clamp(syncPendingRate, AnimationSyncGroupRegistry::RateMin,
            AnimationSyncGroupRegistry::RateMax);
        syncPendingValid = false;
        syncPendingHasTime = false;
    }
    // 每个 Tick 重建“本反馈窗口”的事件：先并入控制面（Play/Stop/Select/TransitionTo）
    // 补发的 end，再追加本次位置穿越；未被读取的控制面事件不会在同帧 Tick 后丢失。
    firedNotifies = std::move(pendingNotifies);
    pendingNotifies.clear();
    if (!playing || Duration() == 0 || deltaSeconds == 0) return;
    AdvanceSyncSourceClock(deltaSeconds);
    if (syncJumped)
    {
        // 与 Seek 一样，相位跳变不补发被跳过的事件；只按新位置重建活动区间，
        // 避免旧 notifyState 在时间跳变后继续悬空。
        std::fill(notifyActive.begin(), notifyActive.end(), 0);
        RefreshActiveNotifyStates();
    }
    // 多 section Montage 走独立的推进器（见 TickMontageSections）；单 section/普通片段
    // 保持下面的原路径不变，避免既有行为因 section 支持发生位级漂移。
    if (!montageSegments.empty())
    {
        TickSegmentedMontage(deltaSeconds, meshTransform, actorPosition, actorYaw);
        return;
    }
    if (!montageSections.empty())
    {
        TickMontageSections(deltaSeconds, meshTransform, actorPosition, actorYaw);
        return;
    }
    const float rangeStart = PlaybackRangeStart();
    const float rangeEnd = PlaybackRangeEnd();
    const double nextPosition = static_cast<double>(time) + double(deltaSeconds) * rate;
    // 循环域是 [rangeStart,rangeEnd)；非循环在 rangeEnd 处 clamp。这里只求一次回绕，
    // 单个 tick 跨越多圈时事件只按“尾部+头部”各处理一次（与单次时钟采样一致）。
    bool wrapped = false;
    double next = nextPosition;
    if (looping)
    {
        if (next >= rangeEnd)
        {
            next = double(rangeStart) +
                std::fmod(next - double(rangeStart), double(rangeEnd - rangeStart));
            wrapped = true;
        }
    }
    else next = std::min(next, double(rangeEnd));
    const auto epoch = discontinuity + (looping && !inPlace && wrapped ? 1 : 0);
    const float sampleTime = static_cast<float>(next);
    const float elapsed = IsBlending() ? std::min(blendElapsed + deltaSeconds, blendDuration) : 0;
    if (sampleTime == time && epoch == discontinuity && !wrapped && !IsBlending()) return;
    const float previousTime = time;
    auto locals = Sample(selected, sampleTime);
    if (!inertialization.Empty() && elapsed < blendDuration)
    {
        const auto target = ReadKinematics(locals, asset->mesh.joints.size());
        locals = WriteKinematics(std::move(locals), inertialization.Evaluate(target, elapsed));
    }
    else if (!blendSource.empty() && elapsed < blendDuration)
        locals = Crossfade(blendSource, std::move(locals), elapsed / blendDuration, asset->mesh);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> nextModels;
    auto nextPoses = Evaluate(locals, epoch, nextModels);
    poses.swap(nextPoses);
    jointModels.swap(nextModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    displayedLocals.swap(locals);
    if (epoch == discontinuity)
    {
        const auto& clip = asset->AnimationAsset().clips.at(selected);
        rootMotionDelta = RootTranslation(clip, sampleTime) - RootTranslation(clip, time);
        const auto rotation = RootRotation(clip, sampleTime) *
            RootRotation(clip, time).inverse();
        // 先放原始根 yaw；旋转 warp 只在本 tick 有激活窗口且提交了 target 旋转时
        // 覆盖它，平移结果（rootMotionDelta）与 yaw 通道互不覆盖。
        float yawDelta = YawFromQuaternion(rotation);
        // 原生 modifier 在模型空间改写本 tick 的根位移/根 yaw；C# 只消费最终
        // rootMotion/rootMotionYaw 交给 Jolt 与 Actor 旋转，不做窗口提取、
        // target 换算或 SkewWarp 数学。
        if (!motionWarp.Empty())
        {
            if (meshTransform && actorPosition)
                rootMotionDelta = motionWarp.Apply(asset->AnimationAsset().mesh, clip, time, sampleTime,
                    rootMotionDelta, *meshTransform, *actorPosition, actorYaw, &yawDelta);
            motionWarpState = motionWarp.DebugState(time);
        }
        else motionWarpState.clear();
        rootMotionYawDelta = yawDelta;
    }
    time = sampleTime;
    RefreshSyncMarker();
    // notify 只在姿态求值成功后提交：异常 tick 不改变活动区间。事件按实际位置穿越触发，
    // 显式 seek/切片段不补发历史（见 Seek/Select 的清理）。
    if (wrapped)
    {
        ScanNotifySegment(std::max(double(previousTime), double(rangeStart)), double(rangeEnd), false);
        // 回绕时仍活跃的区间先在循环末尾补 end(interrupted)，再扫描新循环头部；
        // 跨越循环点的 notifyState 在同一 tick 内得到 end→begin，固定“先尾部后头部”。
        EndActiveNotifyStates(rangeEnd, true, false);
        ScanNotifySegment(double(rangeStart), double(sampleTime), false);
    }
    else
    {
        // 一次性播放到达范围末尾的那一 tick 允许 time==rangeEnd 的瞬时 notify；
        // 越过末尾仍活跃的 notifyState 在 completed 时补 end(interrupted)。
        const bool terminal = !looping && sampleTime >= rangeEnd;
        ScanNotifySegment(std::max(double(previousTime), double(rangeStart)),
            double(sampleTime), terminal);
        if (terminal) EndActiveNotifyStates(rangeEnd, true, false);
    }
    RefreshActiveNotifyStates();
    blendElapsed = elapsed;
    if (IsBlending() && elapsed >= blendDuration)
    {
        blendSource.clear();
        inertialization.Clear();
        ClearSyncSourceClock();
    }
    if (Completed()) playing = false;
    discontinuity = epoch; // 仅保留原根的循环才会归零模型根轨迹。
}

void SkeletalPlayback::ClearSyncSourceClock()
{
    syncSourceValid = false;
    syncSourceSelected = 0;
    syncSourceTime = 0;
    syncSourceRangeStart = 0;
    syncSourceRangeEnd = 0;
    syncSourceRate = 1;
    syncSourceLooping = true;
}

void SkeletalPlayback::AdvanceSyncSourceClock(float deltaSeconds)
{
    if (!syncSourceValid) return;
    const float length = syncSourceRangeEnd - syncSourceRangeStart;
    if (!std::isfinite(length) || length <= 0) return;
    double next = double(syncSourceTime) + double(deltaSeconds) * syncSourceRate;
    if (syncSourceLooping)
    {
        next = double(syncSourceRangeStart) +
            std::fmod(next - double(syncSourceRangeStart), double(length));
        if (next < syncSourceRangeStart) next += length;
    }
    else next = std::min(next, double(syncSourceRangeEnd));
    syncSourceTime = static_cast<float>(next);
}

void SkeletalPlayback::TickMontageSections(float deltaSeconds,
    const Eigen::Matrix4f* meshTransform, const Eigen::Vector3f* actorPosition, float actorYaw)
{
    const auto& clip = asset->AnimationAsset().clips.at(selected);
    const float previousTime = time;
    const float elapsed = IsBlending() ? std::min(blendElapsed + deltaSeconds, blendDuration) : 0;
    double remaining = static_cast<double>(deltaSeconds) * rate;
    float position = time;
    Eigen::Vector3f translation = Eigen::Vector3f::Zero();
    float yaw = 0;
    // 段切换的时间跳跃不计入根运动：只累加“实际移动过”的子区间，等价 UE 每个
    // sub-step 先 ExtractRootMotionFromTrackRange 再 Accumulate（AnimMontage.cpp:2649-2670）；
    // 跳转本身不产生根位移/根旋转。
    int jumps = 0;
    // UE 单 tick 最多 10 个 sub-step（AnimMontage.cpp:2575）。超过时放弃本 tick 剩余
    // 时间、保留当前位置继续下一 tick，避免 section 自连接时死循环。
    constexpr int maxSectionSteps = 10;
    // 链接落点与段末相差小于该值视为连续过渡（导入/序列化浮点误差），不按“向后跳”处理。
    constexpr float sectionJumpEpsilon = 1e-4f;
    int steps = 0;
    for (; remaining > 0 && steps < maxSectionSteps; ++steps)
    {
        const float segmentStart = position;
        const float segmentEnd = PlaybackRangeEnd();
        const double target = static_cast<double>(segmentStart) + remaining;
        const bool reachesEnd = target >= static_cast<double>(segmentEnd);
        const float segmentStop = reachesEnd ? segmentEnd : static_cast<float>(target);
        translation += RootTranslation(clip, segmentStop) - RootTranslation(clip, segmentStart);
        yaw = std::remainder(yaw + YawFromQuaternion(RootRotation(clip, segmentStop) *
            RootRotation(clip, segmentStart).inverse()), 2.0f * 3.14159265358979323846f);
        remaining -= std::max(0.0,
            static_cast<double>(segmentStop) - static_cast<double>(segmentStart));
        position = segmentStop;
        const int nextSection = reachesEnd ? MontageNextSectionIndex() : -1;
        // 段边界按半开区间 [segmentStart, segmentStop) 扫描：恰好落在段末的事件属于下一段，
        // 由下一段的 from 触发（下一段起点 == 段末时只触发一次，不重复也不丢）。整条链的
        // 最后一段沿用一次性播放的 terminal 语义（允许 time==end 的瞬时 notify），
        // 与未安装 section 链的单段路径一致。
        ScanNotifySegment(static_cast<double>(segmentStart), static_cast<double>(segmentStop),
            reachesEnd && nextSection < 0);
        if (!reachesEnd) break;
        if (nextSection < 0)
        {
            // nextSection 为空：停在段末并保留当前姿态；Completed()/Stop 语义与单段一致。
            EndActiveNotifyStates(segmentEnd, true, false);
            break;
        }
        const MontageSection& currentSection = montageSections.at(montageSection);
        const float linkStart = ResolveMontageSectionLinkTime(currentSection);
        // 向后跳（section 自连接/回前段）沿用既有循环回绕语义：先在段末补
        // end(interrupted)，再扫描新段头部；向前跳/连续过渡不结束活动区间
        // （UE 只改 Position，活动 notifyState 由后续时间穿越结束，见
        // AnimMontage.cpp:2715-2733）。前跳越过 notifyState 结束点时，下一次扫描
        // 会以新段起点补 end，不会留下永久的活动区间。
        if (linkStart < segmentEnd - sectionJumpEpsilon)
            EndActiveNotifyStates(segmentEnd, true, false);
        const MontageSection& next = montageSections.at(static_cast<size_t>(nextSection));
        montageSection = static_cast<size_t>(nextSection);
        // 只改范围，不经过公开 SetPlaybackRange（那会清空刚推进的 section 链）。
        SetPlaybackRangeInternal(next.startTime, next.endTime);
        // PlayMontage 已校验 linkStart 落在目标 section 内；这里只做防御性 clamp，
        // 保证时钟永远在 [rangeStart,rangeEnd]。
        position = std::clamp(linkStart, PlaybackRangeStart(), PlaybackRangeEnd());
        if (std::abs(position - segmentStop) > sectionJumpEpsilon) ++jumps;
    }
    // 时间跳变是真实姿态不连续：与 Seek/Select 一样推进 discontinuity。连续 section
    // 过渡（position == 段末）不推进，post-blend 姿态历史保持连续。
    const uint64_t epoch = discontinuity + (jumps > 0 ? 1 : 0);
    auto locals = Sample(selected, position);
    if (!inertialization.Empty() && elapsed < blendDuration)
    {
        const auto target = ReadKinematics(locals, asset->mesh.joints.size());
        locals = WriteKinematics(std::move(locals), inertialization.Evaluate(target, elapsed));
    }
    else if (!blendSource.empty() && elapsed < blendDuration)
        locals = Crossfade(blendSource, std::move(locals), elapsed / blendDuration, asset->mesh);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> nextModels;
    auto nextPoses = Evaluate(locals, epoch, nextModels);
    poses.swap(nextPoses);
    jointModels.swap(nextModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    displayedLocals.swap(locals);
    rootMotionDelta = translation;
    float yawDelta = yaw;
    // warp 窗口时间与 clip-local 时间同源；窗口队列在切段时不重建。若本 tick 最终位置
    // 在起点之前（回前段），MotionWarpRuntime 会因 current<=previous 跳过窗口，本 tick
    // 只保留原始根运动——这是一次真实的位置跳变，不应把跳跃区间算成 warp 位移。
    if (!motionWarp.Empty())
    {
        if (meshTransform && actorPosition)
            rootMotionDelta = motionWarp.Apply(asset->AnimationAsset().mesh, clip, previousTime, position,
                translation, *meshTransform, *actorPosition, actorYaw, &yawDelta);
        motionWarpState = motionWarp.DebugState(previousTime);
    }
    else motionWarpState.clear();
    rootMotionYawDelta = yawDelta;
    time = position;
    RefreshActiveNotifyStates();
    blendElapsed = elapsed;
    if (IsBlending() && elapsed >= blendDuration)
    {
        blendSource.clear();
        inertialization.Clear();
    }
    if (Completed()) playing = false;
    discontinuity = epoch;
}

Eigen::Vector3f SkeletalPlayback::ApplySegmentedMontageWarp(
    const MontageSegment& segment, const Clip& clip, float sourceFrom, float sourceTo,
    const Eigen::Vector3f& rawDelta, const Eigen::Matrix4f& meshTransform,
    const Eigen::Vector3f& actorPosition, float actorYaw, float& yawDelta) const
{
    if (motionWarp.Empty()) return rawDelta;

    const float clipDuration = clip.runtime->duration();
    std::vector<MotionWarpWindow> mapped;
    mapped.reserve(motionWarp.Windows().size());
    for (const MotionWarpWindow& window : motionWarp.Windows())
    {
        const float begin = std::max(window.startTime, segment.startTime);
        const float end = std::min(window.endTime, segment.endTime);
        if (!(end > begin)) continue;
        // 窗口时间从 Montage 时间轴映射到该段 clip 本地时间；跨段窗口在每段
        // 各自裁剪，避免用上一段的 clip 时间采样下一段的根/骨骼。
        MotionWarpWindow local = window;
        local.startTime = MontageSourceTime(segment, begin, clipDuration);
        local.endTime = MontageSourceTime(segment, end, clipDuration);
        if (!(local.endTime > local.startTime)) continue;
        mapped.push_back(std::move(local));
    }
    if (mapped.empty()) return rawDelta;

    MotionWarpRuntime mappedWarp;
    mappedWarp.SetWindows(std::move(mapped));
    return mappedWarp.Apply(asset->AnimationAsset().mesh, clip, sourceFrom, sourceTo,
        rawDelta, meshTransform, actorPosition, actorYaw, &yawDelta);
}

void SkeletalPlayback::TickSegmentedMontage(float deltaSeconds,
    const Eigen::Matrix4f* meshTransform, const Eigen::Vector3f* actorPosition,
    float actorYaw)
{
    const auto& clips = asset->AnimationAsset().clips;
    const float previousTime = time;
    const float elapsed = IsBlending() ? std::min(blendElapsed + deltaSeconds, blendDuration) : 0;
    double remaining = static_cast<double>(deltaSeconds) * rate;
    float position = time;
    Eigen::Vector3f translation = Eigen::Vector3f::Zero();
    float yaw = 0;
    int jumps = 0;
    constexpr int maxSteps = 10;
    constexpr float epsilon = 1e-5f;
    int steps = 0;

    while (remaining > epsilon && steps < maxSteps)
    {
        ++steps;
        if (montageSections.empty()) break;
        const MontageSection& section = montageSections.at(montageSection);
        const float sectionEnd = section.endTime;
        MontageSample sample = SampleMontageTimeline(position, position >= sectionEnd);
        const size_t segmentIndex = sample.segment;
        const MontageSegment& segment = montageSegments.at(segmentIndex);
        const Clip& clip = clips.at(montageSegmentClips.at(segmentIndex));
        const float clipDuration = clip.runtime->duration();

        const double target = static_cast<double>(position) + remaining;
        const float segmentEnd = std::min(sectionEnd, segment.endTime);
        const float pieceStop = std::min(static_cast<float>(target), segmentEnd);
        const bool reachesSectionEnd = pieceStop >= sectionEnd - epsilon;
        if (!(pieceStop > position + epsilon))
        {
            // 已经站在段末：只推进段身份，不消耗时间。
            if (segmentIndex + 1 < montageSegments.size() && position < sectionEnd)
            {
                montageSegment = segmentIndex + 1;
                selected = montageSegmentClips[montageSegment];
                ++jumps;
                continue;
            }
            break;
        }

        const float sourceFrom = MontageSourceTime(segment, position, clipDuration);
        const float sourceTo = MontageSourceTime(segment, pieceStop, clipDuration);
        Eigen::Vector3f pieceDelta = RootTranslation(clip, sourceTo) -
            RootTranslation(clip, sourceFrom);
        float pieceYaw = YawFromQuaternion(RootRotation(clip, sourceTo) *
            RootRotation(clip, sourceFrom).inverse());
        if (!motionWarp.Empty() && meshTransform && actorPosition)
        {
            pieceDelta = ApplySegmentedMontageWarp(segment, clip, sourceFrom, sourceTo,
                pieceDelta, *meshTransform, *actorPosition, actorYaw, pieceYaw);
        }
        translation += pieceDelta;
        yaw = std::remainder(yaw + pieceYaw, 2.0f * 3.14159265358979323846f);

        remaining -= std::max(0.0,
            static_cast<double>(pieceStop) - static_cast<double>(position));
        const float pieceStart = position;
        position = pieceStop;
        const int nextSection = reachesSectionEnd ? MontageNextSectionIndex() : -1;
        ScanNotifySegment(static_cast<double>(pieceStart), static_cast<double>(pieceStop),
            reachesSectionEnd && nextSection < 0);

        if (reachesSectionEnd)
        {
            if (nextSection < 0)
            {
                EndActiveNotifyStates(sectionEnd, true, false);
                break;
            }
            const float linkStart = ResolveMontageSectionLinkTime(section);
            if (linkStart < sectionEnd - 1e-4f)
                EndActiveNotifyStates(sectionEnd, true, false);
            const MontageSection& next = montageSections.at(static_cast<size_t>(nextSection));
            montageSection = static_cast<size_t>(nextSection);
            SetPlaybackRangeInternal(next.startTime, next.endTime);
            position = std::clamp(linkStart, PlaybackRangeStart(), PlaybackRangeEnd());
            if (std::abs(position - sectionEnd) > 1e-4f) ++jumps;
            continue;
        }
        if (segmentIndex + 1 < montageSegments.size() &&
            position >= segment.endTime - epsilon)
        {
            montageSegment = segmentIndex + 1;
            selected = montageSegmentClips[montageSegment];
            ++jumps;
            continue;
        }
        break;
    }

    const MontageSample finalSample = SampleMontageTimeline(position,
        position >= PlaybackRangeEnd() - epsilon);
    montageSegment = finalSample.segment;
    selected = finalSample.clip;
    auto locals = Sample(selected, finalSample.clipTime);
    if (!inertialization.Empty() && elapsed < blendDuration)
    {
        const auto target = ReadKinematics(locals, asset->mesh.joints.size());
        locals = WriteKinematics(std::move(locals), inertialization.Evaluate(target, elapsed));
    }
    else if (!blendSource.empty() && elapsed < blendDuration)
        locals = Crossfade(blendSource, std::move(locals), elapsed / blendDuration, asset->mesh);
    const uint64_t epoch = discontinuity + (jumps > 0 ? 1 : 0);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> nextModels;
    auto nextPoses = Evaluate(locals, epoch, nextModels);
    poses.swap(nextPoses);
    jointModels.swap(nextModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    displayedLocals.swap(locals);
    rootMotionDelta = translation;
    rootMotionYawDelta = yaw;
    motionWarpState = motionWarp.Empty() ? std::string() :
        motionWarp.DebugState(previousTime);
    time = position;
    RefreshActiveNotifyStates();
    blendElapsed = elapsed;
    if (IsBlending() && elapsed >= blendDuration)
    {
        blendSource.clear();
        inertialization.Clear();
    }
    if (Completed()) playing = false;
    discontinuity = epoch;
}

void SkeletalPlayback::TransitionTo(const std::string& clipId, float seconds, bool loop,
    float blendSeconds, bool removeRoot)
{
    size_t index = 0;
    const auto& animation = asset->AnimationAsset();
    while (index < animation.clips.size() && animation.clips[index].id != clipId) ++index;
    if (index == animation.clips.size()) throw std::invalid_argument("Unknown animation clip: " + clipId);
    if (!std::isfinite(seconds) || seconds < 0 || seconds > animation.clips[index].runtime->duration())
        throw std::invalid_argument("Transition time must be inside the clip");
    if (!std::isfinite(blendSeconds) || blendSeconds < 0)
        throw std::invalid_argument("Transition duration must be finite and nonnegative");
    // 根消费策略属于位移所有权边界。平滑过渡不能同时保证首帧连续和立即移除旧根，
    // 因而策略切换必须由调用方显式选零时长硬切；普通 MM 的 in-place 策略保持不变。
    if (blendSeconds > 0 && removeRoot != inPlace)
        throw std::invalid_argument("Changing root policy requires a zero-duration transition");
    SkeletalPlayback candidate(*this);
    const float previousTime = time;
    candidate.selected = index;
    candidate.time = seconds;
    candidate.inPlace = removeRoot;
    candidate.looping = loop;
    candidate.rate = 1;
    candidate.requestedRate = 1;
    candidate.playing = true;
    candidate.explicitlyPaused = false;
    // 过渡开启新的播放段：旧 Montage 的播放范围/notify 覆盖不能带到新片段，
    // 否则 MM transition 会继承被 Montage section 截短的 Duration。
    candidate.playbackRangeStart = 0;
    candidate.playbackRangeEnd = 0;
    candidate.montageNotifies.clear();
    candidate.montageId.clear();
    candidate.montageLength = 0;
    candidate.montageSections.clear();
    candidate.montageSection = 0;
    candidate.ClearMontageSegments();
    candidate.blendElapsed = 0;
    candidate.blendDuration = blendSeconds;
    auto target = candidate.Sample(index, seconds);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> targetModels;
    auto targetPoses = candidate.Evaluate(target, discontinuity + (blendSeconds == 0), targetModels);
    if (blendSeconds == 0)
    {
        candidate.blendSource.clear();
        candidate.inertialization.Clear();
        candidate.displayedLocals.swap(target);
        candidate.poses.swap(targetPoses);
        candidate.jointModels.swap(targetModels);
        candidate.preIkModels = candidate.jointModels;
        candidate.motionModels = candidate.jointModels;
        candidate.visualLocals.clear();
        candidate.slopeWarpedLocals.clear();
        candidate.footPlacementSource.reset();
        candidate.rootOffset.Reset("hard-cut");
        candidate.ResetFootPlacement("hard-cut");
        ++candidate.discontinuity;
    }
    else
    {
        // 首次求值 alpha=0，完全保留已发布的姿态。任意次数中断仍只有一份冻结源，
        // 正常混合不改变 epoch，使当帧 post-blend 姿态继续参与速度历史。
        if (inertializationEnabled)
        {
            // 速度只在过渡边界前看 1ms 的同一条姿态曲线，不推进第二个动画时钟。
            // ozz 压缩采样的导数为数值估计；有限 Hermite 对该估计满足 C1，不承诺任意源 C2。
            constexpr float step = 0.001f;
            // 一次性片段播完时 playing=false，PreviewPose 会原样返回当前姿态，测出的速度是 0；
            // 惯性化于是以零速度起步（整个过渡先“停一下”再加速），只在切换瞬间表现为一次顿挫。
            // 这种情况回看一小步取片段自身的出帧速度：此时 displayedLocals 就是 rangeEnd 姿态，
            // Completed() 又要求混合已结束，所以两者是同一条未混合曲线。
            const float sourceFrom = std::max(PlaybackRangeStart(), time - step);
            const float sourceDelta = time - sourceFrom;
            std::vector<JointKinematics> sourceMotion;
            // 只有“播完停在末尾”才回看；显式 Pause 停在片段中间时速度必须是 0。
            if (!playing && !explicitlyPaused && !looping && sourceDelta > 1e-5f)
                sourceMotion = MeasureKinematics(Sample(selected, sourceFrom), displayedLocals,
                    asset->mesh.joints.size(), sourceDelta);
            else
                sourceMotion = MeasureKinematics(displayedLocals, PreviewPose(step),
                    asset->mesh.joints.size(), step);
            const auto targetMotion = MeasureKinematics(target, candidate.Sample(index,
                loop ? float(std::fmod(double(seconds) + step, candidate.Duration())) :
                    std::min(seconds + step, candidate.Duration())), asset->mesh.joints.size(), step);
            candidate.inertialization.Begin(sourceMotion, targetMotion, blendSeconds);
            candidate.blendSource.clear();
        }
        else
        {
            candidate.inertialization.Clear();
            candidate.blendSource = displayedLocals;
        }
    }
    // 过渡源保留一份只读影子相位时钟：follower 持续对齐需要知道旧 leader 的
    // 归一化时间，但这里不复制姿态播放器，也不产生第二份 notify/根轨迹。
    if (blendSeconds > 0)
    {
        candidate.syncSourceValid = true;
        candidate.syncSourceSelected = selected;
        candidate.syncSourceTime = time;
        candidate.syncSourceRangeStart = PlaybackRangeStart();
        candidate.syncSourceRangeEnd = PlaybackRangeEnd();
        candidate.syncSourceRate = rate;
        candidate.syncSourceLooping = looping;
    }
    else candidate.ClearSyncSourceClock();
    candidate.syncDecisionValid = false;
    candidate.syncLeader.clear();
    candidate.syncPendingValid = false;
    candidate.syncPendingHasTime = false;
    // 过渡是时钟跳变：先结束旧播放段的活动区间（interrupted），再按新片段重建事件表；
    // 新片段不补发目标位置之前的历史（与 seek 的“硬跳变不重放”语义一致）。
    candidate.EndActiveNotifyStates(previousTime, true, true);
    candidate.RebuildNotifyEvents();
    if (candidate.Completed()) candidate.playing = false;
    *this = std::move(candidate);
    activeSyncGroup.clear();
    RefreshSyncMarker();
}

void SkeletalPlayback::TransitionToSynced(const std::string& clipId, float seconds, bool loop,
    float blendSeconds, bool removeRoot, const std::string& syncGroup, float syncPhase)
{
    float resolvedSeconds = seconds;
    std::string matchedGroup;
    if (!syncGroup.empty() && std::isfinite(syncPhase) && syncPhase >= 0)
    {
        const auto& clips = asset->AnimationAsset().clips;
        size_t index = 0;
        while (index < clips.size() && clips[index].id != clipId) ++index;
        if (index < clips.size())
        {
            const SequenceData& sequence = clips[index].sequence;
            for (const PlayerBinding& player : sequence.players)
            {
                if (player.syncGroup != syncGroup ||
                    player.syncMethod == SyncMethod::DoNotSync ||
                    player.syncMethod == SyncMethod::Graph) continue;
                matchedGroup = syncGroup;
                const float duration = clips[index].runtime->duration();
                resolvedSeconds = std::clamp(syncPhase, 0.0f, 1.0f) * duration;
                if (loop)
                    resolvedSeconds = duration > 0 ? std::fmod(resolvedSeconds, duration) : 0;
                else
                    resolvedSeconds = std::min(resolvedSeconds, std::max(0.0f, duration - 1e-4f));
                break;
            }
        }
    }
    TransitionTo(clipId, resolvedSeconds, loop, blendSeconds, removeRoot);
    activeSyncGroup = std::move(matchedGroup);
    RefreshSyncMarker();
}

float SkeletalPlayback::SyncPhase() const
{
    const float duration = ClipDuration();
    if (duration <= 0 || !std::isfinite(time)) return 0;
    float phase = time / duration;
    if (looping) phase -= std::floor(phase);
    return std::clamp(phase, 0.0f, 1.0f);
}

void SkeletalPlayback::RefreshSyncMarker()
{
    syncMarkerName.clear();
    syncMarkerAlpha = 0;
    const auto& sequence = asset->AnimationAsset().clips.at(selected).sequence;
    if (sequence.syncMarkers.empty() || !std::isfinite(time)) return;
    const SyncMarker* previous = nullptr;
    const SyncMarker* next = nullptr;
    for (const SyncMarker& marker : sequence.syncMarkers)
    {
        if (marker.time <= time) previous = &marker;
        else
        {
            next = &marker;
            break;
        }
    }
    if (previous == nullptr) return;
    syncMarkerName = previous->name;
    if (next != nullptr && next->time > previous->time)
        syncMarkerAlpha = std::clamp(
            (time - previous->time) / (next->time - previous->time), 0.0f, 1.0f);
}

const PlayerBinding* SkeletalPlayback::SyncBinding() const
{
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty()) return nullptr;
    const SequenceData& sequence = animation.clips.at(selected).sequence;
    const PlayerBinding* fallback = nullptr;
    for (const PlayerBinding& player : sequence.players)
    {
        if (player.syncGroup.empty() || player.syncMethod == SyncMethod::DoNotSync) continue;
        if (fallback == nullptr) fallback = &player;
        if (!activeSyncGroup.empty() && player.syncGroup == activeSyncGroup) return &player;
    }
    if (fallback != nullptr) return fallback;
    return sequence.players.empty() ? nullptr : &sequence.players.front();
}

const PlayerBinding* SkeletalPlayback::SyncSourceBinding() const
{
    if (!syncSourceValid) return nullptr;
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty() || syncSourceSelected >= animation.clips.size()) return nullptr;
    const SequenceData& sequence = animation.clips.at(syncSourceSelected).sequence;
    const PlayerBinding* fallback = nullptr;
    for (const PlayerBinding& player : sequence.players)
    {
        if (player.syncGroup.empty() || player.syncMethod == SyncMethod::DoNotSync) continue;
        if (fallback == nullptr) fallback = &player;
        if (!activeSyncGroup.empty() && player.syncGroup == activeSyncGroup) return &player;
    }
    if (fallback != nullptr) return fallback;
    return sequence.players.empty() ? nullptr : &sequence.players.front();
}

const std::vector<SyncMarker>* SkeletalPlayback::SyncSourceMarkers() const
{
    if (!syncSourceValid) return nullptr;
    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty() || syncSourceSelected >= animation.clips.size()) return nullptr;
    return &animation.clips.at(syncSourceSelected).sequence.syncMarkers;
}

SyncGroupNode SkeletalPlayback::BuildSyncGroupNode(std::string_view identity,
    std::string_view ownerKey, std::string_view localKey, bool source)
{
    SyncGroupNode node;
    node.playback = this;
    node.identity = identity;
    node.ownerKey = ownerKey;
    node.localKey = localKey;
    node.canApply = !source;
    node.active = playing;
    node.transitioning = IsBlending();
    const float alpha = BlendAlpha();

    const auto& animation = asset->AnimationAsset();
    if (animation.clips.empty()) return node;
    if (source)
    {
        if (!syncSourceValid || syncSourceSelected >= animation.clips.size())
        {
            node.method = SyncMethod::DoNotSync;
            node.active = false;
            return node;
        }
        // 影子时钟只代表交叉淡入的旧片段：混合结束后它已不在画面上；非循环片段
        // 播完后相位 clamp 在 rangeEnd，继续当 leader 会把目标循环钉在末帧
        // （表现就是“卡脚”）。两种情况都不能参与选举。
        node.active = playing && IsBlending() &&
            (syncSourceLooping || syncSourceTime < syncSourceRangeEnd - 1e-4f);
        const SequenceData& sequence = animation.clips.at(syncSourceSelected).sequence;
        node.time = syncSourceTime;
        node.rangeStart = syncSourceRangeStart;
        node.rangeEnd = syncSourceRangeEnd;
        node.rate = syncSourceRate;
        node.looping = syncSourceLooping;
        node.markers = &sequence.syncMarkers;
        node.weight = std::clamp(1.0f - alpha, 0.0f, 1.0f);
        if (const PlayerBinding* binding = SyncSourceBinding())
        {
            node.group = binding->syncGroup;
            node.role = binding->syncRole;
            node.method = binding->syncMethod;
            node.scope = binding->syncGroupScope;
        }
        return node;
    }

    const SequenceData& sequence = animation.clips.at(selected).sequence;
    node.time = time;
    node.rangeStart = PlaybackRangeStart();
    node.rangeEnd = PlaybackRangeEnd();
    node.rate = rate;
    node.looping = looping;
    node.markers = &sequence.syncMarkers;
    node.weight = IsBlending() ? std::clamp(alpha, 0.0f, 1.0f) : 1.0f;
    if (const PlayerBinding* binding = SyncBinding())
    {
        node.group = binding->syncGroup;
        node.role = binding->syncRole;
        node.method = binding->syncMethod;
        node.scope = binding->syncGroupScope;
    }
    return node;
}

void SkeletalPlayback::ApplySyncGroupResult(const SyncGroupNodeResult& result,
    std::string_view leaderIdentity)
{
    syncDecision = result;
    syncDecisionValid = true;
    syncLeader.assign(leaderIdentity);
    syncPendingValid = true;
    syncPendingHasTime = result.follower && result.hasFollow;
    if (syncPendingHasTime)
    {
        syncPendingTime = result.followTime;
        syncPendingRate = result.followRate;
    }
    else
    {
        // leader/无 leader/DoNotSync 都回到作者请求速率，避免上次 follower 的
        // 时长比速率泄漏到下一段片段。
        syncPendingTime = time;
        syncPendingRate = requestedRate;
    }
}

bool SkeletalPlayback::SyncActive() const
{
    return playing && syncDecisionValid && !syncDecision.doNotSync &&
        (syncDecision.leader || syncDecision.follower);
}

void SkeletalPlayback::Play(const std::string& clipId, bool loop, float speed, bool removeRoot)
{
    if (!std::isfinite(speed) || speed <= 0 || speed > 4)
        throw std::invalid_argument("Animation rate must be in (0,4]");
    size_t index = 0;
    const auto& animation = asset->AnimationAsset();
    while (index < animation.clips.size() && animation.clips[index].id != clipId) ++index;
    if (index == animation.clips.size()) throw std::invalid_argument("Unknown animation clip: " + clipId);
    // 请求先在候选值上完成全部采样，失败不改变活动时钟/根策略。
    SkeletalPlayback candidate(*this);
    candidate.inPlace = removeRoot;
    candidate.looping = loop;
    candidate.rate = speed;
    candidate.requestedRate = speed;
    candidate.playing = true;
    candidate.explicitlyPaused = false;
    candidate.inertializationEnabled = false;
    candidate.SetRootOffsetEnabled(false);
    candidate.Select(index);
    *this = std::move(candidate);
}

void SkeletalPlayback::SetMotionWarpWindows(std::vector<MotionWarpWindow> windows)
{
    motionWarp.SetWindows(std::move(windows));
    motionWarpState.clear();
}

void SkeletalPlayback::ClearMotionWarp()
{
    motionWarp.Clear();
    motionWarpState.clear();
}

void SkeletalPlayback::SetRate(float speed)
{
    if (!std::isfinite(speed) || speed <= 0 || speed > 4)
        throw std::invalid_argument("Animation rate must be in (0,4]");
    rate = speed;
    requestedRate = speed;
}

void SkeletalPlayback::SetRootOffsetEnabled(bool enabled)
{
    if (rootOffset.State().enabled == enabled) return;
    if (enabled) rootOffset.SetEnabled(true);
    else
    {
        SkeletalPlayback candidate(*this);
        candidate.rootOffset.SetEnabled(false);
        candidate.ResetRootOffset("disabled");
        *this = std::move(candidate);
    }
}

void SkeletalPlayback::ResetRootOffset(const char* reason)
{
    auto nextRoot = rootOffset;
    nextRoot.Reset(reason);
    auto nextFoot = footPlacement;
    if (nextFoot) nextFoot->Reset(reason);
    if (jointModels != motionModels)
    {
        std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
        auto restored = Evaluate(displayedLocals, discontinuity, models);
        poses.swap(restored);
        jointModels = motionModels;
    }
    preIkModels = motionModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    footPlacement = std::move(nextFoot);
    rootOffset = std::move(nextRoot);
}

void SkeletalPlayback::ApplyVisualRoot(const Eigen::Matrix4f& component,
    const Eigen::Vector3f& achievedVelocity, float deltaSeconds)
{
    if (!rootOffset.State().enabled || deltaSeconds <= 0 || explicitlyPaused ||
        (!playing && !Completed())) return;
    if (!std::isfinite(deltaSeconds) || !achievedVelocity.allFinite())
        throw std::invalid_argument("Visual root input must be finite");
    if (!inPlace)
    {
        ResetRootOffset("root-motion");
        return;
    }
    FootPlacementSolver::ValidateComponent(component);
    auto candidate = rootOffset;
    candidate.Update(component, achievedVelocity, deltaSeconds);
    auto joints = ReadKinematics(displayedLocals, asset->mesh.joints.size());
    Eigen::Matrix4f worldCorrection = Eigen::Matrix4f::Identity();
    worldCorrection.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(candidate.State().yawRadians, Eigen::Vector3f::UnitY()).toRotationMatrix();
    const Eigen::Vector3f pivot = component.block<3, 1>(0, 3);
    worldCorrection.block<3, 1>(0, 3) = pivot + candidate.State().worldTranslation -
        worldCorrection.block<3, 3>(0, 0) * pivot;
    const Eigen::Matrix4f localCorrection = component.inverse() * worldCorrection * component;
    joints.front().translation =
        (localCorrection * joints.front().translation.homogeneous()).head<3>();
    joints.front().rotation = (Eigen::Quaternionf(localCorrection.block<3, 3>(0, 0)).normalized() *
        joints.front().rotation).normalized();
    auto locals = WriteKinematics(displayedLocals, joints);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
    auto corrected = Evaluate(locals, discontinuity, models);
    poses.swap(corrected);
    preIkModels = models;
    jointModels.swap(models);
    visualLocals.swap(locals);
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    rootOffset = std::move(candidate);
}

void SkeletalPlayback::SetFootPlacementEnabled(bool enabled)
{
    if (enabled && !footPlacement)
    {
        footPlacement.emplace(asset->mesh);
        footPlacement->SetLockType(footLockType);
    }
    if (!footPlacement || footPlacement->State().enabled == enabled) return;
    footPlacement->SetEnabled(enabled);
    footPlacementSource.reset();
    if (!enabled) ResetFootPlacement("disabled");
}

void SkeletalPlayback::SetFootPlacementLockType(FootLockType type)
{
    if (footLockType == type) return;
    footLockType = type;
    if (!footPlacement) return;
    footPlacement->SetLockType(type);
    footPlacementSource.reset();
    ResetFootPlacement("lock-mode");
}

const FootPlacementState& SkeletalPlayback::FootPlacement() const
{
    static const FootPlacementState disabled;
    return footPlacement ? footPlacement->State() : disabled;
}

void SkeletalPlayback::SetSlopeWarpingEnabled(bool enabled)
{
    if (enabled)
    {
        if (!slopeWarping) slopeWarping.emplace(asset->mesh);
        slopeWarping->SetParameters(slopeWarpingSettings);
        slopeWarping->SetEnabled(true);
        return;
    }
    if (!slopeWarping) slopeWarping.emplace(asset->mesh);
    if (!slopeWarping->Enabled()) return;
    // 先复位并恢复基础姿态，再关闭 solver；ResetSlopeWarping 需要读取 active 状态。
    ResetSlopeWarping("disabled");
    slopeWarping->SetEnabled(false);
}

bool SkeletalPlayback::SlopeWarpingEnabled() const
{
    return slopeWarping && slopeWarping->Enabled();
}

void SkeletalPlayback::SetSlopeWarpingParameters(const SlopeWarpingParameters& parameters)
{
    slopeWarpingSettings = parameters;
    if (slopeWarping) slopeWarping->SetParameters(parameters);
}

void SkeletalPlayback::ResetSlopeWarping(const char* reason)
{
    if (!slopeWarping) return;
    const bool wasActive = slopeWarping->State().active;
    slopeWarping->Reset(reason);
    if (!wasActive) return;
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
    auto restored = Evaluate(visualLocals.empty() ? displayedLocals : visualLocals,
        discontinuity, models);
    poses.swap(restored);
    jointModels.swap(models);
    preIkModels = jointModels;
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
}

void SkeletalPlayback::ApplySlopeWarping(const Eigen::Matrix4f& component, bool grounded,
    const std::string& owner, float deltaSeconds, const GroundProbe& probe)
{
    if (!slopeWarping || !slopeWarping->Enabled() || deltaSeconds <= 0 ||
        explicitlyPaused || (!playing && !Completed())) return;
    FootPlacementSolver::ValidateComponent(component);
    const LocalPose& base = visualLocals.empty() ? displayedLocals : visualLocals;
    auto candidate = *slopeWarping;
    bool changed = false;
    const auto solved = candidate.Solve(asset->mesh, base, *preIkModels, component,
        grounded, owner, deltaSeconds, probe, changed);
    if (!changed)
    {
        const bool wasActive = slopeWarping->State().active;
        *slopeWarping = std::move(candidate);
        if (wasActive)
        {
            std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
            auto restored = Evaluate(base, discontinuity, models);
            poses.swap(restored);
            jointModels.swap(models);
            preIkModels = jointModels;
            slopeWarpedLocals.clear();
            footPlacementSource.reset();
        }
        return;
    }
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
    auto corrected = Evaluate(solved, discontinuity, models);
    poses.swap(corrected);
    jointModels.swap(models);
    preIkModels = jointModels;
    slopeWarpedLocals = solved;
    footPlacementSource.reset();
    *slopeWarping = std::move(candidate);
}

const SlopeWarpingState& SkeletalPlayback::SlopeWarping() const
{
    static const SlopeWarpingState disabled;
    return slopeWarping ? slopeWarping->State() : disabled;
}

void SkeletalPlayback::SetPoseDriver(std::shared_ptr<const PoseDriverSet> set)
{
    if (!set)
    {
        poseDriver.reset();
        return;
    }
    // 绑定期解析关节并预解 RBF 系数；失败直接抛，候选播放器由上层整体丢弃。
    poseDriver = std::make_shared<const PoseDriverSolver>(asset->mesh, std::move(set));
}

PoseDriverFrame SkeletalPlayback::SamplePoseDriver() const
{
    if (!poseDriver) return {};
    // PostProcess 在基础 locomotion 与斜坡修正之后执行；足 IK 不在 UE PostProcess 之前，
    // 所以这里刻意不使用 footPlacementSource。
    const LocalPose& base = slopeWarpedLocals.empty() ? displayedLocals : slopeWarpedLocals;
    if (base.empty()) return {};
    return poseDriver->Solve(base);
}

void SkeletalPlayback::SetPostProcess(std::shared_ptr<const PostProcessAsset> asset)
{
    if (!asset)
    {
        postProcess.reset();
        footPlacementLocals.clear();
        return;
    }
    // PostProcess 按目标网格解析驱动骨/目标骨；缺骨只降级该条目，不使构造失败。
    postProcess = std::make_shared<const PostProcessSolver>(this->asset->mesh, std::move(asset));
}

void SkeletalPlayback::ApplyPostProcess(float deltaSeconds)
{
    if (!postProcess || !postProcess->Valid() || deltaSeconds <= 0 || explicitlyPaused ||
        (!playing && !Completed()))
        return;
    // 依次回退：足 IK 结果 → 斜坡修正 → 基础姿态。PostProcess 是最后一道姿态修正，
    // 因此不能从 displayedLocals 重新求值，否则会丢掉足 IK。
    // footPlacementSource==preIkModels 是足 IK 结果的唯一新鲜度标记；Tick/Reset 会换掉
    // preIkModels，旧 IK 局部姿态随之失效。
    const bool hasFootIk = !footPlacementLocals.empty() && footPlacementSource &&
        footPlacementSource == preIkModels;
    const LocalPose& base = hasFootIk ? footPlacementLocals :
        (!slopeWarpedLocals.empty() ? slopeWarpedLocals : displayedLocals);
    if (base.empty()) return;
    LocalPose corrected = base;
    if (!postProcess->Apply(corrected, &postProcessStats)) return;
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
    auto next = Evaluate(corrected, discontinuity, models);
    poses.swap(next);
    jointModels.swap(models);
    ++postProcessApplications;
    // preIkModels 保持“足 IK 前”的语义，下一帧 FootPlacement 仍以它为求解源。
}

void SkeletalPlayback::CopyPoseGlobals(const Eigen::Matrix4f& component,
    std::vector<Eigen::Matrix4f>& globals) const
{
    // 与 PostProcess 使用同一份“最终基础姿态”。
    const bool hasFootIk = !footPlacementLocals.empty() && footPlacementSource &&
        footPlacementSource == preIkModels;
    const LocalPose& base = hasFootIk ? footPlacementLocals :
        (!slopeWarpedLocals.empty() ? slopeWarpedLocals : displayedLocals);
    if (base.empty())
    {
        globals.clear();
        return;
    }
    globals = LocalToModels(asset->mesh, base);
    for (auto& value : globals)
        value = component * value;
}

void SkeletalPlayback::ApplyPoseGlobals(const Eigen::Matrix4f& component,
    const std::vector<Eigen::Matrix4f>& globals)
{
    if (globals.size() != asset->mesh.joints.size() || globals.empty())
        return;
    const bool hasFootIk = !footPlacementLocals.empty() && footPlacementSource &&
        footPlacementSource == preIkModels;
    LocalPose base = hasFootIk ? footPlacementLocals :
        (!slopeWarpedLocals.empty() ? slopeWarpedLocals : displayedLocals);
    if (base.empty()) return;

    const Eigen::Matrix4f inverseComponent = component.inverse();
    std::vector<Eigen::Matrix4f> models(globals.size());
    for (size_t joint = 0; joint < globals.size(); ++joint)
        models[joint] = inverseComponent * globals[joint];
    for (size_t joint = 0; joint < models.size(); ++joint)
    {
        const int parent = asset->mesh.joints[joint].parent;
        const Eigen::Matrix4f parentModel = parent >= 0 ?
            models[static_cast<size_t>(parent)] : Eigen::Matrix4f::Identity();
        const Eigen::Matrix4f local = parentModel.inverse() * models[joint];
        const size_t lane = joint % 4;
        const auto& translation = local.block<3, 1>(0, 3);
        auto& value = base[joint / 4].translation;
        float lanes[4];
        ozz::math::StorePtrU(value.x, lanes);
        lanes[lane] = translation.x();
        value.x = ozz::math::simd_float4::LoadPtrU(lanes);
        ozz::math::StorePtrU(value.y, lanes);
        lanes[lane] = translation.y();
        value.y = ozz::math::simd_float4::LoadPtrU(lanes);
        ozz::math::StorePtrU(value.z, lanes);
        lanes[lane] = translation.z();
        value.z = ozz::math::simd_float4::LoadPtrU(lanes);

        const Eigen::Quaternionf rotation(local.block<3, 3>(0, 0));
        auto& quaternion = base[joint / 4].rotation;
        ozz::math::StorePtrU(quaternion.x, lanes);
        lanes[lane] = rotation.x();
        quaternion.x = ozz::math::simd_float4::LoadPtrU(lanes);
        ozz::math::StorePtrU(quaternion.y, lanes);
        lanes[lane] = rotation.y();
        quaternion.y = ozz::math::simd_float4::LoadPtrU(lanes);
        ozz::math::StorePtrU(quaternion.z, lanes);
        lanes[lane] = rotation.z();
        quaternion.z = ozz::math::simd_float4::LoadPtrU(lanes);
        ozz::math::StorePtrU(quaternion.w, lanes);
        lanes[lane] = rotation.w();
        quaternion.w = ozz::math::simd_float4::LoadPtrU(lanes);
    }
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> modelsOut;
    auto next = Evaluate(base, discontinuity, modelsOut);
    poses.swap(next);
    jointModels.swap(modelsOut);
}

void SkeletalPlayback::ResetFootPlacement(const char* reason)
{
    if (!footPlacement) return;
    // 重建纯 pre-IK palette 后才提交，旧不可变姿态仍供已发布快照使用。
    if (jointModels != preIkModels)
    {
        std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
        auto restored = Evaluate(!slopeWarpedLocals.empty() ? slopeWarpedLocals :
            visualLocals.empty() ? displayedLocals : visualLocals, discontinuity, models);
        poses.swap(restored);
        jointModels = preIkModels;
    }
    footPlacement->Reset(reason);
    footPlacementSource.reset();
}

void SkeletalPlayback::ApplyFootPlacement(const Eigen::Matrix4f& component, bool grounded,
    const std::string& owner, float deltaSeconds, const GroundProbe& probe)
{
    if (!footPlacement || !footPlacement->State().enabled || deltaSeconds <= 0 || explicitlyPaused ||
        (!playing && !Completed()) ||
        (footPlacementSource == preIkModels && !Completed())) return;
    if (!inPlace)
    {
        ResetFootPlacement("root-motion");
        return;
    }
    // 脚锁状态与求解后的各 section 姿态先在候选上完成；任何异常不污染活动反馈。
    auto candidate = *footPlacement;
    const auto solved = candidate.Solve(asset->mesh,
        !slopeWarpedLocals.empty() ? slopeWarpedLocals :
            visualLocals.empty() ? displayedLocals : visualLocals, *preIkModels,
        component, grounded, owner, deltaSeconds, probe);
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> models;
    auto corrected = Evaluate(solved, discontinuity, models);
    candidate.Measure(*models, component);
    poses.swap(corrected);
    jointModels.swap(models);
    *footPlacement = std::move(candidate);
    footPlacementSource = preIkModels;
    footPlacementLocals = solved;
}

std::shared_ptr<SkeletalPlayback> SkeletalPlayback::NewInstance() const
{
    return std::make_shared<SkeletalPlayback>(asset);
}

void SkeletalPlayback::Seek(float seconds)
{
    if (!std::isfinite(seconds) || seconds < PlaybackRangeStart() || seconds > PlaybackRangeEnd())
        throw std::invalid_argument("Seek must be inside the playback range");
    size_t sampleClip = selected;
    float clipTime = seconds;
    size_t segmentIndex = montageSegment;
    if (!montageSegments.empty())
    {
        const MontageSample sample = SampleMontageTimeline(seconds, seconds >= PlaybackRangeEnd());
        sampleClip = sample.clip;
        clipTime = sample.clipTime;
        segmentIndex = sample.segment;
    }
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> nextModels;
    auto locals = Sample(sampleClip, clipTime);
    auto next = Evaluate(locals, discontinuity + 1, nextModels);
    // 显式 seek 是硬跳变：清空活动区间且不补发被跳过的 begin/end（与 UE 的 seek 语义一致）。
    // 事件缓冲也一并丢弃，下一 Tick 只处理 seconds 之后的位置穿越。
    std::fill(notifyActive.begin(), notifyActive.end(), 0);
    RefreshActiveNotifyStates();
    firedNotifies.clear();
    pendingNotifies.clear();
    poses.swap(next);
    jointModels.swap(nextModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    displayedLocals.swap(locals);
    blendSource.clear();
    inertialization.Clear();
    time = seconds;
    selected = sampleClip;
    montageSegment = segmentIndex;
    activeSyncGroup.clear();
    ClearSyncSourceClock();
    syncDecisionValid = false;
    syncLeader.clear();
    syncPendingValid = false;
    syncPendingHasTime = false;
    RefreshSyncMarker();
    ++discontinuity;
    rootOffset.Reset("seek");
    ResetFootPlacement("seek");
}

void SkeletalPlayback::Select(size_t clipIndex)
{
    if (clipIndex >= asset->AnimationAsset().clips.size())
        throw std::out_of_range("Unknown animation clip");
    std::shared_ptr<const std::vector<Eigen::Matrix4f>> nextModels;
    auto locals = Sample(clipIndex, 0);
    auto next = Evaluate(locals, discontinuity + 1, nextModels);
    // 切片段：旧片段的活动 notifyState 补 end(interrupted)，事件延到下一 Tick 被反馈读取。
    // Montage 覆盖层、section 链与播放范围都属于上一播放段，随 Select 复位到整段片段。
    EndActiveNotifyStates(time, true, true);
    montageNotifies.clear();
    montageId.clear();
    montageLength = 0;
    montageSections.clear();
    montageSection = 0;
    ClearMontageSegments();
    playbackRangeStart = 0;
    playbackRangeEnd = 0;
    poses.swap(next);
    jointModels.swap(nextModels);
    preIkModels = jointModels;
    motionModels = jointModels;
    visualLocals.clear();
    slopeWarpedLocals.clear();
    footPlacementSource.reset();
    displayedLocals.swap(locals);
    blendSource.clear();
    inertialization.Clear();
    selected = clipIndex;
    time = 0;
    activeSyncGroup.clear();
    ClearSyncSourceClock();
    syncDecisionValid = false;
    syncLeader.clear();
    syncPendingValid = false;
    syncPendingHasTime = false;
    RefreshSyncMarker();
    ++discontinuity;
    rootOffset.Reset("select");
    ResetFootPlacement("select");
    RebuildNotifyEvents();
}
}
