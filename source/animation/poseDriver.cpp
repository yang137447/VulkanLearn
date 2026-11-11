#include "poseDriver.h"

#include "shader/build/contentHash.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace VL::Animation
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
constexpr float DegreesToRadians = Pi / 180.0f;
constexpr float RadiansToDegrees = 180.0f / Pi;
// UE KINDA_SMALL_NUMBER；RBF 的阈值、半径下限与奇异判定都用它。
constexpr float KindaSmallNumber = 1.0e-4f;
constexpr float SingularityThreshold = 0.4999995f;

float ReadLane(ozz::math::SimdFloat4 vector, size_t lane)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    return values[lane];
}

Eigen::Vector3f ReadTranslation(const LocalPose& locals, size_t joint)
{
    const auto& value = locals[joint / 4].translation;
    const size_t lane = joint % 4;
    return {ReadLane(value.x, lane), ReadLane(value.y, lane), ReadLane(value.z, lane)};
}

Eigen::Quaternionf ReadRotation(const LocalPose& locals, size_t joint)
{
    const auto& value = locals[joint / 4].rotation;
    const size_t lane = joint % 4;
    return Eigen::Quaternionf(ReadLane(value.w, lane), ReadLane(value.x, lane),
        ReadLane(value.y, lane), ReadLane(value.z, lane)).normalized();
}

// UE FRotator::NormalizeAxis：归一到 (-180, 180]。
float NormalizeAxis(float angle)
{
    if (angle > 180.0f || angle < -180.0f)
    {
        angle = std::fmod(angle + 180.0f, 360.0f);
        if (angle < 0.0f) angle += 360.0f;
        angle -= 180.0f;
    }
    return angle;
}

// VL = (UE.X, UE.Z, -UE.Y)，对应绕 X 轴 -90° 的正交基。
// 本文件所有姿态/目标都在 UE 空间比较，因此 VL 姿态先做共轭：
//   q_ue = q_basis^-1 * q_vl * q_basis，t_ue = (x, -z, y)
// 位置映射的逆变换是 (x, -z, y)，不是简单的 y/z 交换；写错会整体偏 90°。
const Eigen::Quaternionf& BasisQuaternion()
{
    // Eigen 构造顺序是 (w, x, y, z)：这是绕 X 轴 -90°。
    static const Eigen::Quaternionf value(0.70710678118654752440f,
        -0.70710678118654752440f, 0.0f, 0.0f);
    return value;
}

Eigen::Vector3f ToUeTranslation(const Eigen::Vector3f& value)
{
    return {value.x(), -value.z(), value.y()};
}

Eigen::Quaternionf ToUeRotation(const Eigen::Quaternionf& value)
{
    const Eigen::Quaternionf& basis = BasisQuaternion();
    return (basis.conjugate() * value * basis).normalized();
}

// UE FRotator::Quaternion()（标量路径，度，先取 mod 360）。
Eigen::Quaternionf RotatorToQuaternion(const Eigen::Vector3f& rotatorDegrees)
{
    const float pitch = std::fmod(rotatorDegrees.x(), 360.0f) * DegreesToRadians * 0.5f;
    const float yaw = std::fmod(rotatorDegrees.y(), 360.0f) * DegreesToRadians * 0.5f;
    const float roll = std::fmod(rotatorDegrees.z(), 360.0f) * DegreesToRadians * 0.5f;
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float sr = std::sin(roll), cr = std::cos(roll);
    Eigen::Quaternionf result;
    result.x() = cr * sp * sy - sr * cp * cy;
    result.y() = -cr * sp * cy - sr * cp * sy;
    result.z() = cr * cp * sy - sr * sp * cy;
    result.w() = cr * cp * cy + sr * sp * sy;
    return result.normalized();
}

// UE FQuat::Rotator()；SingularityTest = Z*X - W*Y。
Eigen::Vector3f QuaternionToRotator(const Eigen::Quaternionf& q)
{
    const float x = q.x(), y = q.y(), z = q.z(), w = q.w();
    const float singularity = z * x - w * y;
    if (singularity < -SingularityThreshold)
        return {-90.0f, NormalizeAxis(-2.0f * std::atan2(x, w) * RadiansToDegrees), 0.0f};
    if (singularity > SingularityThreshold)
        return {90.0f, NormalizeAxis(2.0f * std::atan2(x, w) * RadiansToDegrees), 0.0f};
    const float pitch = std::asin(std::clamp(2.0f * singularity, -1.0f, 1.0f)) * RadiansToDegrees;
    const float yaw = std::atan2(2.0f * (w * z + x * y),
        1.0f - 2.0f * (y * y + z * z)) * RadiansToDegrees;
    const float roll = std::atan2(-2.0f * (w * x + y * z),
        1.0f - 2.0f * (x * x + y * y)) * RadiansToDegrees;
    return {pitch, yaw, roll};
}

// UE FQuat::AngularDistance：acos(2*dot^2 - 1)，即 2*acos(|dot|)。
float AngularDistance(const Eigen::Quaternionf& a, const Eigen::Quaternionf& b)
{
    const float dot = a.x() * b.x() + a.y() * b.y() + a.z() * b.z() + a.w() * b.w();
    return std::acos(std::clamp(2.0f * dot * dot - 1.0f, -1.0f, 1.0f));
}

