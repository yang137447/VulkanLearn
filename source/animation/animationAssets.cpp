#include "animationAssets.h"

#include <cmath>
#include <stdexcept>
#include <ozz/animation/offline/raw_animation_utils.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>

namespace VL::Animation
{
namespace
{
void SetLane(ozz::math::SimdFloat4& target, float value)
{
    target = ozz::math::SetX(target, ozz::math::simd_float4::Load1(value));
}

void RestoreRoot(const Clip& clip, float time, ozz::math::SoaTransform& first)
{
    ozz::math::Transform root;
    if (!ozz::animation::offline::SampleTrack(clip.raw.tracks[0], time, &root))
    {
        throw std::runtime_error("Raw root track sampling failed");
    }
    // ozz SamplingJob 对插值系数使用 RcpEst，长根位移会放大误差；
    // 根轨道保留原始精度并在层级求值前覆盖 lane 0，身体关节仍走压缩采样。
    SetLane(first.translation.x, root.translation.x);
    SetLane(first.translation.y, root.translation.y);
    SetLane(first.translation.z, root.translation.z);
    SetLane(first.rotation.x, root.rotation.x);
    SetLane(first.rotation.y, root.rotation.y);
    SetLane(first.rotation.z, root.rotation.z);
    SetLane(first.rotation.w, root.rotation.w);
    SetLane(first.scale.x, root.scale.x);
    SetLane(first.scale.y, root.scale.y);
    SetLane(first.scale.z, root.scale.z);
}

void ValidateSamplingAssets(const SkeletonMesh& mesh, const Clip& clip)
{
    if (!mesh.runtime || !clip.runtime ||
        mesh.runtime->num_joints() != clip.runtime->num_tracks() ||
        mesh.joints.size() != static_cast<size_t>(mesh.runtime->num_joints()))
        throw std::invalid_argument("Animation sampling requires matching imported assets");
}

void ValidateSampleTime(const Clip& clip, float time)
{
    if (!std::isfinite(time) || time < 0 || time > clip.runtime->duration())
        throw std::invalid_argument("Animation time must be finite and inside the clip");
}

LocalPose SampleWithContext(const SkeletonMesh& mesh, const Clip& clip, float time,
    ozz::animation::SamplingJob::Context& context)
{
    LocalPose locals(mesh.runtime->num_soa_joints());
    ozz::animation::SamplingJob sampling;
    sampling.animation = clip.runtime.get();
    sampling.context = &context;
    sampling.ratio = time / clip.runtime->duration();
    sampling.output = ozz::make_span(locals);
    if (!sampling.Run())
        throw std::runtime_error("Imported animation sampling failed");
    RestoreRoot(clip, time, locals[0]);
    return locals;
}
}

Eigen::Matrix4f FbxToVlBasis()
{
    Eigen::Matrix4f basis = Eigen::Matrix4f::Zero();
    basis(0, 0) = 1;
    basis(1, 2) = 1;
    basis(2, 1) = -1;
    basis(3, 3) = 1;
    return basis;
}

std::vector<std::vector<Eigen::Matrix4f>> SampleModels(
    const SkeletonMesh& mesh, const Clip& clip, const std::vector<float>& times)
{
    ValidateSamplingAssets(mesh, clip);
    for (float time : times) ValidateSampleTime(clip, time);
    // 一批时刻复用同片段 context，仍可倒序 seek；空批次也验证资产入口。
    ozz::animation::SamplingJob::Context context(mesh.runtime->num_joints());
    std::vector<std::vector<Eigen::Matrix4f>> frames;
    for (float time : times)
        frames.push_back(LocalToModels(mesh, SampleWithContext(mesh, clip, time, context)));
    return frames;
}

LocalPose SampleLocals(const SkeletonMesh& mesh, const Clip& clip, float time)
{
    ValidateSamplingAssets(mesh, clip);
    ValidateSampleTime(clip, time);
    // context 仅绑定本次片段；独立调用不会误复用已销毁动画地址对应的缓存。
    ozz::animation::SamplingJob::Context context(mesh.runtime->num_joints());
    return SampleWithContext(mesh, clip, time, context);
}

std::vector<Eigen::Matrix4f> LocalToModels(const SkeletonMesh& mesh, const LocalPose& locals)
{
    if (!mesh.runtime || locals.size() != static_cast<size_t>(mesh.runtime->num_soa_joints()))
        throw std::invalid_argument("Local pose requires a matching imported skeleton");
    ozz::vector<ozz::math::Float4x4> models(mesh.runtime->num_joints());
    ozz::animation::LocalToModelJob hierarchy;
    hierarchy.skeleton = mesh.runtime.get();
    hierarchy.input = ozz::make_span(locals);
    hierarchy.output = ozz::make_span(models);
    if (!hierarchy.Run())
        throw std::runtime_error("Imported animation hierarchy failed");
    std::vector<Eigen::Matrix4f> frame(models.size());
    for (size_t joint = 0; joint < models.size(); ++joint)
    {
        for (int column = 0; column < 4; ++column)
        {
            ozz::math::StorePtrU(models[joint].cols[column], frame[joint].col(column).data());
        }
    }
    return frame;
}
}
