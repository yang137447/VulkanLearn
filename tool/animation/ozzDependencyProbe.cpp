#include "ozzDependencyProbe.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_animation_utils.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/maths/transform.h>

#include "shader/build/contentHash.h"

namespace VL::AnimationPreflight
{
namespace
{
template<typename T>
void ReadTrustedFixture(const std::filesystem::path& path, T& value)
{
    // ozz archive 自身不防御损坏/截断数据；仅允许本工具读取锁定依赖自带的可信文件。
    // 此函数不能复用为正式动画资产加载器，后者需要在作者数据边界建立校验与预算。
    ozz::io::File file(path.string().c_str(), "rb");
    if (!file.opened())
    {
        throw std::runtime_error("Cannot open dependency fixture: " + path.string());
    }
    ozz::io::IArchive archive(&file);
    if (!archive.TestTag<T>())
    {
        throw std::runtime_error("Unexpected dependency fixture tag: " + path.string());
    }
    archive >> value;
}

ModelPositions ReadPositions(const ozz::vector<ozz::math::Float4x4>& matrices)
{
    ModelPositions positions;
    positions.reserve(matrices.size());
    for (const auto& matrix : matrices)
    {
        // ozz 矩阵按列存储；这里只记录源空间平移，不提前假设它就是 VL 的厘米/y-up。
        float column[4];
        ozz::math::StorePtrU(matrix.cols[3], column);
        for (float component : column)
        {
            if (!std::isfinite(component))
            {
                throw std::runtime_error("Non-finite model-space joint position");
            }
        }
        positions.push_back({column[0], column[1], column[2]});
    }
    return positions;
}

ModelPositions SampleRawReference(
    const ozz::animation::Skeleton& skeleton,
    const ozz::animation::offline::RawAnimation& animation,
    float time)
{
    ozz::vector<ozz::math::Transform> locals(skeleton.num_joints());
    if (!ozz::animation::offline::SampleAnimation(animation, time, ozz::make_span(locals)))
    {
        throw std::runtime_error("Raw animation reference sampling failed");
    }
    ozz::vector<ozz::math::Float4x4> models(skeleton.num_joints());
    const auto parents = skeleton.joint_parents();
    for (int joint = 0; joint < skeleton.num_joints(); ++joint)
    {
        const auto local = ozz::math::Float4x4::FromAffine(locals[joint]);
        const int parent = parents[joint];
        models[joint] = parent == ozz::animation::Skeleton::kNoParent
            ? local : models[parent] * local;
    }
    return ReadPositions(models);
}

float Distance(const JointPosition& left, const JointPosition& right)
{
    const float x = left[0] - right[0];
    const float y = left[1] - right[1];
    const float z = left[2] - right[2];
    return std::sqrt(x * x + y * y + z * z);
}
}

std::vector<ModelPositions> SampleModelPositions(
    const ozz::animation::Skeleton& skeleton,
    const ozz::animation::Animation& animation,
    const std::vector<float>& ratios)
{
    // 在工具入口一次性验证样本定义；不借 ozz 的 ratio clamp 掩盖无效作者输入。
    if (skeleton.num_joints() <= 0 || skeleton.num_joints() != animation.num_tracks() ||
        !std::isfinite(animation.duration()) || animation.duration() <= 0.0f)
    {
        throw std::invalid_argument("Expected nonempty matching skeleton/animation tracks");
    }
    for (float ratio : ratios)
    {
        if (!std::isfinite(ratio) || ratio < 0.0f || ratio > 1.0f)
        {
            throw std::invalid_argument("Sample ratio must be finite and in [0, 1]");
        }
    }

    // context 只属于这次动画求值，不跨动画地址复用；避免上游文档提示的地址重用缓存问题。
    ozz::animation::SamplingJob::Context context(skeleton.num_joints());
    ozz::vector<ozz::math::SoaTransform> locals(skeleton.num_soa_joints());
    ozz::vector<ozz::math::Float4x4> models(skeleton.num_joints());
    std::vector<ModelPositions> result;
    result.reserve(ratios.size());
    for (float ratio : ratios)
    {
        ozz::animation::SamplingJob sampling;
        sampling.animation = &animation;
        sampling.context = &context;
        sampling.ratio = ratio;
        sampling.output = ozz::make_span(locals);
        if (!sampling.Run())
        {
            throw std::runtime_error("ozz sampling failed");
        }
        ozz::animation::LocalToModelJob hierarchy;
        hierarchy.skeleton = &skeleton;
        hierarchy.input = ozz::make_span(locals);
        hierarchy.output = ozz::make_span(models);
        if (!hierarchy.Run())
        {
            throw std::runtime_error("ozz local-to-model failed");
        }
        result.push_back(ReadPositions(models));
    }
    return result;
}

nlohmann::json BuildDependencyReport()
{
    const std::filesystem::path root(VL_OZZ_MEDIA_ROOT);
    const auto skeletonPath = root / "pab_skeleton.ozz";
    const auto rawPath = root / "pab_walk_raw.ozz";
    ozz::animation::Skeleton skeleton;
    ozz::animation::offline::RawAnimation raw;
    ReadTrustedFixture(skeletonPath, skeleton);
    ReadTrustedFixture(rawPath, raw);
    if (!raw.Validate() || !std::isfinite(raw.duration) ||
        raw.num_tracks() != skeleton.num_joints())
    {
        throw std::runtime_error("Invalid reference clip or skeleton track mismatch");
    }
    // 从真实原始动作构建本机运行时数据，避免只加载一个预烘焙文件就声称转换链验证完成。
    ozz::animation::offline::AnimationBuilder builder;
    const auto animation = builder(raw);
    if (!animation)
    {
        throw std::runtime_error("ozz animation builder failed");
    }
    constexpr int SampleCount = 121;
    std::vector<float> ratios;
    for (int index = 0; index < SampleCount; ++index)
    {
        ratios.push_back(static_cast<float>(index) / (SampleCount - 1));
    }
    const auto sampled = SampleModelPositions(skeleton, *animation, ratios);
    float maxError = 0.0f;
    float maxTravel = 0.0f;
    nlohmann::json frames = nlohmann::json::array();
    for (int frame = 0; frame < SampleCount; ++frame)
    {
        const auto reference = SampleRawReference(skeleton, raw, ratios[frame] * raw.duration);
        for (int joint = 0; joint < skeleton.num_joints(); ++joint)
        {
            maxError = std::max(maxError, Distance(reference[joint], sampled[frame][joint]));
            maxTravel = std::max(maxTravel, Distance(sampled.front()[joint], sampled[frame][joint]));
        }
        frames.push_back({{"time", ratios[frame] * raw.duration}, {"positions", sampled[frame]}});
    }
    nlohmann::json joints = nlohmann::json::array();
    for (int index = 0; index < skeleton.num_joints(); ++index)
    {
        joints.push_back({{"name", skeleton.joint_names()[index]},
            {"parent", skeleton.joint_parents()[index]}});
    }
    return {
        {"format", "vl-animation-dependency-probe-v1"},
        {"scope", "Dependency preflight only; not GASP assets or VL runtime animation"},
        {"dependency", {{"name", "ozz-animation"}, {"version", "0.16.0"},
            {"commit", "6cbdc790123aa4731d82e255df187b3a8a808256"}}},
        {"fixture", {{"skeleton", "media/bin/pab_skeleton.ozz"},
            {"rawClip", "media/bin/pab_walk_raw.ozz"},
            {"skeletonBlake3", ContentHasher::HashFile(skeletonPath).ToHex()},
            {"rawClipBlake3", ContentHasher::HashFile(rawPath).ToHex()}}},
        {"space", "Unmodified source model space; no VL unit/axis conversion"},
        {"duration", raw.duration}, {"jointCount", skeleton.num_joints()},
        {"trackCount", raw.num_tracks()}, {"sampleCount", SampleCount},
        {"maxModelPositionError", maxError}, {"maxJointTravel", maxTravel},
        {"joints", std::move(joints)}, {"frames", std::move(frames)}
    };
}
}