// UE FQuat::ToSwingTwist：先把向量部分投影到 twist 轴，归一化后求出 twist，
// 再 OutSwing = *this * OutTwist^-1。这里只返回 swing。
Eigen::Quaternionf SwingPart(const Eigen::Quaternionf& q, const Eigen::Vector3f& axis)
{
    Eigen::Quaternionf swing, twist;
    ToSwingTwist(q, axis, swing, twist);
    return swing;
}

float TwistAngle(const Eigen::Quaternionf& q, const Eigen::Vector3f& axis)
{
    const float xyz = axis.dot(Eigen::Vector3f(q.x(), q.y(), q.z()));
    return 2.0f * std::atan2(xyz, q.w());
}

Eigen::Vector3f TwistAxisVector(PoseDriverAxis axis)
{
    switch (axis)
    {
    case PoseDriverAxis::Y: return {0.0f, 1.0f, 0.0f};
    case PoseDriverAxis::Z: return {0.0f, 0.0f, 1.0f};
    case PoseDriverAxis::X:
    default: return {1.0f, 0.0f, 0.0f};
    }
}

// FRBFEntry 的 3 分量布局固定为 (Roll, Pitch, Yaw)。
void AddRotator(std::vector<float>& values, const Eigen::Vector3f& rotatorDegrees)
{
    values.push_back(rotatorDegrees.z());
    values.push_back(rotatorDegrees.x());
    values.push_back(rotatorDegrees.y());
}

void AddVector(std::vector<float>& values, const Eigen::Vector3f& vector)
{
    values.push_back(vector.x());
    values.push_back(vector.y());
    values.push_back(vector.z());
}

Eigen::Vector3f EntryRotator(const std::vector<float>& values, size_t index)
{
    const size_t base = index * 3;
    return {values[base + 1], values[base + 2], values[base]};
}

Eigen::Quaternionf EntryQuat(const std::vector<float>& values, size_t index)
{
    return RotatorToQuaternion(EntryRotator(values, index));
}

float EntryDistance(const std::vector<float>& a, const std::vector<float>& b,
    RBFDistanceMethod method, const Eigen::Vector3f& twistAxis)
{
    if (a.size() != b.size() || a.size() % 3 != 0)
        return 0.0f;
    const size_t rotations = a.size() / 3;
    float total = 0.0f;
    for (size_t index = 0; index < rotations; ++index)
    {
        float distance = 0.0f;
        switch (method)
        {
        case RBFDistanceMethod::Euclidean:
        {
            // RBFDistanceMetric::Euclidean(FRotator)：Euler 三分量先转弧度再求 L2。
            const Eigen::Vector3f left = EntryRotator(a, index) * DegreesToRadians;
            const Eigen::Vector3f right = EntryRotator(b, index) * DegreesToRadians;
            distance = (left - right).norm();
            break;
        }
        case RBFDistanceMethod::Quaternion:
            distance = AngularDistance(EntryQuat(a, index), EntryQuat(b, index));
            break;
        case RBFDistanceMethod::SwingAngle:
        case RBFDistanceMethod::Default:
            distance = AngularDistance(SwingPart(EntryQuat(a, index), twistAxis),
                SwingPart(EntryQuat(b, index), twistAxis));
            break;
        case RBFDistanceMethod::TwistAngle:
            distance = std::abs(TwistAngle(EntryQuat(a, index), twistAxis) -
                TwistAngle(EntryQuat(b, index), twistAxis));
            break;
        }
        total += distance * distance;
    }
    return std::sqrt(total);
}

float DistanceBetween(const std::vector<float>& a, const std::vector<float>& b,
    const RBFParams& params, RBFDistanceMethod overrideMethod)
{
    const RBFDistanceMethod method = overrideMethod == RBFDistanceMethod::Default ?
        params.distanceMethod : overrideMethod;
    return EntryDistance(a, b, method, TwistAxisVector(params.twistAxis)) * RadiansToDegrees;
}

float WeightedValue(float value, float kernelWidth, RBFFunctionType type, bool backCompFix)
{
    if (value < 0.0f) return 0.0f;
    switch (type)
    {
    case RBFFunctionType::Gaussian:
        return backCompFix ? std::exp(-value * value) :
            std::exp(-value * (1.0f / kernelWidth) * (1.0f / kernelWidth));
    case RBFFunctionType::Exponential:
        return backCompFix ? 1.0f / std::exp(value) :
            std::exp(-2.0f * value / kernelWidth);
    case RBFFunctionType::Cubic:
    {
        const float scaled = value / kernelWidth;
        return std::max(1.0f - scaled * scaled * scaled, 0.0f);
    }
    case RBFFunctionType::Quintic:
    {
        const float scaled = value / kernelWidth;
        return std::max(1.0f - std::pow(scaled, 5.0f), 0.0f);
    }
    case RBFFunctionType::Linear:
    case RBFFunctionType::Default:
    default:
        return backCompFix ? std::max(1.0f - value, 0.0f) :
            (kernelWidth - std::clamp(value, 0.0f, kernelWidth)) / kernelWidth;
    }
}

bool NearlyZero(float value)
{
    return std::abs(value) <= KindaSmallNumber;
}

