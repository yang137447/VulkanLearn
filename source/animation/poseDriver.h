#pragma once

#include "animationAssets.h"

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace VL::Animation
{
// UE AnimNode_PoseDriver 的正式原生子集（UE 5.8）。
//
// 只实现 Echo PostProcess 实际使用、且能逐项对照 UE 源码的路径：
//   Rotation / Translation 输入，DriveCurves 输出。
// DrivePoses 依赖 UPoseAsset 混合，不在当前运行时能力内；解析期直接拒绝，
// 不允许静默降级成空曲线。
//
// 空间约定：资产里的 TargetTranslation / TargetRotation 保持 UE 空间（cm、度、
// FRotator 的 Pitch/Yaw/Roll 语义）；求解前把 VL 局部姿态共轭回 UE 空间再比较。
// 这样资产值可以逐字节与 T3D 对照，不需要在导入工具里做二次轴向换算。

enum class RBFSolverType
{
    Additive,
    Interpolative
};

enum class RBFFunctionType
{
    Gaussian,
    Exponential,
    Linear,
    Cubic,
    Quintic,
    // 继承 FRBFParams::Function
    Default
};

enum class RBFDistanceMethod
{
    Euclidean,
    Quaternion,
    SwingAngle,
    TwistAngle,
    // 继承 FRBFParams::DistanceMethod
    Default
};

enum class RBFNormalizeMethod
{
    OnlyNormalizeAboveOne,
    AlwaysNormalize,
    NormalizeWithinMedian,
    NoNormalization
};

enum class PoseDriverAxis
{
    X,
    Y,
    Z
};

const char* RBFSolverTypeName(RBFSolverType value);
const char* RBFFunctionTypeName(RBFFunctionType value);
const char* RBFDistanceMethodName(RBFDistanceMethod value);
const char* RBFNormalizeMethodName(RBFNormalizeMethod value);
const char* PoseDriverAxisName(PoseDriverAxis value);
bool TryParsePoseDriverAxis(const std::string& value, PoseDriverAxis& axis);

// UE FQuat::ToSwingTwist 的共用实现：先把向量部分投影到 twist 轴求出 twist，
// 再 OutSwing = value * OutTwist^-1。PoseDriver 的 SwingAngle 与 ControlRig 替代
// 节点都走这一份，避免两处公式漂移。
void ToSwingTwist(const Eigen::Quaternionf& value, const Eigen::Vector3f& axis,
    Eigen::Quaternionf& swing, Eigen::Quaternionf& twist);
Eigen::Vector3f PoseDriverAxisVector(PoseDriverAxis axis);

// UE FRBFTarget：每个目标对每个 SourceBone 各保存一份 T/R。
// PoseTargets 的旋转是作者姿态下的局部旋转（UE FRotator），不做参考姿态归一化，
// 除非 PoseDriver 打开了 bEvalFromRefPose。
struct PoseDriverTarget
{
    std::vector<Eigen::Vector3f> translations;
    // 每项为 (Pitch, Yaw, Roll)，单位度，保持 T3D 作者顺序。
    std::vector<Eigen::Vector3f> rotations;
    float scale = 1.0f;
    RBFDistanceMethod distanceMethod = RBFDistanceMethod::Default;
    RBFFunctionType function = RBFFunctionType::Default;
    bool applyCustomCurve = false;
    std::string drivenName;
};

struct RBFParams
{
    RBFSolverType solverType = RBFSolverType::Additive;
    float radius = 1.0f;
    bool automaticRadius = false;
    RBFFunctionType function = RBFFunctionType::Gaussian;
    // 与 FAnimNode_PoseDriver 构造函数一致：默认 SwingAngle，不是 FRBFParams 的 Euclidean。
    RBFDistanceMethod distanceMethod = RBFDistanceMethod::SwingAngle;
    PoseDriverAxis twistAxis = PoseDriverAxis::X;
    float weightThreshold = 0.0001f;
    RBFNormalizeMethod normalizeMethod = RBFNormalizeMethod::OnlyNormalizeAboveOne;
    Eigen::Vector3f medianReference = Eigen::Vector3f::Zero();
    float medianMin = 45.0f;
    float medianMax = 60.0f;
};

struct PoseDriverDefinition
{
    std::string name;
    std::vector<std::string> sourceBones;
    std::string evalSpaceBone;
    bool evalFromRefPose = false;
    bool translationSource = false;
    std::vector<PoseDriverTarget> targets;
    RBFParams params;
};

struct PoseDriverSet
{
    std::string format;
    std::string id;
    std::string sourcePath;
    std::string sourceDigest;
    std::vector<PoseDriverDefinition> drivers;
};

struct PoseCurveValue
{
    std::string name;
    float value = 0;
};

struct PoseDriverDiagnostic
{
    std::string driver;
    std::string code;
    std::string detail;
};

struct PoseDriverFrame
{
    // 按驱动顺序合并；重名曲线后写覆盖，与 UE FBlendedCurve::Union 一致。
    std::vector<PoseCurveValue> curves;
    std::vector<PoseDriverDiagnostic> diagnostics;
    bool active = false;
    int solvedDrivers = 0;
};

// 纯解析：只接收已解析 JSON，不读文件、不算摘要。
// 未知字段、错误类型、非有限值、空 SourceBones、DrivePoses、自定义曲线一律拒绝，
// 避免资产笔误被解释成另一套行为。
PoseDriverSet ParsePoseDriverSet(const nlohmann::json& json, std::string_view sourcePath);

// 从资源根读取正式资产；路径必须是资源相对路径，文件摘要写入 sourceDigest，
// 供上层缓存键与诊断使用。解析失败抛 std::invalid_argument / std::runtime_error。
std::shared_ptr<const PoseDriverSet> LoadPoseDriverSet(const std::filesystem::path& resourceRoot,
    const std::string& relativePath);

// 单个驱动的编译结果：关节下标、参考姿态与预解 RBF 系数。
struct PoseDriverCompiled;

// 绑定一次网格；缺骨只记录诊断并让该驱动保持 UE 的“identity 输入”行为，
// 不使播放器构造失败。Solve 只读姿态，不写播放器状态，可在任意候选姿态上求值。
class PoseDriverSolver
{
public:
    PoseDriverSolver(const SkeletonMesh& mesh, std::shared_ptr<const PoseDriverSet> set);
    PoseDriverSolver(const PoseDriverSolver&) = delete;
    PoseDriverSolver& operator=(const PoseDriverSolver&) = delete;
    PoseDriverSolver(PoseDriverSolver&&) noexcept;
    PoseDriverSolver& operator=(PoseDriverSolver&&) noexcept;
    ~PoseDriverSolver();

    const std::shared_ptr<const PoseDriverSet>& Set() const { return asset; }
    const std::vector<PoseDriverDiagnostic>& BindDiagnostics() const { return bindDiagnostics; }

    PoseDriverFrame Solve(const LocalPose& locals) const;

private:
    std::shared_ptr<const PoseDriverSet> asset;
    // 只读网格拓扑/参考姿态；生命周期由播放器保证长于 solver。
    const SkeletonMesh* mesh = nullptr;
    std::vector<std::unique_ptr<PoseDriverCompiled>> drivers;
    std::vector<PoseDriverDiagnostic> bindDiagnostics;
};
}
