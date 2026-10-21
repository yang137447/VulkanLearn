#include "motionFeatures.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace VL::Animation
{
namespace
{
float Number(const nlohmann::json& value, const char* key, float minimum, float maximum)
{
    const auto& field = value.at(key);
    if (!field.is_number()) throw std::invalid_argument("Motion schema requires number");
    const float result = field.get<float>();
    if (!std::isfinite(result) || result < minimum || result > maximum)
        throw std::invalid_argument("Motion schema number outside budget: " + std::string(key));
    return result;
}

Eigen::Matrix4f RigidPower(Eigen::Matrix4f matrix, int power)
{
    if (power < 0) { matrix = matrix.inverse().eval(); power = -power; }
    Eigen::Matrix4f result = Eigen::Matrix4f::Identity();
    while (power)
    {
        if (power & 1) result = (result * matrix).eval();
        matrix = (matrix * matrix).eval();
        power >>= 1;
    }
    return result;
}
}

FeatureSchema ParseFeatureSchema(const nlohmann::json& json)
{
    if (!json.is_object() || json.at("type") != "motionFeatureSchema" ||
        json.at("version") != 1 || json.at("space") != "root-heading-cm-yup-zforward" ||
        json.at("poseStage") != "post-blend-pre-ik" ||
        json.at("normalization") != "population-std-zero-to-one-v1")
        throw std::invalid_argument("Unsupported motion schema policy/version");
    FeatureSchema schema;
    schema.sampleRate = Number(json, "sampleRate", 1, 120);
    schema.velocityStep = Number(json, "velocityStep", 0.001f, 0.1f);
    const auto& times = json.at("trajectoryTimes");
    if (!times.is_array() || times.empty() || times.size() > 16)
        throw std::invalid_argument("Motion schema trajectory budget");
    schema.trajectoryTimes.clear();
    for (const auto& value : times)
    {
        if (!value.is_number()) throw std::invalid_argument("Motion offset must be numeric");
        const float time = value.get<float>();
        if (!std::isfinite(time) || time < -1 || time > 1 ||
            (!schema.trajectoryTimes.empty() && time <= schema.trajectoryTimes.back()))
            throw std::invalid_argument("Motion offsets must be ordered, unique and within one second");
        schema.trajectoryTimes.push_back(time);
    }
    const auto& joints = json.at("joints");
    if (!joints.is_array() || joints.empty() || joints.size() > 16)
        throw std::invalid_argument("Motion schema joint budget");
    std::set<std::string> names;
    for (const auto& joint : joints)
    {
        if (!joint.is_string() || joint.get<std::string>().empty() ||
            !names.insert(joint.get<std::string>()).second)
            throw std::invalid_argument("Motion schema requires unique named joints");
        schema.joints.push_back(joint.get<std::string>());
    }
    const auto& weights = json.at("weights");
    schema.trajectoryPositionWeight = Number(weights, "trajectoryPosition", 0.000001f, 1000);
    schema.trajectoryFacingWeight = Number(weights, "trajectoryFacing", 0.000001f, 1000);
    schema.posePositionWeight = Number(weights, "posePosition", 0.000001f, 1000);
    schema.poseVelocityWeight = Number(weights, "poseVelocity", 0.000001f, 1000);
    return schema;
}

nlohmann::json FeatureSchemaJson(const FeatureSchema& schema)
{
    return {{"type", "motionFeatureSchema"}, {"version", schema.version},
        {"space", "root-heading-cm-yup-zforward"}, {"poseStage", "post-blend-pre-ik"},
        {"normalization", "population-std-zero-to-one-v1"}, {"sampleRate", schema.sampleRate},
        {"velocityStep", schema.velocityStep}, {"trajectoryTimes", schema.trajectoryTimes},
        {"joints", schema.joints}, {"weights", {
            {"trajectoryPosition", schema.trajectoryPositionWeight}, {"trajectoryFacing", schema.trajectoryFacingWeight},
            {"posePosition", schema.posePositionWeight}, {"poseVelocity", schema.poseVelocityWeight}}}};
}

FeatureSchema PreviewFeatureSchema(const SkeletonMesh& mesh)
{
    FeatureSchema schema;
    for (const char* name : {"pelvis", "foot_l", "foot_r"})
        for (const auto& joint : mesh.joints)
            if (joint.name == name) schema.joints.push_back(joint.name);
    if (schema.joints.empty()) schema.joints.push_back(mesh.joints.at(0).name);
    return schema;
}

std::vector<size_t> ResolveFeatureJoints(const SkeletonMesh& mesh, const FeatureSchema& schema)
{
    std::vector<size_t> result;
    for (const auto& name : schema.joints)
    {
        size_t index = 0;
        while (index < mesh.joints.size() && mesh.joints[index].name != name) ++index;
        if (index == mesh.joints.size()) throw std::invalid_argument("Missing feature joint: " + name);
        result.push_back(index);
    }
    return result;
}

std::vector<std::string> FeatureChannelNames(const FeatureSchema& schema)
{
    std::vector<std::string> result;
    for (const auto time : schema.trajectoryTimes)
        for (const char* field : {"position.x", "position.z", "facing.x", "facing.z"})
            result.push_back("trajectory[" + std::to_string(time) + "]." + field);
    for (const auto& joint : schema.joints)
        for (const char* field : {"position.x", "position.y", "position.z", "velocity.x", "velocity.y", "velocity.z"})
            result.push_back(joint + "." + field);
    return result;
}

std::vector<float> FeatureChannelWeights(const FeatureSchema& schema)
{
    std::vector<float> result;
    for (size_t i = 0; i < schema.trajectoryTimes.size(); ++i)
        for (float weight : {schema.trajectoryPositionWeight, schema.trajectoryPositionWeight,
            schema.trajectoryFacingWeight, schema.trajectoryFacingWeight}) result.push_back(weight);
    for (size_t i = 0; i < schema.joints.size(); ++i)
        for (float weight : {schema.posePositionWeight, schema.posePositionWeight, schema.posePositionWeight,
            schema.poseVelocityWeight, schema.poseVelocityWeight, schema.poseVelocityWeight}) result.push_back(weight);
    return result;
}

Eigen::Matrix4f HeadingFrame(const Eigen::Matrix4f& root)
{
    const auto forward = root.block<3, 1>(0, 2);
    if (std::hypot(forward.x(), forward.z()) < 1e-6f)
        throw std::invalid_argument("Motion root requires a planar heading");
    const float yaw = std::atan2(forward.x(), forward.z());
    Eigen::Matrix4f result = Eigen::Matrix4f::Identity();
    result.block<3, 3>(0, 0) = Eigen::AngleAxisf(yaw, Eigen::Vector3f::UnitY()).toRotationMatrix();
    result.block<3, 1>(0, 3) = root.block<3, 1>(0, 3);
    return result;
}

std::vector<Eigen::Matrix4f> SampleMotionPose(
    const SkeletonMesh& mesh, const Clip& clip, double time, bool loop)
{
    if (!std::isfinite(time) || std::abs(time) > 602)
        throw std::invalid_argument("Motion time outside supported range");
    const double duration = clip.runtime->duration();
    if (!loop) return SampleModels(mesh, clip, {static_cast<float>(time)}).front();
    const double cycleCount = std::floor(time / duration);
    if (!std::isfinite(cycleCount) || std::abs(cycleCount) > 1000000)
        throw std::invalid_argument("Motion loop cycle budget exceeded");
    const int cycles = static_cast<int>(cycleCount);
    const float local = static_cast<float>(time - cycles * duration);
    auto models = SampleModels(mesh, clip, {local}).front();
    if (cycles)
    {
        const auto ends = SampleModels(mesh, clip, {0, static_cast<float>(duration)});
        // 完整 SE(3) 周期累积：转身循环的前进方向不能按线性位置平移延拓。
        const Eigen::Matrix4f delta = ends.back().front() * ends.front().front().inverse();
        if ((delta.block<3, 3>(0, 0).transpose() * delta.block<3, 3>(0, 0) -
            Eigen::Matrix3f::Identity()).norm() > 0.001f)
            throw std::invalid_argument("Loop root requires rigid cycle transform");
        const Eigen::Matrix4f extension = RigidPower(delta, cycles);
        for (auto& model : models) model = (extension * model).eval();
    }
    return models;
}

bool CanExtractClipFeatures(const Clip& clip, const FeatureSchema& schema, float time, bool loop,
    float playbackRate)
{
    if (!std::isfinite(time) || time < 0 || time > clip.runtime->duration() ||
        !std::isfinite(playbackRate) || playbackRate <= 0 || playbackRate > 4) return false;
    if (loop) return true;
    const float lower = std::min(-schema.velocityStep, schema.trajectoryTimes.front()) * playbackRate;
    const float upper = std::max(0.0f, schema.trajectoryTimes.back()) * playbackRate;
    return time + lower >= 0 && time + upper <= clip.runtime->duration();
}

std::vector<float> EncodeMotionFeatures(const FeatureSchema& schema,
    const Eigen::Matrix4f& root, const std::vector<Eigen::Matrix4f>& trajectory,
    const std::vector<Eigen::Vector3f>& positions, const std::vector<Eigen::Vector3f>& velocities)
{
    if (trajectory.size() != schema.trajectoryTimes.size() ||
        positions.size() != schema.joints.size() || velocities.size() != positions.size())
        throw std::invalid_argument("Motion feature channel shape mismatch");
    const Eigen::Matrix4f frame = HeadingFrame(root);
    const Eigen::Matrix3f inverseRotation = frame.block<3, 3>(0, 0).transpose();
    const Eigen::Vector3f origin = frame.block<3, 1>(0, 3);
    std::vector<float> result;
    for (const auto& sample : trajectory)
    {
        const Eigen::Vector3f point = inverseRotation * (sample.block<3, 1>(0, 3) - origin);
        const Eigen::Vector3f facing = inverseRotation * HeadingFrame(sample).block<3, 1>(0, 2);
        for (float value : {point.x(), point.z(), facing.x(), facing.z()}) result.push_back(value);
    }
    for (size_t i = 0; i < positions.size(); ++i)
    {
        const Eigen::Vector3f position = inverseRotation * (positions[i] - origin);
        const Eigen::Vector3f velocity = inverseRotation * velocities[i];
        for (float value : {position.x(), position.y(), position.z(), velocity.x(), velocity.y(), velocity.z()})
            result.push_back(value);
    }
    for (float value : result)
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite motion feature");
    return result;
}

std::vector<float> ExtractClipFeatures(const SkeletonMesh& mesh, const Clip& clip,
    const FeatureSchema& schema, float time, bool loop, float playbackRate)
{
    if (!CanExtractClipFeatures(clip, schema, time, loop, playbackRate))
        throw std::invalid_argument("Clip features require a valid rate and complete time window");
    const auto joints = ResolveFeatureJoints(mesh, schema);
    const auto current = SampleMotionPose(mesh, clip, time, loop);
    // offset 属于运行时秒，采样时刻才乘倍率；差分仍除运行时步长，保留真实播放速度。
    // 数据库构建沿用默认倍率 1，只有准确继续候选使用当前播放器的重参数化时间。
    const auto before = SampleMotionPose(mesh, clip, double(time) - schema.velocityStep * playbackRate, loop);
    std::vector<Eigen::Matrix4f> trajectory;
    for (float offset : schema.trajectoryTimes)
        trajectory.push_back(SampleMotionPose(mesh, clip, double(time) + offset * playbackRate, loop).front());
    std::vector<Eigen::Vector3f> positions, velocities;
    for (size_t joint : joints)
    {
        positions.push_back(current[joint].block<3, 1>(0, 3));
        velocities.push_back((current[joint].block<3, 1>(0, 3) - before[joint].block<3, 1>(0, 3)) /
            schema.velocityStep);
    }
    return EncodeMotionFeatures(schema, current.front(), trajectory, positions, velocities);
}
}