// FRBFSolver::GetRadiusForTarget：只有 Additive 会把目标缩放乘进半径。
float GetRadius(float targetScale, const RBFParams& params)
{
    float radius = params.radius;
    if (params.solverType == RBFSolverType::Additive)
        radius *= targetScale;
    return std::max(radius, KindaSmallNumber);
}

Eigen::Matrix3f NormalizedRotation(const Eigen::Matrix4f& matrix)
{
    // restLocal 可能带缩放；Quaternion(Matrix3) 假定正交，先逐列归一化。
    Eigen::Matrix3f rotation = matrix.block<3, 3>(0, 0);
    for (int column = 0; column < 3; ++column)
    {
        const Eigen::Vector3f axis = rotation.col(column);
        const float length = axis.norm();
        rotation.col(column) = length > KindaSmallNumber ? Eigen::Vector3f(axis / length) : Eigen::Vector3f::Unit(column);
    }
    return rotation;
}

struct CompiledTarget
{
    std::vector<float> values;
    float scale = 1.0f;
    RBFDistanceMethod distanceMethod = RBFDistanceMethod::Default;
    RBFFunctionType function = RBFFunctionType::Default;
    std::string drivenName;
};

struct CompiledSourceBone
{
    // -1 表示网格缺骨：UE 会用 identity 参与 RBF，并在诊断里报告。
    int joint = -1;
    Eigen::Vector3f refTranslation = Eigen::Vector3f::Zero();
    Eigen::Quaternionf refRotation = Eigen::Quaternionf::Identity();
};
}

// 编译结果放在头文件的不透明指针后面，避免把 Eigen 矩阵尺寸暴露给调用方。
struct PoseDriverCompiled
{
    const PoseDriverDefinition* definition = nullptr;
    std::vector<CompiledSourceBone> sources;
    int evalSpaceJoint = -1;
    std::vector<CompiledTarget> targets;
    Eigen::MatrixXf coefficients;
    bool interpolativeValid = true;
    float kernelWidth = 1.0f;
};

