#include "footPlacement.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <ozz/animation/runtime/ik_two_bone_job.h>
#include <ozz/base/maths/simd_quaternion.h>

namespace VL::Animation
{
namespace
{
size_t FindJoint(const SkeletonMesh& mesh, const std::string& name)
{
    for (size_t index = 0; index < mesh.joints.size(); ++index)
        if (mesh.joints[index].name == name) return index;
    throw std::invalid_argument("Foot placement requires joint: " + name);
}

Eigen::Vector3f Position(const Eigen::Matrix4f& matrix)
{
    return matrix.block<3, 1>(0, 3);
}

Eigen::Matrix3f Rotation(const Eigen::Matrix4f& matrix)
{
    return matrix.block<3, 3>(0, 0) / matrix.block<3, 1>(0, 0).norm();
}

Eigen::Vector3f Point(const Eigen::Matrix4f& matrix, const Eigen::Vector3f& local)
{
    return (matrix * local.homogeneous()).head<3>();
}

ozz::math::Float4x4 OzzMatrix(const Eigen::Matrix4f& matrix)
{
    ozz::math::Float4x4 result;
    for (int column = 0; column < 4; ++column)
        result.cols[column] = ozz::math::simd_float4::LoadPtrU(matrix.col(column).data());
    return result;
}

void SetLane(ozz::math::SimdFloat4& vector, size_t lane, float value)
{
    float values[4];
    ozz::math::StorePtrU(vector, values);
    values[lane] = value;
    vector = ozz::math::simd_float4::LoadPtrU(values);
}

void SetRotation(LocalPose& locals, size_t joint, const Eigen::Quaternionf& rotation)
{
    auto& value = locals[joint / 4].rotation;
    const size_t lane = joint % 4;
    SetLane(value.x, lane, rotation.x());
    SetLane(value.y, lane, rotation.y());
    SetLane(value.z, lane, rotation.z());
    SetLane(value.w, lane, rotation.w());
}

void MultiplyRotation(LocalPose& locals, size_t joint, const ozz::math::SimdQuaternion& correction)
{
    // 与 ozz two_bone_ik 示例一致：局部原旋转右乘修正，不直接改模型矩阵。
    auto& rotation = locals[joint / 4].rotation;
    ozz::math::SimdQuaternion lanes[4];
    ozz::math::Transpose4x4(&rotation.x, &lanes->xyzw);
    lanes[joint % 4] = lanes[joint % 4] * correction;
    ozz::math::Transpose4x4(&lanes->xyzw, &rotation.x);
}

void TranslateLocal(LocalPose& locals, size_t joint, const Eigen::Vector3f& delta)
{
    auto& value = locals[joint / 4].translation;
    float coordinates[4];
    ozz::math::SimdFloat4* channels[3] = {&value.x, &value.y, &value.z};
    for (size_t axis = 0; axis < 3; ++axis)
    {
        ozz::math::StorePtrU(*channels[axis], coordinates);
        coordinates[joint % 4] += delta[axis];
        *channels[axis] = ozz::math::simd_float4::LoadPtrU(coordinates);
    }
}

Eigen::Vector3f Sole(const std::vector<Eigen::Matrix4f>& models, const Eigen::Matrix4f& component,
    size_t foot, const Eigen::Vector3f& local)
{
    return Point(component * models.at(foot), local);
}

float ReachablePelvisOffset(const Eigen::Vector3f& hip, const Eigen::Vector3f& ankle, float reach)
{
    const Eigen::Vector3f difference = ankle - hip;
    const float horizontalSquared = difference.x() * difference.x() + difference.z() * difference.z();
    if (horizontalSquared >= reach * reach) return -15;
    // 固定水平目标下，垂直上界由球面截面精确给出；径向超长量不等于所需下移量。
    const float maximumHeight = ankle.y() + std::sqrt(reach * reach - horizontalSquared);
    return std::clamp(maximumHeight - hip.y(), -15.0f, 0.0f);
}

bool SupportsFoot(const FootGroundHit& hit)
{
    return hit.hit && hit.normal.y() >= 0.70710678f && !hit.supportIdentity.empty();
}

bool SameSupportPlane(const FootGroundHit& first, const FootGroundHit& second)
{
    return first.normal.dot(second.normal) > 0.999f &&
        std::abs(first.normal.dot(second.point - first.point)) < 0.1f &&
        std::abs(second.normal.dot(first.point - second.point)) < 0.1f;
}
}

FootPlacementSolver::FootPlacementSolver(const SkeletonMesh& mesh)
{
    pelvis = FindJoint(mesh, "pelvis");
    const float planeHeight = mesh.joints.at(0).restModel(1, 3);
    for (size_t side = 0; side < legs.size(); ++side)
    {
        auto& leg = legs[side];
        const std::string suffix = side == 0 ? "_l" : "_r";
        leg.thigh = FindJoint(mesh, "thigh" + suffix);
        leg.calf = FindJoint(mesh, "calf" + suffix);
        leg.foot = FindJoint(mesh, "foot" + suffix);
        leg.ball = FindJoint(mesh, "ball" + suffix);
        if (mesh.joints[leg.thigh].parent != static_cast<int>(pelvis) ||
            mesh.joints[leg.calf].parent != static_cast<int>(leg.thigh) ||
            mesh.joints[leg.foot].parent != static_cast<int>(leg.calf) ||
            mesh.joints[leg.ball].parent != static_cast<int>(leg.foot))
            throw std::invalid_argument("Foot placement requires a direct pelvis/thigh/calf/foot/ball chain");
        const auto& foot = mesh.joints[leg.foot].restModel;
        Eigen::Vector3f reference = Position(mesh.joints[leg.ball].restModel);
        reference.y() = planeHeight;
        leg.soleLocal = Point(foot.inverse(), reference);
        // 源骨架没有 heel 骨：使用踝下的绑定平面代理，不按鞋长常量猜实际鞋跟。
        Eigen::Vector3f heel = Position(foot);
        heel.y() = planeHeight;
        leg.heelLocal = Point(foot.inverse(), heel);
        leg.upLocal = Rotation(foot).transpose() * Eigen::Vector3f::UnitY();
        const auto& calf = mesh.joints[leg.calf].restModel;
        const Eigen::Vector3f upper = Position(mesh.joints[leg.thigh].restModel) - Position(calf);
        const Eigen::Vector3f lower = Position(foot) - Position(calf);
        const Eigen::Vector3f normal = upper.cross(lower);
        if (upper.norm() < 1 || lower.norm() < 1 || normal.norm() < 0.001f)
            throw std::invalid_argument("Foot placement requires nondegenerate rest legs with a defined knee plane");
        // 正向轴使膝角张开；左右腿骨架局部坐标不同，不能写死同一个 XYZ 轴。
        leg.midAxis = (Rotation(calf).transpose() * normal.normalized()).normalized();
    }
}

void FootPlacementSolver::SetEnabled(bool value)
{
    if (state.enabled == value) return;
    state.enabled = value;
    Reset(value ? "warming-up" : "disabled");
}

void FootPlacementSolver::SetLockType(FootLockType value)
{
    if (lockType == value) return;
    lockType = value;
    state.lockMode = value == FootLockType::Auto ? "auto" :
        value == FootLockType::PivotAroundBall ? "ball" : "ankle";
    // 支点语义变化后旧锚点不可复用，重新等待低速近地帧。
    Reset("lock-mode");
}

void FootPlacementSolver::Reset(const char* reason)
{
    const bool enabled = state.enabled;
    const std::string lockMode = state.lockMode.empty() ? "auto" : state.lockMode;
    state = {};
    state.enabled = enabled;
    state.lockMode = lockMode;
    state.reason = reason;
    historyValid = false;
}

void FootPlacementSolver::ValidateComponent(const Eigen::Matrix4f& component)
{
    const Eigen::Matrix3f linear = component.block<3, 3>(0, 0);
    const float scale = linear.col(0).norm();
    if (!component.allFinite() || scale < 0.0001f ||
        (linear.transpose() * linear - Eigen::Matrix3f::Identity() * scale * scale).norm() >
            0.001f * scale * scale || linear.determinant() <= 0 ||
        (component.row(3) - Eigen::RowVector4f(0, 0, 0, 1)).norm() > 0.0001f)
        throw std::invalid_argument("Foot placement requires a rigid positive uniform component scale");
}

LocalPose FootPlacementSolver::Solve(const SkeletonMesh& mesh, const LocalPose& locals,
    const std::vector<Eigen::Matrix4f>& preIkModels, const Eigen::Matrix4f& component,
    bool grounded, const std::string& owner, float deltaSeconds, const GroundProbe& probe)
{
    ValidateComponent(component);
    LocalPose solved = locals;
    if (!grounded || deltaSeconds > 0.1f)
    {
        Reset(grounded ? "large-delta" : "airborne");
        return solved;
    }
    if (historyValid && ((Position(component) - Position(previousComponent)).norm() > 100 ||
        Eigen::AngleAxisf(Rotation(component) * Rotation(previousComponent).transpose()).angle() >
            3.14159265358979323846f / 3))
    {
        Reset("teleported");
        return solved;
    }
    std::array<Eigen::Vector3f, 2> targetAnkles;
    std::array<Eigen::Matrix3f, 2> targetRotations;
    float desiredPelvis = 0;
    // 骨盆下移同时带动摆动腿；只允许下降到摆动参考点仍高于其地面的高度。
    float swingClearance = -std::numeric_limits<float>::max();
    bool anyGround = false;
    bool anyPlanted = false;
    const float componentScale = component.block<3, 1>(0, 0).norm();
    for (size_t side = 0; side < legs.size(); ++side)
    {
        auto& leg = legs[side];
        auto& foot = state.feet[side];
        foot.preSole = Sole(preIkModels, component, leg.foot, leg.soleLocal);
        foot.preHeel = Sole(preIkModels, component, leg.foot, leg.heelLocal);
        foot.postSole = foot.preSole;
        foot.postHeel = foot.preHeel;
        foot.probeOrigin = foot.preSole + Eigen::Vector3f(0, 30, 0);
        foot.probeDisplacement = {0, -70, 0};
        foot.heelProbeOrigin = foot.preHeel + Eigen::Vector3f(0, 30, 0);
        foot.heelProbeDisplacement = {0, -70, 0};
        const std::array<FootGroundHit, 2> hits{
            probe.ProbeGround(owner, foot.probeOrigin, foot.probeDisplacement),
            probe.ProbeGround(owner, foot.heelProbeOrigin, foot.heelProbeDisplacement)};
        const std::array<bool, 2> supported{SupportsFoot(hits[0]), SupportsFoot(hits[1])};
        foot.grounded = supported[0];
        foot.heelGrounded = supported[1];
        foot.groundPoint = hits[0].point;
        foot.groundNormal = hits[0].normal;
        foot.heelGroundPoint = hits[1].point;
        foot.heelGroundNormal = hits[1].normal;
        foot.heelSupportIdentity = hits[1].supportIdentity;
        foot.targetError = 0;
        const std::array<Eigen::Vector3f, 2> references{foot.preSole, foot.preHeel};
        const std::array<float, 2> speeds{
            historyValid ? (foot.preSole - leg.previousSole).norm() / deltaSeconds : 0,
            historyValid ? (foot.preHeel - leg.previousHeel).norm() / deltaSeconds : 0};
        const std::array<float, 2> gaps{hits[0].normal.dot(foot.preSole - hits[0].point),
            hits[1].normal.dot(foot.preHeel - hits[1].point)};
        leg.previousSole = foot.preSole;
        leg.previousHeel = foot.preHeel;
        if (!supported[0] && !supported[1])
        {
            foot.planted = false;
            foot.supportIdentity.clear();
            foot.contactMode = "none";
            continue;
        }
        anyGround = true;
        const std::array<bool, 2> canPlant{
            supported[0] && historyValid && speeds[0] <= 40 && std::abs(gaps[0]) <= 10,
            supported[1] && historyValid && speeds[1] <= 40 && std::abs(gaps[1]) <= 10};
        foot.soleSpeed = speeds[0];
        foot.heelSpeed = speeds[1];
        foot.soleGap = gaps[0];
        foot.heelGap = gaps[1];
        foot.canPlantBall = canPlant[0];
        foot.canPlantHeel = canPlant[1];
        // Auto 是 VL 的动态支点（补 UE 尚未实现的自动检测）；Ball/Ankle 是显式 UE 对照模式。
        // 三种模式都只选择“哪个参考点固定在锚点”，IK 终点始终是 foot 骨。
        const bool autoMode = lockType == FootLockType::Auto;
        size_t pivot = autoMode ? (leg.heelPivot ? 1 : 0) :
            (lockType == FootLockType::PivotAroundAnkle ? 1 : 0);
        const bool separatePlanes = supported[0] && supported[1] && !SameSupportPlane(hits[0], hits[1]);
        const size_t higher = hits[1].point.y() > hits[0].point.y() ? 1 : 0;
        const bool rollSwitch = autoMode && foot.planted && supported[0] && supported[1] &&
            !separatePlanes && canPlant[1 - pivot] && gaps[1 - pivot] + 1 < gaps[pivot];
        // 保留源节点“源动画低速且接近地面才锁”的意图；Auto 允许滚动切换，显式模式不摇摆。
        if (foot.planted && (!supported[pivot] || speeds[pivot] > 80 || std::abs(gaps[pivot]) > 16 ||
            (references[pivot] - foot.anchor).norm() > 20 ||
            foot.supportIdentity != hits[pivot].supportIdentity || rollSwitch))
            foot.planted = false;
        if (foot.planted && autoMode && separatePlanes && higher != pivot && canPlant[higher])
            foot.planted = false;
        const bool plantable = autoMode ? (canPlant[0] || canPlant[1]) : canPlant[pivot];
        if (!foot.planted && plantable)
        {
            if (autoMode)
            {
                if (separatePlanes) pivot = canPlant[higher] ? higher : (canPlant[0] ? 0 : 1);
                else if (canPlant[0] && canPlant[1])
                {
                    if (gaps[1] + 1 < gaps[0]) pivot = 1;
                    else if (gaps[0] + 1 < gaps[1]) pivot = 0;
                }
                else pivot = canPlant[0] ? 0 : 1;
            }
            leg.heelPivot = pivot == 1;
            foot.planted = true;
            foot.anchor = references[pivot] - hits[pivot].normal * gaps[pivot];
        }
        foot.contactMode = foot.planted ? (leg.heelPivot ? "heel" : "ball") : "none";
        // 命中地面不等于支撑：摆腿/高抬脚保持原动画，只有已通过迟滞的锁足才求解。
        if (!foot.planted)
        {
            // 未锁足的点不会被 IK 抬回；限制骨盆下移，避免摆动脚被整体压入地面。
            for (size_t point = 0; point < 2; ++point)
                if (hits[point].hit && hits[point].normal.y() > 0.5f)
                {
                    const float gap = hits[point].normal.dot(references[point] - hits[point].point);
                    swingClearance = std::max(swingClearance, -gap / hits[point].normal.y());
                }
            foot.supportIdentity.clear();
            continue;
        }
        foot.supportIdentity = hits[pivot].supportIdentity;
        const auto& hit = hits[pivot];
        const auto& pivotLocal = pivot == 1 ? leg.heelLocal : leg.soleLocal;
        anyPlanted = true;
        const Eigen::Vector3f targetSole = foot.anchor;
        const Eigen::Matrix3f preWorldRotation = Rotation(component * preIkModels[leg.foot]);
        const Eigen::Vector3f currentUp = preWorldRotation * leg.upLocal;
        const bool fullPlaneContact = canPlant[0] && canPlant[1] && !separatePlanes;
        // 单支点须保留源脚跟抬起/足部滚动，只叠加地形倾斜；双点均低速近地且共面才整脚贴合。
        // 否则 ball 的低速会把高速抬起的 heel 压回地面，误把脚尖支撑当作平脚支撑。
        const Eigen::Vector3f alignFrom = fullPlaneContact ? currentUp : Eigen::Vector3f::UnitY();
        targetRotations[side] =
            Eigen::Quaternionf::FromTwoVectors(alignFrom, hit.normal).toRotationMatrix() * preWorldRotation;
        targetAnkles[side] = targetSole - targetRotations[side] * pivotLocal * componentScale;
        // 踏面不共面时只取明确的单支点，不把台阶连成虚构斜坡；同一支撑面内另一参考点保持非穿透。
        // 复探目标位置而非沿用源足下方的命中，避免锁点与源足分离后跨越台阶边缘。
        const auto& otherLocal = pivot == 1 ? leg.soleLocal : leg.heelLocal;
        const Eigen::Vector3f otherTarget = targetAnkles[side] +
            targetRotations[side] * otherLocal * componentScale;
        const auto otherGround = probe.ProbeGround(owner, otherTarget + Eigen::Vector3f(0, 30, 0),
            Eigen::Vector3f(0, -70, 0));
        // 只在另一参考点位于同一支撑面时做非穿透修正；跨踏面时它属于下一级踏面，
        // 强行抬到当前平面会把支撑点顶离锚点，形成台阶边缘悬浮。
        if (SupportsFoot(otherGround) && SameSupportPlane(otherGround, hit))
        {
            const float otherGap = otherGround.normal.dot(otherTarget - otherGround.point);
            if (otherGap < 0) targetAnkles[side].y() -= otherGap / otherGround.normal.y();
        }
        const Eigen::Vector3f hip = Point(component, Position(preIkModels[leg.thigh]));
        const float length = ((Position(preIkModels[leg.thigh]) - Position(preIkModels[leg.calf])).norm() +
            (Position(preIkModels[leg.calf]) - Position(preIkModels[leg.foot])).norm()) * componentScale;
        // ozz soften=.98 从此半径开始让目标落后；在同一半径内为真实锁点和将来重锚点预留可达量。
        // 预期点随原姿态/地面连续移动，避免到 20cm 解锁重锚时才启动 pelvis 追赶。
        // 当前足仍锁在旧 anchor，不能提前移动脚来掩盖不可达；pelvis 上下仍各限 60cm/s。
        const Eigen::Vector3f prospectiveAnkle = references[pivot] - hit.normal * gaps[pivot] -
            targetRotations[side] * pivotLocal * componentScale;
        desiredPelvis = std::min(desiredPelvis,
            ReachablePelvisOffset(hip, targetAnkles[side], length * 0.98f));
        desiredPelvis = std::min(desiredPelvis,
            ReachablePelvisOffset(hip, prospectiveAnkle, length * 0.98f));
    }
    // 支撑间隙只用于限制向下骨盆偏移，不把数值噪声变成向上抬升。
    if (desiredPelvis < 0 && swingClearance > desiredPelvis)
        desiredPelvis = std::min(0.0f, std::max(desiredPelvis, swingClearance));
    const float maximumPelvisChange = 60 * deltaSeconds;
    state.pelvisOffset += std::clamp(desiredPelvis - state.pelvisOffset,
        -maximumPelvisChange, maximumPelvisChange);
    if (std::abs(state.pelvisOffset) < 1e-4f) state.pelvisOffset = 0;
    const bool hadHistory = historyValid;
    historyValid = anyGround;
    previousComponent = component;
    // 地面步态的双脚短暂解锁只平滑释放视觉骨盆；空中/传送等明确不连续仍走前面的硬重置。
    state.active = anyPlanted || state.pelvisOffset != 0;
    state.reason = anyPlanted ? "planted" : (state.active ? "releasing" :
        (!anyGround ? "no-ground" : (hadHistory ? "unplanted" : "warming-up")));
    if (!state.active) return solved;
    // 只改视觉 pelvis；胶囊/Jolt 位移所有权保持在角色物理路径。
    const Eigen::Vector3f modelOffset = component.block<3, 3>(0, 0).inverse() *
        Eigen::Vector3f(0, state.pelvisOffset, 0);
    const int parent = mesh.joints[pelvis].parent;
    const Eigen::Vector3f localOffset = parent < 0 ? modelOffset :
        preIkModels[parent].block<3, 3>(0, 0).inverse() * modelOffset;
    TranslateLocal(solved, pelvis, localOffset);
    auto models = LocalToModels(mesh, solved);
    for (size_t side = 0; side < legs.size(); ++side)
    {
        if (!state.feet[side].planted) continue;
        const auto& leg = legs[side];
        const auto start = OzzMatrix(models[leg.thigh]);
        const auto middle = OzzMatrix(models[leg.calf]);
        const auto end = OzzMatrix(models[leg.foot]);
        const Eigen::Vector3f target = Point(component.inverse(), targetAnkles[side]);
        const Eigen::Vector3f hip = Position(models[leg.thigh]);
        const Eigen::Vector3f ankle = Position(models[leg.foot]);
        const Eigen::Vector3f knee = Position(models[leg.calf]);
        const Eigen::Vector3f along = (ankle - hip).normalized();
        Eigen::Vector3f pole = knee - hip - along * along.dot(knee - hip);
        if (pole.squaredNorm() < 0.0001f)
            pole = (Rotation(models[leg.calf]) * leg.midAxis).cross(along);
        pole.normalize();
        ozz::animation::IKTwoBoneJob job;
        job.start_joint = &start;
        job.mid_joint = &middle;
        job.end_joint = &end;
        job.target = ozz::math::simd_float4::Load3PtrU(target.data());
        job.pole_vector = ozz::math::simd_float4::Load3PtrU(pole.data());
        job.mid_axis = ozz::math::simd_float4::Load3PtrU(leg.midAxis.data());
        job.soften = 0.98f;
        ozz::math::SimdQuaternion startCorrection, middleCorrection;
        job.start_joint_correction = &startCorrection;
        job.mid_joint_correction = &middleCorrection;
        if (!job.Run()) throw std::runtime_error("Foot two-bone IK failed");
        MultiplyRotation(solved, leg.thigh, startCorrection);
        MultiplyRotation(solved, leg.calf, middleCorrection);
        models = LocalToModels(mesh, solved);
        const Eigen::Matrix3f desiredModelRotation = Rotation(component).transpose() * targetRotations[side];
        const Eigen::Matrix3f footLocalRotation = Rotation(models[leg.calf]).transpose() * desiredModelRotation;
        SetRotation(solved, leg.foot, Eigen::Quaternionf(footLocalRotation).normalized());
        models = LocalToModels(mesh, solved);
    }
    return solved;
}

void FootPlacementSolver::Measure(const std::vector<Eigen::Matrix4f>& models, const Eigen::Matrix4f& component)
{
    for (size_t side = 0; side < legs.size(); ++side)
    {
        auto& foot = state.feet[side];
        foot.postSole = Sole(models, component, legs[side].foot, legs[side].soleLocal);
        foot.postHeel = Sole(models, component, legs[side].foot, legs[side].heelLocal);
        if (!historyValid && state.reason != "releasing" && state.reason != "no-ground")
        {
            foot.preSole = foot.postSole;
            foot.preHeel = foot.postHeel;
        }
        const auto& postPivot = legs[side].heelPivot ? foot.postHeel : foot.postSole;
        foot.targetError = foot.planted ? (postPivot - foot.anchor).norm() : 0;
    }
}
}
