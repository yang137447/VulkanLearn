#include "particles/particleSkeletalPose.h"

#include "animation/animationAssets.h"
#include "animation/skeletalPlayback.h"

namespace VL
{

std::shared_ptr<const std::vector<Eigen::Matrix4f>>
SampleParticleSkeletalModels(
    const Animation::SkeletalAsset& asset,
    std::size_t clipIndex,
    float time,
    bool loop)
{
    using namespace VL::Animation;
    // 没有片段或片段无效时退回绑定姿态：UE Niagara 的 SkeletalMesh Renderer
    // 在未指定动画时同样显示 rest pose，而不是空网格。
    if (asset.clips.empty() || clipIndex >= asset.clips.size())
    {
        auto models = std::make_shared<std::vector<Eigen::Matrix4f>>();
        models->reserve(asset.mesh.joints.size());
        for (const Joint& joint : asset.mesh.joints)
        {
            models->push_back(joint.restModel);
        }
        return models;
    }
    const Clip& clip = asset.clips.at(clipIndex);
    float sampleTime = time;
    if (loop)
    {
        const float duration = std::max(
            clip.runtime ? clip.runtime->duration() : 0.0f,
            1.0e-4f);
        sampleTime = std::fmod(sampleTime, duration);
        if (sampleTime < 0.0f)
        {
            sampleTime += duration;
        }
    }
    const LocalPose locals = SampleLocals(asset.mesh, clip, sampleTime);
    return std::make_shared<const std::vector<Eigen::Matrix4f>>(
        LocalToModels(asset.mesh, locals));
}

float WrapParticleSkeletalPoseTime(
    float baseTime,
    float elapsedSeconds,
    float rate,
    float duration,
    bool loop)
{
    float time = baseTime + elapsedSeconds * rate;
    const float safeDuration = std::max(duration, 1.0e-4f);
    if (loop)
    {
        time = std::fmod(time, safeDuration);
        if (time < 0.0f)
        {
            time += safeDuration;
        }
        return time;
    }
    return std::clamp(time, 0.0f, safeDuration);
}

} // namespace VL