namespace
{
void EnsureFinite(const Eigen::Vector3f& value, const char* field)
{
    if (!value.allFinite())
        throw std::invalid_argument(std::string("PoseDriver field is not finite: ") + field);
}

const nlohmann::json& Required(const nlohmann::json& object, const char* key, const char* owner)
{
    if (!object.is_object() || !object.contains(key))
        throw std::invalid_argument(std::string("PoseDriver missing field ") + owner + "." + key);
    return object.at(key);
}

void RejectUnknown(const nlohmann::json& object, std::initializer_list<const char*> allowed,
    const char* owner)
{
    if (!object.is_object())
        throw std::invalid_argument(std::string("PoseDriver object expected at ") + owner);
    for (const auto& item : object.items())
    {
        bool known = false;
        for (const char* key : allowed)
            if (item.key() == key) { known = true; break; }
        if (!known)
            throw std::invalid_argument(std::string("PoseDriver unknown field ") + owner + "." +
                item.key());
    }
}

std::string ReadString(const nlohmann::json& value, const char* field)
{
    if (!value.is_string())
        throw std::invalid_argument(std::string("PoseDriver expects string: ") + field);
    return value.get<std::string>();
}

float ReadNumber(const nlohmann::json& value, const char* field)
{
    if (!value.is_number())
        throw std::invalid_argument(std::string("PoseDriver expects number: ") + field);
    const float result = value.get<float>();
    if (!std::isfinite(result))
        throw std::invalid_argument(std::string("PoseDriver number is not finite: ") + field);
    return result;
}

bool ReadBool(const nlohmann::json& value, const char* field)
{
    if (!value.is_boolean())
        throw std::invalid_argument(std::string("PoseDriver expects bool: ") + field);
    return value.get<bool>();
}

Eigen::Vector3f ReadVector3(const nlohmann::json& value, const char* field)
{
    if (!value.is_array() || value.size() != 3)
        throw std::invalid_argument(std::string("PoseDriver expects [x,y,z]: ") + field);
    Eigen::Vector3f result;
    for (size_t index = 0; index < 3; ++index)
        result[static_cast<Eigen::Index>(index)] = ReadNumber(value.at(index), field);
    return result;
}

RBFSolverType ParseSolverType(const std::string& value)
{
    if (value == "Additive") return RBFSolverType::Additive;
    if (value == "Interpolative") return RBFSolverType::Interpolative;
    throw std::invalid_argument("PoseDriver unknown solverType: " + value);
}

RBFFunctionType ParseFunction(const std::string& value)
{
    if (value == "Gaussian") return RBFFunctionType::Gaussian;
    if (value == "Exponential") return RBFFunctionType::Exponential;
    if (value == "Linear") return RBFFunctionType::Linear;
    if (value == "Cubic") return RBFFunctionType::Cubic;
    if (value == "Quintic") return RBFFunctionType::Quintic;
    if (value == "Default") return RBFFunctionType::Default;
    throw std::invalid_argument("PoseDriver unknown function: " + value);
}

RBFDistanceMethod ParseDistanceMethod(const std::string& value)
{
    if (value == "Euclidean") return RBFDistanceMethod::Euclidean;
    if (value == "Quaternion") return RBFDistanceMethod::Quaternion;
    if (value == "SwingAngle") return RBFDistanceMethod::SwingAngle;
    if (value == "TwistAngle") return RBFDistanceMethod::TwistAngle;
    if (value == "Default") return RBFDistanceMethod::Default;
    throw std::invalid_argument("PoseDriver unknown distanceMethod: " + value);
}

RBFNormalizeMethod ParseNormalizeMethod(const std::string& value)
{
    if (value == "OnlyNormalizeAboveOne") return RBFNormalizeMethod::OnlyNormalizeAboveOne;
    if (value == "AlwaysNormalize") return RBFNormalizeMethod::AlwaysNormalize;
    if (value == "NormalizeWithinMedian") return RBFNormalizeMethod::NormalizeWithinMedian;
    if (value == "NoNormalization") return RBFNormalizeMethod::NoNormalization;
    throw std::invalid_argument("PoseDriver unknown normalizeMethod: " + value);
}

PoseDriverAxis ParseAxis(const std::string& value)
{
    if (value == "X") return PoseDriverAxis::X;
    if (value == "Y") return PoseDriverAxis::Y;
    if (value == "Z") return PoseDriverAxis::Z;
    throw std::invalid_argument("PoseDriver unknown twistAxis: " + value);
}

RBFParams ParseParams(const nlohmann::json& json)
{
    RejectUnknown(json, {"solverType", "radius", "automaticRadius", "function", "distanceMethod",
        "twistAxis", "weightThreshold", "normalizeMethod", "medianReference", "medianMin",
        "medianMax"}, "rbf");
    RBFParams params;
    params.solverType = ParseSolverType(ReadString(Required(json, "solverType", "rbf"), "rbf.solverType"));
    params.radius = ReadNumber(Required(json, "radius", "rbf"), "rbf.radius");
    params.automaticRadius = ReadBool(Required(json, "automaticRadius", "rbf"), "rbf.automaticRadius");
    params.function = ParseFunction(ReadString(Required(json, "function", "rbf"), "rbf.function"));
    params.distanceMethod = ParseDistanceMethod(
        ReadString(Required(json, "distanceMethod", "rbf"), "rbf.distanceMethod"));
    params.twistAxis = ParseAxis(ReadString(Required(json, "twistAxis", "rbf"), "rbf.twistAxis"));
    params.weightThreshold = ReadNumber(Required(json, "weightThreshold", "rbf"), "rbf.weightThreshold");
    params.normalizeMethod = ParseNormalizeMethod(
        ReadString(Required(json, "normalizeMethod", "rbf"), "rbf.normalizeMethod"));
    params.medianReference = ReadVector3(Required(json, "medianReference", "rbf"), "rbf.medianReference");
    params.medianMin = ReadNumber(Required(json, "medianMin", "rbf"), "rbf.medianMin");
    params.medianMax = ReadNumber(Required(json, "medianMax", "rbf"), "rbf.medianMax");
    if (params.radius <= 0.0f || params.weightThreshold < 0.0f ||
        params.medianMax < params.medianMin)
        throw std::invalid_argument("PoseDriver rbf parameters are out of range");
    if (params.automaticRadius && params.solverType != RBFSolverType::Interpolative)
        throw std::invalid_argument("PoseDriver automaticRadius requires the Interpolative solver");
    return params;
}

PoseDriverTarget ParseTarget(const nlohmann::json& json, size_t sourceCount, size_t index)
{
    const std::string owner = "targets[" + std::to_string(index) + "]";
    RejectUnknown(json, {"drivenName", "scale", "distanceMethod", "function", "applyCustomCurve",
        "transforms"}, owner.c_str());
    PoseDriverTarget target;
    target.drivenName = ReadString(Required(json, "drivenName", owner.c_str()), "drivenName");
    if (target.drivenName.empty())
        throw std::invalid_argument("PoseDriver target has an empty drivenName");
    target.scale = ReadNumber(Required(json, "scale", owner.c_str()), "scale");
    target.distanceMethod = ParseDistanceMethod(
        ReadString(Required(json, "distanceMethod", owner.c_str()), "distanceMethod"));
    target.function = ParseFunction(ReadString(Required(json, "function", owner.c_str()), "function"));
    target.applyCustomCurve = ReadBool(
        Required(json, "applyCustomCurve", owner.c_str()), "applyCustomCurve");
    // FRichCurve 映射尚未实现；拒绝而不是忽略，避免资产看起来生效但数值不同。
    if (target.applyCustomCurve)
        throw std::invalid_argument("PoseDriver custom curves are not supported yet");
    const auto& transforms = Required(json, "transforms", owner.c_str());
    if (!transforms.is_array() || transforms.size() != sourceCount)
        throw std::invalid_argument("PoseDriver target transforms must match sourceBones");
    for (const auto& entry : transforms)
    {
        RejectUnknown(entry, {"translation", "rotation"}, "transform");
        const Eigen::Vector3f translation = ReadVector3(
            Required(entry, "translation", "transform"), "transform.translation");
        EnsureFinite(translation, "transform.translation");
        const auto& rotation = Required(entry, "rotation", "transform");
        RejectUnknown(rotation, {"pitch", "yaw", "roll"}, "transform.rotation");
        const Eigen::Vector3f rotator(
            ReadNumber(Required(rotation, "pitch", "transform.rotation"), "pitch"),
            ReadNumber(Required(rotation, "yaw", "transform.rotation"), "yaw"),
            ReadNumber(Required(rotation, "roll", "transform.rotation"), "roll"));
        EnsureFinite(rotator, "transform.rotation");
        target.translations.push_back(translation);
        target.rotations.push_back(rotator);
    }
    return target;
}
}

