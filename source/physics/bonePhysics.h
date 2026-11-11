#pragma once

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace VL::Physics
{
// UE AnimNode_RigidBody 的正式原生子集：按 PhysicsAsset 的骨骼刚体做真实模拟。
//
// 一个 set 对应一个 PA_* 资产：kinematic 刚体每帧跟随动画姿态，simulated 刚体由
// Jolt 求解（胶囊形状 + swing/twist 限角 + 质量/阻尼），结果再写回骨骼局部姿态。
// 资产单位是 UE 厘米；Jolt 侧统一换算成米。

enum class BonePhysicsMotion
{
    Free,
    Limited,
    Locked
};

struct BonePhysicsShape
{
    float radius = 1.0f;
    // UE SphylElem.Length 是圆柱段全长，Jolt 需要半高。
    float halfHeight = 0.0f;
    Eigen::Vector3f center = Eigen::Vector3f::Zero();
    Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
};

struct BonePhysicsBody
{
    std::string bone;
    bool simulated = false;
    BonePhysicsShape shape;
    // <=0 表示按形状体积与密度估算。
    float mass = 0.0f;
    float linearDamping = 0.0f;
    float angularDamping = 0.0f;
};

struct BonePhysicsLimit
{
    BonePhysicsMotion motion = BonePhysicsMotion::Free;
    float degrees = 0.0f;
};

struct BonePhysicsConstraint
{
    std::string name;
    std::string child;
    std::string parent;
    BonePhysicsLimit swing1;
    BonePhysicsLimit swing2;
    BonePhysicsLimit twist;
    float stiffness = 0.0f;
    float damping = 0.0f;
    Eigen::Vector3f childPosition = Eigen::Vector3f::Zero();
    Eigen::Quaternionf childRotation = Eigen::Quaternionf::Identity();
    Eigen::Vector3f parentPosition = Eigen::Vector3f::Zero();
    Eigen::Quaternionf parentRotation = Eigen::Quaternionf::Identity();
};

struct BonePhysicsSet
{
    std::string name;
    std::string asset;
    std::string baseBone;
    Eigen::Vector3f gravity = Eigen::Vector3f(0.0f, -980.0f, 0.0f);
    std::vector<BonePhysicsBody> bodies;
    std::vector<BonePhysicsConstraint> constraints;
};

struct BonePhysicsAsset
{
    std::string format;
    std::string id;
    std::string sourcePath;
    std::string sourceDigest;
    std::vector<BonePhysicsSet> sets;
};

struct BonePhysicsDiagnostic
{
    std::string set;
    std::string code;
    std::string detail;
};

struct BonePhysicsState
{
    bool enabled = false;
    bool active = false;
    int simulatedBodies = 0;
    int constraints = 0;
    // 本帧最大的刚体位移（cm），用于诊断与传送检测。
    float maxDisplacement = 0.0f;
    std::string reason = "disabled";
};

// 供 DebugDraw 使用的最小刚体描述（世界空间，厘米）。
struct BonePhysicsDebugBody
{
    std::string bone;
    std::string constraintParent;
    Eigen::Vector3f center = Eigen::Vector3f::Zero();
    Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
    float radius = 0.0f;
    float halfHeight = 0.0f;
    bool simulated = false;
};

// 纯解析；未知字段/非 capsule 形状/非法限角一律拒绝。
BonePhysicsAsset ParseBonePhysicsAsset(const nlohmann::json& json, std::string_view sourcePath);

std::shared_ptr<const BonePhysicsAsset> LoadBonePhysicsAsset(
    const std::filesystem::path& resourceRoot, const std::string& relativePath);

// 每个角色一份；内部拥有独立的 Jolt PhysicsSystem，只在 GT 上调用。
// 本类不依赖动画运行时头文件：输入/输出都是世界空间（厘米）的全局矩阵，
// 局部姿态的采样与写回由调用方（SkeletalPlayback）完成。
class BonePhysicsSolver
{
public:
    BonePhysicsSolver(std::vector<std::string> jointNames, std::vector<int> jointParents,
        std::vector<Eigen::Matrix4f> bindLocals, std::shared_ptr<const BonePhysicsAsset> asset);
    BonePhysicsSolver(const BonePhysicsSolver&) = delete;
    BonePhysicsSolver& operator=(const BonePhysicsSolver&) = delete;
    ~BonePhysicsSolver();

    const BonePhysicsAsset* Asset() const;
    const std::vector<BonePhysicsDiagnostic>& BindDiagnostics() const { return bindDiagnostics; }
    const BonePhysicsState& State() const { return state; }
    bool Valid() const { return validSets > 0; }

    // globals 必须是已乘组件变换的世界矩阵（cm）；simulated 骨会被就地覆盖。
    bool Solve(std::vector<Eigen::Matrix4f>& globals, float deltaSeconds);
    // globals 为 Solve 之后的世界矩阵；输出的 center/rotation 已包含胶囊偏移。
    void CollectDebugBodies(const std::vector<Eigen::Matrix4f>& globals,
        std::vector<BonePhysicsDebugBody>& out) const;
    void Reset(const char* reason);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::shared_ptr<const BonePhysicsAsset> asset;
    std::vector<std::string> jointNames;
    std::vector<int> jointParents;
    std::vector<Eigen::Matrix4f> bindLocals;
    std::vector<BonePhysicsDiagnostic> bindDiagnostics;
    BonePhysicsState state;
    size_t validSets = 0;
};
}
