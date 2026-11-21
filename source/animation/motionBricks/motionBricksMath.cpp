#include "motionBricksMath.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace VL::Animation::MotionBricks
{
namespace
{
constexpr int kSomaJointCount = 30;
constexpr int kSomaPositionCount = kSomaJointCount - 1;
constexpr int kSomaRotationOffset = kSomaPositionCount * 3;
constexpr int kPredictedRootOffset = 0;
constexpr int kPredictedPositionOffset = 5;
constexpr int kPredictedRotationOffset = kPredictedPositionOffset + kSomaPositionCount * 3;
constexpr int kPredictedContactOffset = 366 - 4;

void ValidateFinite(const Transform& value, const char* context)
{
    if (!value.position.allFinite() || !value.rotation.coeffs().allFinite())
        throw std::invalid_argument(std::string(context) + " contains a non-finite transform");
}

void ValidatePose(const SkeletonBinding& binding, const Pose& pose, const char* context)
{
    if (pose.size() != binding.jointNames.size())
        throw std::invalid_argument(std::string(context) + " joint count does not match the skeleton");
    for (const Transform& value : pose)
        ValidateFinite(value, context);
}

void ValidateBinding(const SkeletonBinding& binding)
{
    const size_t jointCount = binding.jointNames.size();
    if (jointCount == 0)
        throw std::invalid_argument("MotionBricks skeleton needs at least one joint");
    if (binding.parentIndices.size() != jointCount ||
        binding.referenceLocalPose.size() != jointCount ||
        binding.poseToModel.size() != jointCount)
        throw std::invalid_argument("MotionBricks skeleton arrays have inconsistent sizes");
    if (binding.modelToPose.size() != kSomaJointCount)
        throw std::invalid_argument("SOMA binding must map 30 model joints");
    if (binding.modelToPose[0] != 0)
        throw std::invalid_argument("SOMA Hips must be the runtime skeleton's root joint");
    if (binding.parentIndices[0] != -1)
        throw std::invalid_argument("The runtime skeleton must have one root at index zero");
    for (size_t joint = 1; joint < jointCount; ++joint)
    {
        const int parent = binding.parentIndices[joint];
        if (parent < 0 || parent >= static_cast<int>(joint))
            throw std::invalid_argument("MotionBricks skeleton parents must precede their children");
    }
    for (size_t joint = 0; joint < jointCount; ++joint)
        ValidateFinite(binding.referenceLocalPose[joint], "reference pose");
    for (size_t modelJoint = 0; modelJoint < binding.modelToPose.size(); ++modelJoint)
    {
        const int poseJoint = binding.modelToPose[modelJoint];
        if (poseJoint < 0 || poseJoint >= static_cast<int>(jointCount))
            throw std::invalid_argument("modelToPose contains an out-of-range joint");
    }
    for (size_t poseJoint = 0; poseJoint < jointCount; ++poseJoint)
    {
        const int modelJoint = binding.poseToModel[poseJoint];
        if (modelJoint < 0) continue;
        if (modelJoint >= static_cast<int>(binding.modelToPose.size()) ||
            binding.modelToPose[static_cast<size_t>(modelJoint)] != static_cast<int>(poseJoint))
            throw std::invalid_argument("modelToPose and poseToModel disagree");
        const int parent = binding.parentIndices[poseJoint];
        if (parent >= 0 && binding.poseToModel[static_cast<size_t>(parent)] < 0)
            throw std::invalid_argument("A mapped SOMA joint has an unmapped parent");
    }
    if (binding.rightLegJoint < 0 || binding.rightLegJoint >= static_cast<int>(jointCount) ||
        binding.leftLegJoint < 0 || binding.leftLegJoint >= static_cast<int>(jointCount))
        throw std::invalid_argument("SOMA binding is missing RightLeg or LeftLeg");
}

Eigen::Vector3f AlignedPosition(const Transform& alignment, const Eigen::Vector3f& position)
{
    return alignment.rotation.conjugate() * (position - alignment.position);
}

Eigen::Vector3f Rotate(const Eigen::Quaternionf& rotation, const Eigen::Vector3f& vector)
{
    return rotation * vector;
}

float HeadingFromHipPositions(const Eigen::Vector3f& right, const Eigen::Vector3f& left)
{
    Eigen::Vector3f across(right.x() - left.x(), 0.0f, right.z() - left.z());
    const float length = across.norm();
    if (length <= 1e-6f) return 0.0f;
    across /= length;
    const Eigen::Vector3f forward = Eigen::Vector3f::UnitY().cross(across);
    return std::atan2(forward.x(), forward.z());
}

Transform ComputeGuideAlignment(const SkeletonBinding& binding, const PoseFrames& world,
    const Eigen::Vector2f& desiredHeading)
{
    const Pose& lastContext = world[3];
    const float currentAngle = HeadingFromHipPositions(
        lastContext[binding.rightLegJoint].position,
        lastContext[binding.leftLegJoint].position);
    const float desiredAngle = std::atan2(desiredHeading.x(), desiredHeading.y());

    Transform alignment;
    alignment.position = Eigen::Vector3f(lastContext[0].position.x(), 0.0f,
        lastContext[0].position.z());
    alignment.rotation = Eigen::Quaternionf(
        Eigen::AngleAxisf(currentAngle - desiredAngle, Eigen::Vector3f::UnitY()));
    return Normalized(alignment);
}

Eigen::Quaternionf ReadRotation6D(const std::array<float, 366>& features, int offset,
    const char* context)
{
    Eigen::Vector3f xRaw(features[offset], features[offset + 1], features[offset + 2]);
    Eigen::Vector3f yRaw(features[offset + 3], features[offset + 4], features[offset + 5]);
    if (!xRaw.allFinite() || !yRaw.allFinite())
        throw std::invalid_argument(std::string(context) + " has a non-finite 6D rotation");
    const float xLength = xRaw.norm();
    if (xLength <= 1e-8f)
        throw std::invalid_argument(std::string(context) + " has a degenerate 6D rotation");
    const Eigen::Vector3f xAxis = xRaw / xLength;
    Eigen::Vector3f zAxis = xAxis.cross(yRaw);
    const float zLength = zAxis.norm();
    if (zLength <= 1e-8f)
        throw std::invalid_argument(std::string(context) + " has a degenerate 6D rotation");
    zAxis /= zLength;
    const Eigen::Vector3f yAxis = zAxis.cross(xAxis);

    Eigen::Matrix3f matrix;
    matrix.col(0) = xAxis;
    matrix.col(1) = yAxis;
    matrix.col(2) = zAxis;
    Eigen::Quaternionf rotation(matrix);
    rotation.normalize();
    return rotation;
}

void WriteRotation6D(const Eigen::Quaternionf& rotation, std::array<float, 267>& values,
    int offset)
{
    const Eigen::Vector3f xAxis = Rotate(rotation, Eigen::Vector3f::UnitX());
    const Eigen::Vector3f yAxis = Rotate(rotation, Eigen::Vector3f::UnitY());
    values[offset] = xAxis.x();
    values[offset + 1] = xAxis.y();
    values[offset + 2] = xAxis.z();
    values[offset + 3] = yAxis.x();
    values[offset + 4] = yAxis.y();
    values[offset + 5] = yAxis.z();
}

Transform ApplyDelta(const Transform& transform, const Transform& delta, float weight)
{
    Transform weighted;
    weighted.position = delta.position * weight;
    const Eigen::Quaternionf identity = Eigen::Quaternionf::Identity();
    weighted.rotation = identity.slerp(weight, delta.rotation).normalized();
    return Compose(transform, weighted);
}

Transform ComputeRootEndpointDelta(const Transform& generated, const Transform& guide)
{
    Transform delta;
    delta.position = guide.position - generated.position;
    delta.rotation = (generated.rotation.normalized().conjugate() *
        guide.rotation.normalized()).normalized();
    return delta;
}

Transform ApplyRootCorrection(const Transform& root, const Transform& start,
    const Transform& end, float endWeight)
{
    const float startWeight = 1.0f - endWeight;
    const Eigen::Quaternionf identity = Eigen::Quaternionf::Identity();
    const Eigen::Quaternionf startRotation = identity.slerp(startWeight, start.rotation);
    const Eigen::Quaternionf endRotation = identity.slerp(endWeight, end.rotation);

    Transform corrected;
    corrected.position = root.position + start.position * startWeight + end.position * endWeight;
    corrected.rotation = (root.rotation * startRotation * endRotation).normalized();
    return corrected;
}

struct FrameSample
{
    int frame = 0;
    int next = 0;
    float blend = 0;
};

FrameSample SampleFrames(float time, float sampleRate, size_t frameCount)
{
    if (!std::isfinite(time) || !std::isfinite(sampleRate) || sampleRate <= 0)
        throw std::invalid_argument("MotionBricks sample time and rate must be finite and positive");
    if (frameCount == 0)
        throw std::invalid_argument("Cannot sample an empty MotionBricks animation");

    const int lastFrame = static_cast<int>(frameCount) - 1;
    const float fractional = time * sampleRate;
    FrameSample sample;
    sample.frame = std::clamp(static_cast<int>(std::floor(fractional)), 0, lastFrame);
    sample.next = std::min(sample.frame + 1, lastFrame);
    sample.blend = sample.frame < lastFrame ?
        std::clamp(fractional - static_cast<float>(sample.frame), 0.0f, 1.0f) : 0.0f;
    return sample;
}
}

Transform Compose(const Transform& parent, const Transform& local)
{
    Transform result;
    result.rotation = (parent.rotation * local.rotation).normalized();
    result.position = parent.position + Rotate(parent.rotation, local.position);
    return result;
}

Transform Inverse(const Transform& value)
{
    Transform result;
    result.rotation = value.rotation.conjugate().normalized();
    result.position = Rotate(result.rotation, -value.position);
    return result;
}

Transform Blend(const Transform& from, const Transform& to, float alpha)
{
    Transform result;
    result.position = from.position + (to.position - from.position) * alpha;
    result.rotation = from.rotation.slerp(alpha, to.rotation).normalized();
    return result;
}

Transform Normalized(Transform value)
{
    if (!value.position.allFinite() || !value.rotation.coeffs().allFinite())
        throw std::invalid_argument("MotionBricks transform contains a non-finite value");
    const float length = value.rotation.norm();
    if (!(length > 1e-6f))
        throw std::invalid_argument("MotionBricks transform has a degenerate rotation");
    value.rotation.normalize();
    return value;
}

SkeletonBinding BindSomaSkeleton(const ModelContract& contract,
    std::vector<std::string> jointNames, std::vector<int> parentIndices,
    Pose referenceLocalPose)
{
    ValidateSomaContract(contract);
    if (jointNames.empty())
        throw std::invalid_argument("SOMA runtime skeleton must have at least one joint");
    if (parentIndices.size() != jointNames.size() ||
        referenceLocalPose.size() != jointNames.size())
        throw std::invalid_argument("SOMA runtime skeleton arrays have inconsistent sizes");
    if (parentIndices[0] != -1)
        throw std::invalid_argument("SOMA runtime skeleton must have its root at index zero");
    for (size_t joint = 1; joint < jointNames.size(); ++joint)
    {
        const int parent = parentIndices[joint];
        if (parent < 0 || parent >= static_cast<int>(joint))
            throw std::invalid_argument("SOMA runtime skeleton parents must precede their children");
    }

    std::map<std::string, int> nameToJoint;
    for (size_t joint = 0; joint < jointNames.size(); ++joint)
    {
        if (jointNames[joint].empty())
            throw std::invalid_argument("SOMA runtime skeleton has an empty joint name");
        if (!nameToJoint.emplace(jointNames[joint], static_cast<int>(joint)).second)
            throw std::invalid_argument("SOMA runtime skeleton has duplicate joint name '" +
                jointNames[joint] + "'");
    }

    SkeletonBinding binding;
    binding.jointNames = std::move(jointNames);
    binding.parentIndices = std::move(parentIndices);
    binding.referenceLocalPose = std::move(referenceLocalPose);
    for (Transform& reference : binding.referenceLocalPose)
        reference = Normalized(reference);
    binding.poseToModel.assign(binding.jointNames.size(), -1);
    binding.modelToPose.resize(contract.jointNames.size(), -1);

    std::string missing;
    for (size_t modelJoint = 0; modelJoint < contract.jointNames.size(); ++modelJoint)
    {
        const auto found = nameToJoint.find(contract.jointNames[modelJoint]);
        if (found == nameToJoint.end())
        {
            if (!missing.empty()) missing += ", ";
            missing += contract.jointNames[modelJoint];
            continue;
        }
        binding.modelToPose[modelJoint] = found->second;
    }
    if (!missing.empty())
        throw std::invalid_argument("SOMA runtime skeleton is missing model joints: " + missing);
    if (binding.modelToPose[0] != 0)
        throw std::invalid_argument("SOMA Hips must be the runtime skeleton's root joint");

    // Model joints outside the connected hierarchy keep their reference local pose
    // during decoding, matching MotionBricksSOMAEngine::_bind_pose_skeleton().
    for (size_t modelJoint = 0; modelJoint < binding.modelToPose.size(); ++modelJoint)
    {
        const int poseJoint = binding.modelToPose[modelJoint];
        const int parent = binding.parentIndices[poseJoint];
        if (parent == -1 || binding.poseToModel[parent] >= 0)
            binding.poseToModel[poseJoint] = static_cast<int>(modelJoint);
    }

    const auto rightLeg = nameToJoint.find("RightLeg");
    const auto leftLeg = nameToJoint.find("LeftLeg");
    if (rightLeg == nameToJoint.end() || leftLeg == nameToJoint.end())
        throw std::invalid_argument("SOMA runtime skeleton needs RightLeg and LeftLeg");
    binding.rightLegJoint = rightLeg->second;
    binding.leftLegJoint = leftLeg->second;
    ValidateBinding(binding);
    return binding;
}

Pose ComputeWorldPose(const SkeletonBinding& binding, const Pose& local,
    const Transform& rootTransform)
{
    ValidateBinding(binding);
    ValidatePose(binding, local, "local pose");
    Pose world(local.size());
    world[0] = Compose(rootTransform, local[0]);
    for (size_t joint = 1; joint < local.size(); ++joint)
        world[joint] = Compose(world[static_cast<size_t>(binding.parentIndices[joint])], local[joint]);
    return world;
}

PoseFrames ComputeWorldPoses(const SkeletonBinding& binding, const PoseFrames& local,
    const Transform& rootTransform)
{
    PoseFrames world;
    world.reserve(local.size());
    for (const Pose& pose : local)
        world.push_back(ComputeWorldPose(binding, pose, rootTransform));
    return world;
}

EncodedInputs EncodeSomaInputs(const SkeletonBinding& binding,
    const std::array<Pose, kContextPoses>& context,
    const std::vector<Pose>& targetPoses, const EncodeConfig& config)
{
    ValidateBinding(binding);
    if (targetPoses.empty() || targetPoses.size() > kTargetCapacity)
        throw std::invalid_argument("SOMA target token requires 1 to 4 poses");
    for (const Pose& pose : context)
        ValidatePose(binding, pose, "context pose");
    for (const Pose& pose : targetPoses)
        ValidatePose(binding, pose, "target pose");
    if (config.allowedTokenCounts.empty())
        throw std::invalid_argument("SOMA allowed token counts cannot be empty");
    if (!config.headingAlignment.allFinite() || config.headingAlignment.norm() <= 1e-6f)
        throw std::invalid_argument("SOMA heading alignment must be finite and non-zero");

    EncodedInputs encoded;
    for (int count : config.allowedTokenCounts)
    {
        if (count < kMinTokenCount || count > kMaxTokenCount)
            throw std::invalid_argument("SOMA allowed token counts must be 6 to 16");
        encoded.allowedPredNumTokens[static_cast<size_t>(count - kMinTokenCount)] = 1;
    }

    PoseFrames guideLocal;
    guideLocal.reserve(kContextAndTargetPoses);
    for (const Pose& pose : context)
        guideLocal.push_back(pose);
    for (const Pose& pose : targetPoses)
        guideLocal.push_back(pose);
    while (guideLocal.size() < kContextAndTargetPoses)
        guideLocal.push_back(guideLocal.back());

    const Transform modelToRuntime = Normalized(config.modelToRuntime);
    const PoseFrames world = ComputeWorldPoses(binding, guideLocal, Inverse(modelToRuntime));
    encoded.alignment = ComputeGuideAlignment(binding, world, config.headingAlignment);
    const Eigen::Quaternionf alignmentRotation = encoded.alignment.rotation;
    const Eigen::Quaternionf inverseAlignment = alignmentRotation.conjugate();

    for (int frame = 0; frame < kContextAndTargetPoses; ++frame)
    {
        const int nextFrame = frame == kContextPoses - 1 ? frame :
            std::min(frame + 1, kContextAndTargetPoses - 1);
        const Pose& currentPose = world[static_cast<size_t>(frame)];
        const Pose& nextPose = world[static_cast<size_t>(nextFrame)];

        const Eigen::Vector3f currentHips = AlignedPosition(encoded.alignment,
            currentPose[0].position);
        const Eigen::Vector3f nextHips = AlignedPosition(encoded.alignment,
            nextPose[0].position);
        const Eigen::Vector3f currentRight = AlignedPosition(encoded.alignment,
            currentPose[static_cast<size_t>(binding.rightLegJoint)].position);
        const Eigen::Vector3f currentLeft = AlignedPosition(encoded.alignment,
            currentPose[static_cast<size_t>(binding.leftLegJoint)].position);
        const Eigen::Vector3f nextRight = AlignedPosition(encoded.alignment,
            nextPose[static_cast<size_t>(binding.rightLegJoint)].position);
        const Eigen::Vector3f nextLeft = AlignedPosition(encoded.alignment,
            nextPose[static_cast<size_t>(binding.leftLegJoint)].position);

        const float currentHeading = HeadingFromHipPositions(currentRight, currentLeft);
        const float nextHeading = HeadingFromHipPositions(nextRight, nextLeft);
        const float headingDelta = std::atan2(std::sin(nextHeading - currentHeading),
            std::cos(nextHeading - currentHeading));

        auto& globalRoot = encoded.globalRootValues[static_cast<size_t>(frame)];
        globalRoot = {currentHips.x(), currentHips.y(), currentHips.z(),
            std::cos(currentHeading), std::sin(currentHeading)};

        auto& localRoot = encoded.localRootValues[static_cast<size_t>(frame)];
        localRoot = {headingDelta * kSampleRate,
            (nextHips.x() - currentHips.x()) * kSampleRate,
            (nextHips.z() - currentHips.z()) * kSampleRate,
            currentHips.y()};

        const Eigen::Vector3f rootPosition(currentHips.x(), 0.0f, currentHips.z());
        for (int modelJoint = 0; modelJoint < kSomaJointCount; ++modelJoint)
        {
            const int poseJoint = binding.modelToPose[static_cast<size_t>(modelJoint)];
            const Transform& worldJoint = currentPose[static_cast<size_t>(poseJoint)];
            const Eigen::Vector3f jointPosition = AlignedPosition(encoded.alignment,
                worldJoint.position);
            const Eigen::Quaternionf jointRotation =
                (inverseAlignment * worldJoint.rotation).normalized();

            if (modelJoint > 0)
            {
                const int offset = (modelJoint - 1) * 3;
                const Eigen::Vector3f centered = jointPosition - rootPosition;
                encoded.localPoses[static_cast<size_t>(frame)][static_cast<size_t>(offset)] =
                    centered.x();
                encoded.localPoses[static_cast<size_t>(frame)][static_cast<size_t>(offset + 1)] =
                    centered.y();
                encoded.localPoses[static_cast<size_t>(frame)][static_cast<size_t>(offset + 2)] =
                    centered.z();
            }
            WriteRotation6D(jointRotation, encoded.localPoses[static_cast<size_t>(frame)],
                kSomaRotationOffset + modelJoint * 6);
        }

        const int32_t value = frame < kContextPoses + static_cast<int>(targetPoses.size()) ? 1 : 0;
        encoded.hasGlobalRootValues[static_cast<size_t>(frame)] = value;
        encoded.hasLocalPoses[static_cast<size_t>(frame)] = value;
        encoded.hasLocalRootValues[static_cast<size_t>(frame)] =
            (frame == kContextPoses - 1 ||
             frame == kContextPoses + static_cast<int>(targetPoses.size()) - 1) ? 0 : value;
    }

    return encoded;
}

DecodedMotion DecodeSomaOutput(const SkeletonBinding& binding,
    const Transform& alignment, const Transform& modelToRuntime,
    const std::vector<std::array<float, 366>>& prediction, int tokenCount)
{
    ValidateBinding(binding);
    const Transform normalizedAlignment = Normalized(alignment);
    const Transform normalizedModelToRuntime = Normalized(modelToRuntime);
    const int frameCount = ValidFrameCount(tokenCount);
    if (prediction.size() != 64)
        throw std::invalid_argument("SOMA prediction must contain 64 frames");
    if (frameCount + kBoundaryFrames > static_cast<int>(prediction.size()))
        throw std::invalid_argument("SOMA prediction does not hold the selected token count");

    DecodedMotion decoded;
    decoded.frames.reserve(static_cast<size_t>(frameCount));
    decoded.footContacts.reserve(static_cast<size_t>(frameCount));
    for (int frame = 0; frame < frameCount; ++frame)
    {
        const auto& features = prediction[static_cast<size_t>(kBoundaryFrames + frame)];
        for (float value : features)
            if (!std::isfinite(value))
                throw std::invalid_argument("SOMA prediction contains a non-finite feature");

        Pose pose(binding.jointNames.size());
        const Eigen::Vector3f hipsPosition(features[kPredictedRootOffset],
            features[kPredictedRootOffset + 1], features[kPredictedRootOffset + 2]);
        const Eigen::Quaternionf hipsRotation = ReadRotation6D(features,
            kPredictedRotationOffset, "predicted Hips rotation");
        const Transform hipsTransform = Compose(normalizedAlignment,
            Transform{hipsPosition, hipsRotation});
        pose[0] = Compose(normalizedModelToRuntime, hipsTransform);

        for (size_t joint = 1; joint < pose.size(); ++joint)
        {
            const int modelJoint = binding.poseToModel[joint];
            if (modelJoint < 0)
            {
                pose[joint] = binding.referenceLocalPose[joint];
                continue;
            }
            const int parent = binding.parentIndices[joint];
            const int parentModelJoint = parent >= 0 ? binding.poseToModel[static_cast<size_t>(parent)] : -1;
            if (parentModelJoint < 0)
                throw std::invalid_argument("SOMA mapped joint has an unmapped parent");

            const Eigen::Quaternionf predictedRotation = ReadRotation6D(features,
                kPredictedRotationOffset + modelJoint * 6, "predicted joint rotation");
            const Eigen::Quaternionf parentRotation = ReadRotation6D(features,
                kPredictedRotationOffset + parentModelJoint * 6, "predicted parent rotation");
            const Eigen::Quaternionf localRotation =
                (parentRotation.conjugate() * predictedRotation).normalized();
            pose[joint] = Transform{binding.referenceLocalPose[joint].position, localRotation};
        }
        decoded.frames.push_back(std::move(pose));

        std::array<float, 4> contacts{};
        for (int contact = 0; contact < 4; ++contact)
            contacts[static_cast<size_t>(contact)] = std::clamp(
                features[static_cast<size_t>(kPredictedContactOffset + contact)], 0.0f, 1.0f);
        decoded.footContacts.push_back(contacts);
    }
    return decoded;
}

EndpointDeltas ComputeEndpointDeltas(const SkeletonBinding& binding,
    const Pose& contextLast, const Pose& targetFirst, const PoseFrames& generated)
{
    ValidateBinding(binding);
    ValidatePose(binding, contextLast, "context pose");
    ValidatePose(binding, targetFirst, "target pose");
    EndpointDeltas deltas;
    if (generated.size() < 2)
        return deltas;
    for (const Pose& pose : generated)
        ValidatePose(binding, pose, "generated pose");

    const size_t jointCount = binding.jointNames.size();
    deltas.start.resize(jointCount);
    deltas.end.resize(jointCount);
    deltas.start[0] = ComputeRootEndpointDelta(generated.front()[0], contextLast[0]);
    deltas.end[0] = ComputeRootEndpointDelta(generated.back()[0], targetFirst[0]);
    for (size_t joint = 1; joint < jointCount; ++joint)
    {
        deltas.start[joint] = Compose(Inverse(generated.front()[joint]), contextLast[joint]);
        deltas.end[joint] = Compose(Inverse(generated.back()[joint]), targetFirst[joint]);
    }
    for (size_t joint = 0; joint < jointCount; ++joint)
    {
        deltas.start[joint] = Normalized(deltas.start[joint]);
        deltas.end[joint] = Normalized(deltas.end[joint]);
    }
    return deltas;
}

void ApplyEndpointCorrection(const SkeletonBinding& binding, PoseFrames& generated,
    const EndpointDeltas& deltas)
{
    ValidateBinding(binding);
    if (generated.size() < 2)
        return;
    const size_t jointCount = binding.jointNames.size();
    if (deltas.start.size() != jointCount || deltas.end.size() != jointCount)
        throw std::invalid_argument("SOMA endpoint deltas do not match the skeleton");
    const float denominator = static_cast<float>(generated.size() - 1);
    for (size_t frame = 0; frame < generated.size(); ++frame)
    {
        const float endWeight = static_cast<float>(frame) / denominator;
        const float startWeight = 1.0f - endWeight;
        Pose& pose = generated[frame];
        pose[0] = ApplyRootCorrection(pose[0], deltas.start[0], deltas.end[0], endWeight);
        for (size_t joint = 1; joint < jointCount; ++joint)
        {
            pose[joint] = ApplyDelta(pose[joint], deltas.start[joint], startWeight);
            pose[joint] = ApplyDelta(pose[joint], deltas.end[joint], endWeight);
        }
    }
}

void CorrectEndpoints(const SkeletonBinding& binding, const Pose& contextLast,
    const Pose& targetFirst, PoseFrames& generated)
{
    const EndpointDeltas deltas = ComputeEndpointDeltas(binding, contextLast,
        targetFirst, generated);
    ApplyEndpointCorrection(binding, generated, deltas);
}

Pose SamplePose(const PoseFrames& frames, float time, float sampleRate,
    const Transform& rootOffset)
{
    if (frames.empty())
        throw std::invalid_argument("Cannot sample an empty MotionBricks animation");
    const size_t jointCount = frames.front().size();
    for (const Pose& pose : frames)
        if (pose.size() != jointCount)
            throw std::invalid_argument("MotionBricks animation frames have inconsistent joint counts");
    const FrameSample sample = SampleFrames(time, sampleRate, frames.size());
    const Pose& first = frames[static_cast<size_t>(sample.frame)];
    const Pose& second = frames[static_cast<size_t>(sample.next)];
    Pose output(jointCount);
    for (size_t joint = 0; joint < jointCount; ++joint)
        output[joint] = Blend(first[joint], second[joint], sample.blend);
    output[0] = Compose(Normalized(rootOffset), output[0]);
    return output;
}

std::array<float, 4> SampleFootContacts(
    const std::vector<std::array<float, 4>>& contacts, float time, float sampleRate)
{
    if (contacts.empty())
        throw std::invalid_argument("Cannot sample empty MotionBricks foot contacts");
    const FrameSample sample = SampleFrames(time, sampleRate, contacts.size());
    std::array<float, 4> output{};
    for (int contact = 0; contact < 4; ++contact)
    {
        const float from = contacts[static_cast<size_t>(sample.frame)][static_cast<size_t>(contact)];
        const float to = contacts[static_cast<size_t>(sample.next)][static_cast<size_t>(contact)];
        output[static_cast<size_t>(contact)] = from + (to - from) * sample.blend;
    }
    return output;
}
}