const char* RBFSolverTypeName(RBFSolverType value)
{
    return value == RBFSolverType::Interpolative ? "Interpolative" : "Additive";
}

const char* RBFFunctionTypeName(RBFFunctionType value)
{
    switch (value)
    {
    case RBFFunctionType::Gaussian: return "Gaussian";
    case RBFFunctionType::Exponential: return "Exponential";
    case RBFFunctionType::Linear: return "Linear";
    case RBFFunctionType::Cubic: return "Cubic";
    case RBFFunctionType::Quintic: return "Quintic";
    case RBFFunctionType::Default:
    default: return "Default";
    }
}

const char* RBFDistanceMethodName(RBFDistanceMethod value)
{
    switch (value)
    {
    case RBFDistanceMethod::Euclidean: return "Euclidean";
    case RBFDistanceMethod::Quaternion: return "Quaternion";
    case RBFDistanceMethod::SwingAngle: return "SwingAngle";
    case RBFDistanceMethod::TwistAngle: return "TwistAngle";
    case RBFDistanceMethod::Default:
    default: return "Default";
    }
}

const char* RBFNormalizeMethodName(RBFNormalizeMethod value)
{
    switch (value)
    {
    case RBFNormalizeMethod::AlwaysNormalize: return "AlwaysNormalize";
    case RBFNormalizeMethod::NormalizeWithinMedian: return "NormalizeWithinMedian";
    case RBFNormalizeMethod::NoNormalization: return "NoNormalization";
    case RBFNormalizeMethod::OnlyNormalizeAboveOne:
    default: return "OnlyNormalizeAboveOne";
    }
}

const char* PoseDriverAxisName(PoseDriverAxis value)
{
    switch (value)
    {
    case PoseDriverAxis::Y: return "Y";
    case PoseDriverAxis::Z: return "Z";
    case PoseDriverAxis::X:
    default: return "X";
    }
}

bool TryParsePoseDriverAxis(const std::string& value, PoseDriverAxis& axis)
{
    if (value == "X") { axis = PoseDriverAxis::X; return true; }
    if (value == "Y") { axis = PoseDriverAxis::Y; return true; }
    if (value == "Z") { axis = PoseDriverAxis::Z; return true; }
    return false;
}

Eigen::Vector3f PoseDriverAxisVector(PoseDriverAxis axis)
{
    return TwistAxisVector(axis);
}

void ToSwingTwist(const Eigen::Quaternionf& value, const Eigen::Vector3f& axis,
    Eigen::Quaternionf& swing, Eigen::Quaternionf& twist)
{
    const Eigen::Vector3f vector(value.x(), value.y(), value.z());
    const Eigen::Vector3f projection = vector.dot(axis) * axis;
    // 注意 Eigen 四参数构造是 (w,x,y,z)，必须逐分量写入。
    twist.x() = projection.x();
    twist.y() = projection.y();
    twist.z() = projection.z();
    twist.w() = value.w();
    if (twist.squaredNorm() == 0.0f)
        twist = Eigen::Quaternionf::Identity();
    else
        twist.normalize();
    swing = (value * twist.conjugate()).normalized();
}

PoseDriverSet ParsePoseDriverSet(const nlohmann::json& json, std::string_view sourcePath)
{
    RejectUnknown(json, {"format", "id", "source", "drivers"}, "root");
    PoseDriverSet set;
    set.format = ReadString(Required(json, "format", "root"), "format");
    if (set.format != "vl.anim-pose-driver.v1")
        throw std::invalid_argument("PoseDriver unsupported format: " + set.format);
    set.id = ReadString(Required(json, "id", "root"), "id");
    if (set.id.empty())
        throw std::invalid_argument("PoseDriver id is empty");
    set.sourcePath = std::string(sourcePath);
    if (json.contains("source"))
    {
        const auto& source = json.at("source");
        if (source.is_object() && source.contains("t3dBlake3") && source.at("t3dBlake3").is_string())
            set.sourceDigest = source.at("t3dBlake3").get<std::string>();
    }
    const auto& drivers = Required(json, "drivers", "root");
    if (!drivers.is_array() || drivers.empty() || drivers.size() > 64)
        throw std::invalid_argument("PoseDriver drivers must be a non-empty array of at most 64");
    std::set<std::string> names;
    for (size_t index = 0; index < drivers.size(); ++index)
    {
        const auto& node = drivers.at(index);
        RejectUnknown(node, {"name", "sourceBones", "evalSpaceBone", "evalFromRefPose",
            "driveSource", "driveOutput", "rbf", "targets"}, "driver");
        PoseDriverDefinition driver;
        driver.name = ReadString(Required(node, "name", "driver"), "driver.name");
        if (driver.name.empty() || !names.insert(driver.name).second)
            throw std::invalid_argument("PoseDriver driver names must be non-empty and unique");
        const auto& bones = Required(node, "sourceBones", "driver");
        if (!bones.is_array() || bones.empty() || bones.size() > 8)
            throw std::invalid_argument("PoseDriver sourceBones must contain 1..8 bones");
        for (const auto& bone : bones)
        {
            const std::string name = ReadString(bone, "sourceBones[]");
            if (name.empty())
                throw std::invalid_argument("PoseDriver source bone name is empty");
            driver.sourceBones.push_back(name);
        }
        driver.evalSpaceBone = ReadString(Required(node, "evalSpaceBone", "driver"), "evalSpaceBone");
        driver.evalFromRefPose = ReadBool(
            Required(node, "evalFromRefPose", "driver"), "evalFromRefPose");
        if (!driver.evalSpaceBone.empty() && driver.evalFromRefPose)
            throw std::invalid_argument(
                "PoseDriver evalSpaceBone and evalFromRefPose are mutually exclusive in UE");
        const std::string driveSource = ReadString(
            Required(node, "driveSource", "driver"), "driveSource");
        if (driveSource != "Rotation" && driveSource != "Translation")
            throw std::invalid_argument("PoseDriver unsupported driveSource: " + driveSource);
        driver.translationSource = driveSource == "Translation";
        const std::string driveOutput = ReadString(
            Required(node, "driveOutput", "driver"), "driveOutput");
        if (driveOutput != "Curves")
            throw std::invalid_argument(
                "PoseDriver only supports driveOutput=Curves (DrivePoses needs UPoseAsset)");
        driver.params = ParseParams(Required(node, "rbf", "driver"));
        const auto& targets = Required(node, "targets", "driver");
        if (!targets.is_array() || targets.empty() || targets.size() > 256)
            throw std::invalid_argument("PoseDriver targets must contain 1..256 entries");
        for (size_t target = 0; target < targets.size(); ++target)
            driver.targets.push_back(ParseTarget(targets.at(target), driver.sourceBones.size(), target));
        set.drivers.push_back(std::move(driver));
    }
    return set;
}

std::shared_ptr<const PoseDriverSet> LoadPoseDriverSet(const std::filesystem::path& resourceRoot,
    const std::string& relativePath)
{
    const auto relative = std::filesystem::u8path(relativePath);
    if (relativePath.empty() || relativePath.size() > 1024 || relative.is_absolute() ||
        relative.has_root_name() || relativePath.find('\\') != std::string::npos ||
        relativePath.find(':') != std::string::npos ||
        relative.lexically_normal().generic_string() != relativePath ||
        relative.extension() != ".json" ||
        (relativePath.rfind("Maps/", 0) != 0 && relativePath.rfind("Common/", 0) != 0))
        throw std::invalid_argument("Pose driver requires a formal resource path");
    const auto canonicalRoot = std::filesystem::canonical(resourceRoot);
    const auto resolved = std::filesystem::weakly_canonical(canonicalRoot / relative);
    const auto inside = resolved.lexically_relative(canonicalRoot);
    if (inside.empty() || *inside.begin() == "..")
        throw std::invalid_argument("Pose driver escapes the resource root");
    if (!std::filesystem::exists(resolved))
        throw std::runtime_error("Pose driver asset does not exist: " + relativePath);
    if (std::filesystem::file_size(resolved) > 4 * 1024 * 1024)
        throw std::invalid_argument("Pose driver asset exceeds 4 MiB");
    std::ifstream input(resolved, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (!input || bytes.size() > 4 * 1024 * 1024)
        throw std::runtime_error("Cannot read pose driver asset: " + relativePath);
    auto set = std::make_shared<PoseDriverSet>(ParsePoseDriverSet(nlohmann::json::parse(bytes),
        relativePath));
    set->sourceDigest = ContentHasher::HashString(bytes).ToHex();
    return set;
}

PoseDriverSolver::PoseDriverSolver(const SkeletonMesh& mesh, std::shared_ptr<const PoseDriverSet> set)
    : asset(std::move(set)), mesh(&mesh)
{
    if (!asset)
        throw std::invalid_argument("PoseDriver set is null");
    for (const auto& definition : asset->drivers)
    {
        auto compiled = std::make_unique<PoseDriverCompiled>();
        compiled->definition = &definition;
        for (const auto& name : definition.sourceBones)
        {
            CompiledSourceBone source;
            for (size_t joint = 0; joint < mesh.joints.size(); ++joint)
            {
                if (mesh.joints[joint].name != name) continue;
                source.joint = static_cast<int>(joint);
                const auto& rest = mesh.joints[joint].restLocal;
                source.refTranslation = ToUeTranslation(rest.block<3, 1>(0, 3));
                source.refRotation = ToUeRotation(
                    Eigen::Quaternionf(NormalizedRotation(rest)).normalized());
                break;
            }
            if (source.joint < 0)
            {
                bindDiagnostics.push_back({definition.name, "missing-source-bone", name});
            }
            compiled->sources.push_back(source);
        }
        if (!definition.evalSpaceBone.empty())
        {
            for (size_t joint = 0; joint < mesh.joints.size(); ++joint)
                if (mesh.joints[joint].name == definition.evalSpaceBone)
                {
                    compiled->evalSpaceJoint = static_cast<int>(joint);
                    break;
                }
            if (compiled->evalSpaceJoint < 0)
            {
                bindDiagnostics.push_back({definition.name, "missing-eval-space-bone",
                    definition.evalSpaceBone});
            }
        }

        // 目标数据一次编译成 UE 空间 RBF 分量；bEvalFromRefPose 时按 UE 语义
        // 用参考姿态的逆变换把目标旋转/平移拉回参考局部空间。
        for (const auto& target : definition.targets)
        {
            CompiledTarget compiledTarget;
            compiledTarget.scale = target.scale;
            compiledTarget.distanceMethod = target.distanceMethod;
            compiledTarget.function = target.function;
            compiledTarget.drivenName = target.drivenName;
            for (size_t bone = 0; bone < definition.sourceBones.size(); ++bone)
            {
                if (definition.translationSource)
                {
                    Eigen::Vector3f value = target.translations[bone];
                    if (definition.evalFromRefPose && compiled->sources[bone].joint >= 0)
                        value = compiled->sources[bone].refRotation.conjugate() *
                            (value - compiled->sources[bone].refTranslation);
                    AddVector(compiledTarget.values, value);
                }
                else
                {
                    Eigen::Quaternionf rotation = RotatorToQuaternion(target.rotations[bone]);
                    if (definition.evalFromRefPose && compiled->sources[bone].joint >= 0)
                        rotation = (compiled->sources[bone].refRotation.conjugate() * rotation).normalized();
                    AddRotator(compiledTarget.values, QuaternionToRotator(rotation));
                }
            }
            compiled->targets.push_back(std::move(compiledTarget));
        }

        // Interpolative 只需在绑定时解一次核矩阵逆；Additive 不需要系数。
        if (definition.params.solverType == RBFSolverType::Interpolative)
        {
            const size_t count = compiled->targets.size();
            const float radius = definition.params.radius * DegreesToRadians;
            compiled->kernelWidth = radius;
            if (definition.params.automaticRadius && count > 1)
            {
                float sum = 0.0f;
                int pairs = 0;
                for (size_t a = 0; a < count; ++a)
                    for (size_t b = 0; b < count; ++b)
                        if (a != b)
                        {
                            sum += EntryDistance(compiled->targets[a].values,
                                compiled->targets[b].values, definition.params.distanceMethod,
                                TwistAxisVector(definition.params.twistAxis));
                            ++pairs;
                        }
                compiled->kernelWidth = pairs > 0 ? sum / static_cast<float>(pairs) : radius;
            }
            Eigen::MatrixXf kernel(static_cast<Eigen::Index>(count), static_cast<Eigen::Index>(count));
            for (size_t a = 0; a < count; ++a)
                for (size_t b = 0; b < count; ++b)
                {
                    const float distance = EntryDistance(compiled->targets[a].values,
                        compiled->targets[b].values, definition.params.distanceMethod,
                        TwistAxisVector(definition.params.twistAxis));
                    kernel(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) =
                        WeightedValue(distance, compiled->kernelWidth, definition.params.function, false);
                }
            // 与 UE 一致：行列式接近 0 时不求解，全部权重保持 0。
            if (NearlyZero(kernel.determinant()))
            {
                compiled->interpolativeValid = false;
                bindDiagnostics.push_back({definition.name, "singular-kernel",
                    "RBF kernel matrix is not invertible"});
            }
            else
                compiled->coefficients = kernel.inverse();
        }
        drivers.push_back(std::move(compiled));
    }
}

PoseDriverSolver::PoseDriverSolver(PoseDriverSolver&&) noexcept = default;
PoseDriverSolver& PoseDriverSolver::operator=(PoseDriverSolver&&) noexcept = default;
PoseDriverSolver::~PoseDriverSolver() = default;

PoseDriverFrame PoseDriverSolver::Solve(const LocalPose& locals) const
{
    PoseDriverFrame frame;
    if (!asset || !mesh || drivers.empty() || locals.empty())
        return frame;
    const size_t jointCount = static_cast<size_t>(locals.size()) * 4;
    const size_t meshJoints = mesh->joints.size();

    // EvalSpaceBone 需要组件空间；只在确有驱动使用它时才构建整骨架正运动学。
    std::vector<Eigen::Matrix4f> component;
    const bool needComponent = std::any_of(drivers.begin(), drivers.end(),
        [](const std::unique_ptr<PoseDriverCompiled>& driver)
        { return driver->evalSpaceJoint >= 0; });
    if (needComponent)
    {
        component.assign(std::min(meshJoints, jointCount), Eigen::Matrix4f::Identity());
        for (size_t joint = 0; joint < component.size(); ++joint)
        {
            Eigen::Matrix4f local = Eigen::Matrix4f::Identity();
            local.block<3, 3>(0, 0) = ReadRotation(locals, joint).toRotationMatrix();
            local.block<3, 1>(0, 3) = ReadTranslation(locals, joint);
            const int parent = mesh->joints[joint].parent;
            component[joint] = parent >= 0 && static_cast<size_t>(parent) < joint ?
                component[static_cast<size_t>(parent)] * local : local;
        }
    }

    for (const auto& driver : drivers)
    {
        const auto& definition = *driver->definition;
        // UE：缺骨位保持 identity 继续参与 RBF，只有当所有源骨都无效时才整体跳过。
        bool anyValid = false;
        std::vector<float> input;
        input.reserve(driver->sources.size() * 3);
        for (const auto& source : driver->sources)
        {
            Eigen::Vector3f translation = Eigen::Vector3f::Zero();
            Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
            if (source.joint >= 0 && static_cast<size_t>(source.joint) < jointCount)
            {
                anyValid = true;
                const size_t joint = static_cast<size_t>(source.joint);
                Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
                transform.block<3, 3>(0, 0) = ReadRotation(locals, joint).toRotationMatrix();
                transform.block<3, 1>(0, 3) = ReadTranslation(locals, joint);
                if (driver->evalSpaceJoint >= 0 &&
                    static_cast<size_t>(driver->evalSpaceJoint) < component.size() &&
                    joint < component.size())
                {
                    // UE：SourceBoneTM = SourceBoneCompSpace.GetRelativeTransform(EvalSpaceCompSpace)
                    transform = component[static_cast<size_t>(driver->evalSpaceJoint)].inverse() *
                        component[joint];
                }
                else if (definition.evalFromRefPose)
                {
                    // UE：Pose[bone].GetRelativeTransform(RefPose)，即 RefPose^-1 * Pose。
                    transform = mesh->joints[joint].restLocal.inverse() * transform;
                }
                translation = transform.block<3, 1>(0, 3);
                rotation = Eigen::Quaternionf(transform.block<3, 3>(0, 0)).normalized();
                translation = ToUeTranslation(translation);
                rotation = ToUeRotation(rotation);
            }
            if (definition.translationSource)
                AddVector(input, translation);
            else
                AddRotator(input, QuaternionToRotator(rotation));
        }
        if (!anyValid)
        {
            frame.diagnostics.push_back({definition.name, "no-valid-source-bone",
                "all source bones are missing from the skeleton"});
            continue;
        }

        std::vector<float> weights(driver->targets.size(), 0.0f);
        if (definition.params.solverType == RBFSolverType::Interpolative)
        {
            if (!driver->interpolativeValid)
                continue;
            std::vector<float> valueWeights(driver->targets.size(), 0.0f);
            for (size_t index = 0; index < driver->targets.size(); ++index)
                valueWeights[index] = WeightedValue(
                    EntryDistance(input, driver->targets[index].values,
                        definition.params.distanceMethod,
                        TwistAxisVector(definition.params.twistAxis)),
                    driver->kernelWidth, definition.params.function, false);
            for (size_t index = 0; index < driver->targets.size(); ++index)
            {
                float weight = 0.0f;
                for (size_t other = 0; other < driver->targets.size(); ++other)
                    weight += driver->coefficients(static_cast<Eigen::Index>(index),
                        static_cast<Eigen::Index>(other)) * valueWeights[other];
                weights[index] = std::clamp(weight, 0.0f, 1.0f);
                weights[index] *= driver->targets[index].scale;
            }
        }
        else
        {
            for (size_t index = 0; index < driver->targets.size(); ++index)
            {
                const float distance = DistanceBetween(input, driver->targets[index].values,
                    definition.params, driver->targets[index].distanceMethod);
                const float radius = GetRadius(driver->targets[index].scale, definition.params);
                const float scaled = distance / radius;
                const RBFFunctionType function =
                    driver->targets[index].function == RBFFunctionType::Default ?
                    definition.params.function : driver->targets[index].function;
                weights[index] = WeightedValue(scaled, 1.0f, function, true);
            }
        }

        float total = 0.0f;
        for (float weight : weights) total += weight;
        if (total <= KindaSmallNumber)
            continue;
        float weightScale = 1.0f;
        if (total > 1.0f)
            weightScale = 1.0f / total;
        else if (definition.params.normalizeMethod == RBFNormalizeMethod::AlwaysNormalize)
            weightScale = 1.0f / total;
        else if (definition.params.normalizeMethod == RBFNormalizeMethod::NormalizeWithinMedian &&
            definition.params.medianMax >= definition.params.medianMin)
        {
            std::vector<float> medianEntry;
            while (input.size() > medianEntry.size())
                AddVector(medianEntry, definition.params.medianReference);
            const float medianDistance = DistanceBetween(input, medianEntry, definition.params,
                RBFDistanceMethod::Default);
            if (medianDistance <= definition.params.medianMin)
                weightScale = 1.0f / total;
            else if (medianDistance < definition.params.medianMax)
            {
                const float bias = std::clamp((medianDistance - definition.params.medianMin) /
                    (definition.params.medianMax - definition.params.medianMin), 0.0f, 1.0f);
                weightScale = (1.0f - bias) * (1.0f / total) + bias;
            }
        }

        for (size_t index = 0; index < driver->targets.size(); ++index)
        {
            const float weight = weights[index] * weightScale;
            if (weight <= definition.params.weightThreshold)
                continue;
            const std::string& name = driver->targets[index].drivenName;
            auto existing = std::find_if(frame.curves.begin(), frame.curves.end(),
                [&](const PoseCurveValue& curve) { return curve.name == name; });
            if (existing != frame.curves.end())
                existing->value = weight;
            else
                frame.curves.push_back({name, weight});
        }
        ++frame.solvedDrivers;
        frame.active = true;
    }
    return frame;
}
}
