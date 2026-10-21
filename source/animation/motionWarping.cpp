#include "animation/motionWarping.h"

#include "animationAssets.h"

#include <ozz/animation/offline/raw_animation_utils.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace VL::Animation
{
namespace
{
Eigen::Matrix3f MakeFromXZ(const Eigen::Vector3f& xHint, const Eigen::Vector3f& zHint)
{
    const Eigen::Vector3f x = xHint.normalized();
    Eigen::Vector3f y = zHint.cross(x);
    if (y.squaredNorm() <= 1e-12f)
        y = (std::abs(x.y()) < 0.9f ? Eigen::Vector3f::UnitY() : Eigen::Vector3f::UnitX()).cross(x);
    y.normalize();
    const Eigen::Vector3f z = x.cross(y).normalized();
    Eigen::Matrix3f basis;
    basis.col(0) = x;
    basis.col(1) = y;
    basis.col(2) = z;
    return basis;
}

bool NearlyZero(const Eigen::Vector3f& value, float threshold)
{
    return value.cwiseAbs().maxCoeff() <= threshold;
}

// VL 的 Mesh 世界矩阵只有正交旋转 + 均匀缩放；逐列归一化即可等价 UE
// FTransform 保存的旋转四元数（InverseTransformPositionNoScale 不消费缩放）。
Eigen::Matrix3f MeshRotation(const Eigen::Matrix4f& transform)
{
    Eigen::Matrix3f rotation = transform.block<3, 3>(0, 0);
    for (int column = 0; column < 3; ++column)
        rotation.col(column).normalize();
    return rotation;
}

float ClipDuration(const Clip& clip)
{
    return clip.runtime ? clip.runtime->duration() : 0.0f;
}

// 与 SkeletalPlayback::RootRotation 同源：根轨道局部旋转，缺轨道/采样失败回退单位。
// 只在旋转 warp 需要窗口首尾/剩余旋转时调用。
Eigen::Quaternionf SampleRootRotation(const Clip& clip, float time)
{
    if (clip.raw.tracks.empty()) return Eigen::Quaternionf::Identity();
    ozz::math::Transform root;
    const float clamped = std::clamp(time, 0.0f, ClipDuration(clip));
    if (!ozz::animation::offline::SampleTrack(clip.raw.tracks[0], clamped, &root))
        return Eigen::Quaternionf::Identity();
    return Eigen::Quaternionf(root.rotation.w, root.rotation.x, root.rotation.y,
        root.rotation.z).normalized();
}

// yaw 必须是 VL Y-up 下绕 +Y 的角度；与 SkeletalPlayback::YawFromQuaternion 同一公式
// （atan2(2(wy+xz), 1-2(y²+x²))）。旋转 warp 的整个通道只处理 yaw，pitch/roll 不执行。
float YawOfQuaternion(const Eigen::Quaternionf& rotation)
{
    const Eigen::Quaternionf q = rotation.normalized();
    return std::atan2(2.0f * (q.w() * q.y() + q.x() * q.z()),
        1.0f - 2.0f * (q.y() * q.y() + q.x() * q.x()));
}

// 折回 [-pi, pi]，保证“最短路径”插值与跨 ±pi 的窗口差一致；正好 ±pi 的
// 方向由 remainder 决定，两个方向表示同一朝向。
float WrapYaw(float yaw)
{
    return std::remainder(yaw, 2.0f * 3.14159265358979323846f);
}

// 从 from 到 to 的根旋转差，再投影到 yaw 通道。使用四元数差而不是直接减 yaw 采样值，
// 使根轨道含 pitch/roll 时仍得到同一个旋转的 yaw 分量。
float YawDelta(const Eigen::Quaternionf& from, const Eigen::Quaternionf& to)
{
    return WrapYaw(YawOfQuaternion(to * from.inverse()));
}

// UE URootMotionModifier_Warp::WarpRotation（RootMotionModifier.cpp:550-622）的
// yaw-only 等价实现。UE 的旋转通道是完整四元数；VL 只发布 RootMotionYawDelta，
// 因此把每一步投影到 Y-up yaw：
//
//   CurrentRotation = Actor 当前世界旋转 * BaseVisualRotationOffset
//   TargetRotation  = CurrentRotation^-1 * (TargetWorld * BaseVisualRotationOffset
//                                          * RootMotionRemainingAfterNotify^-1)
//   TimeRemaining   = (EndTime - PreviousPosition) * WarpRotationTimeMultiplier
//   Alpha           = clamp(DeltaSeconds * PlayRate / TimeRemaining, 0, 1)
//   ThisFrame       = Slerp(RootMotionTotal, TargetRotation, Alpha)
//   DeltaOut        = ThisFrame * RootMotionTotal^-1
//   FinalRotation   = ExtraRootMotion * DeltaOut * RootMotionDelta
//
// 所有旋转都绕同一 Y 轴，BaseVisualRotationOffset 在 yaw 上抵消：目标相对 yaw =
// TargetWorldYaw - ActorYaw - RemainingYaw；纯 yaw 四元数的 Slerp 等价于
// total + Alpha * shortestDelta(target, total)。RootMotionTotal 是“PreviousPosition
// 到 EndTime”的剩余根旋转（不是整窗），与 UE ExtractRootMotion 口径一致；
// DeltaSeconds*PlayRate 在 VL 中就是 currentPosition - previousPosition。
float WarpedRootYawDelta(const Clip& clip, const MotionWarpWindow& window,
    float previousPosition, float currentPosition, float endTime, float duration,
    float actorYaw)
{
    const Eigen::Quaternionf previousRotation = SampleRootRotation(clip, previousPosition);
    const Eigen::Quaternionf endRotation = SampleRootRotation(clip, endTime);
    const float totalYaw = YawDelta(previousRotation, endRotation);

    // UE 只在 bSubtractRemainingRootMotion 时缓存 RootMotionRemainingAfterNotify
    // （RootMotionModifier.cpp:479-487），否则为 Identity。
    float remainingYaw = 0;
    if (window.subtractRemainingRootMotion)
        remainingYaw = YawDelta(endRotation, SampleRootRotation(clip, duration));

    // UE GetTargetRotation() 的 Default 分支：目标世界旋转（AdditionalRotationOffset
    // 在资产里没有对应字段，按 Identity）。VL 的 target 是世界朝向，最终通过
    // RootMotionYawDelta 加到 Actor yaw，因此减去 Actor yaw，而不是 Mesh 世界 yaw。
    const float targetYaw = WrapYaw(YawOfQuaternion(window.targetRotation) - actorYaw - remainingYaw);

    const float multiplier = std::isfinite(window.rotationTimeMultiplier) ?
        std::max(window.rotationTimeMultiplier, 1e-4f) : 1.0f;
    const float timeRemaining = (endTime - previousPosition) * multiplier;
    const float alpha = timeRemaining > 1e-6f ?
        std::clamp((currentPosition - previousPosition) / timeRemaining, 0.0f, 1.0f) : 1.0f;

    // UE FQuat::Slerp 的最短路径：yaw 落在 (-pi, pi] 的角度差。
    const float targetThisFrame = totalYaw + alpha * WrapYaw(targetYaw - totalYaw);
    const float deltaOut = WrapYaw(targetThisFrame - totalYaw);

    // UE SkewWarp：WarpedRotation = ExtraRootMotion * WarpRotation(RootMotionDelta,
    // RootMotionTotal, DeltaSeconds)（RootMotionModifier_SkewWarp.cpp:248-252）。
    // RootMotionDelta 只取 PreviousPosition -> min(CurrentPosition, EndTime)。
    const Eigen::Quaternionf insideRotation =
        SampleRootRotation(clip, std::min(currentPosition, endTime));
    const float rawYaw = YawDelta(previousRotation, insideRotation);
    float extraYaw = 0;
    if (currentPosition > endTime)
        extraYaw = YawDelta(endRotation,
            SampleRootRotation(clip, std::min(currentPosition, duration)));
    return WrapYaw(deltaOut + rawYaw + extraYaw);
}

// 采样一条关节轨道的局部 TRS 并转成 4x4（父空间、厘米）。层级换算只在
// SampleJointModelByIndex 里做一次，不引入第二套采样/混合路径。
Eigen::Matrix4f SampleJointLocal(const SkeletonMesh& mesh, const Clip& clip, size_t joint, float time)
{
    if (joint >= clip.raw.tracks.size()) return mesh.joints[joint].restLocal;
    ozz::math::Transform local;
    if (!ozz::animation::offline::SampleTrack(clip.raw.tracks[joint], time, &local))
        return mesh.joints[joint].restLocal;
    Eigen::Matrix4f result = Eigen::Matrix4f::Identity();
    // Eigen 的四元数构造函数是 (w, x, y, z)，ozz 是 (x, y, z, w)；顺序写错会变成
    // 绕 Z 的 180° 旋转，骨骼偏移会把目标翻到反方向。
    const Eigen::Quaternionf rotation(local.rotation.w, local.rotation.x,
        local.rotation.y, local.rotation.z);
    result.block<3, 3>(0, 0) = rotation.toRotationMatrix() *
        Eigen::Vector3f(local.scale.x, local.scale.y, local.scale.z).asDiagonal();
    result.block<3, 1>(0, 3) = Eigen::Vector3f(local.translation.x, local.translation.y, local.translation.z);
    return result;
}

// 从根沿父链拼到目标关节；链在 mesh.joints 里是拓扑序（父索引 < 子索引），
// 因此先收集再由根向下相乘。
bool SampleJointModelByIndex(const SkeletonMesh& mesh, const Clip& clip, size_t joint,
    float time, Eigen::Matrix4f& outTransform)
{
    if (joint >= mesh.joints.size()) return false;
    std::vector<size_t> chain;
    for (size_t index = joint; index < mesh.joints.size();)
    {
        chain.push_back(index);
        const int parent = mesh.joints[index].parent;
        if (parent < 0) break;
        // 导入器按拓扑序写 joints；父索引不小于子索引说明资产无效，直接放弃该次换算。
        if (static_cast<size_t>(parent) >= index) return false;
        index = static_cast<size_t>(parent);
    }
    const float clamped = std::clamp(time, 0.0f, ClipDuration(clip));
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        model = model * SampleJointLocal(mesh, clip, *it, clamped);
    outTransform = model;
    return true;
}
}

bool SampleBoneModelTransform(const SkeletonMesh& mesh, const Clip& clip,
    const std::string& boneName, float time, Eigen::Matrix4f& outTransform)
{
    if (boneName.empty()) return false;
    for (size_t index = 0; index < mesh.joints.size(); ++index)
        if (mesh.joints[index].name == boneName)
            return SampleJointModelByIndex(mesh, clip, index, time, outTransform);
    return false;
}

Eigen::Vector3f SampleRootMotionTranslation(const Clip& clip, float time)
{
    if (clip.raw.tracks.empty()) return Eigen::Vector3f::Zero();
    const float clamped = std::clamp(time, 0.0f, ClipDuration(clip));
    ozz::math::Transform root;
    if (!ozz::animation::offline::SampleTrack(clip.raw.tracks[0], clamped, &root))
        return Eigen::Vector3f::Zero();
    return {root.translation.x, root.translation.y, root.translation.z};
}

void MotionWarpRuntime::SetWindows(std::vector<MotionWarpWindow> value)
{
    windows = std::move(value);
}

void MotionWarpRuntime::Clear()
{
    windows.clear();
}

std::string MotionWarpRuntime::DebugState(float previousPosition) const
{
    std::ostringstream result;
    for (size_t i = 0; i < windows.size(); ++i)
    {
        if (i != 0) result << ',';
        const auto& window = windows[i];
        result << (window.targetName.empty() ? "window" : window.targetName) << ':'
               << (previousPosition < window.startTime ? "waiting" :
                       previousPosition >= window.endTime ? "done" : "active");
    }
    return result.str();
}

Eigen::Vector3f MotionWarpRuntime::Apply(const SkeletonMesh& mesh, const Clip& clip,
    float previousPosition, float currentPosition, const Eigen::Vector3f& modelDelta,
    const Eigen::Matrix4f& meshTransform, const Eigen::Vector3f& actorPosition,
    float actorYaw, float* outYawDelta) const
{
    if (windows.empty() || !(currentPosition > previousPosition)) return modelDelta;

    const float duration = ClipDuration(clip);
    const Eigen::Matrix3f meshRotation = MeshRotation(meshTransform);
    const Eigen::Vector3f meshOrigin = meshTransform.block<3, 1>(0, 3);
    // UE：MeshTransform = FTransform(MeshRotationOffset, MeshTranslationOffset) * ActorTransform，
    // 再把世界目标换算到 Mesh/骨架模型空间。VL 的 warp 目标以胶囊中心表达，而
    // Mesh 原点在脚底，因此减去 R^T(actor - meshOrigin) 把“可视根 -> 胶囊中心”
    // 的偏移还原，避免重复计算 90 cm 下移。这里的 actorPosition 是 C# 提交的
    // capsule center；C# 只消费 Apply 返回的根位移，不重复这段换算。
    const Eigen::Vector3f capsuleCenterInModel =
        meshRotation.transpose() * (actorPosition - meshOrigin);

    Eigen::Vector3f finalTranslation = modelDelta;
    for (const auto& window : windows)
    {
        // UE URootMotionModifier::Update：只有 PreviousPosition ∈ [Start,End) 才激活。
        if (previousPosition < window.startTime || previousPosition >= window.endTime) continue;

        const float endTime = std::min(window.endTime, duration);
        if (!(endTime > previousPosition)) continue;

        // 旋转与平移是 UE modifier 的两个独立分支（bWarpRotation / bWarpTranslation），
        // 旋转不能因为窗口没有平移而 early-out。执行层只支持 UE 默认 Slerp；其它
        // method 已在 BuildMontageWarpWindows 拒绝，这里对直接构造的窗口保守跳过。
        // hasTargetRotation=false 表示提交方没有提供朝向（旧 3 分量调用），不能把
        // 单位四元数当目标，保持原始根 yaw。
        if (outYawDelta && window.warpRotation && window.hasTargetRotation &&
            window.rotationMethod == "Slerp")
        {
            *outYawDelta = WarpedRootYawDelta(clip, window, previousPosition, currentPosition,
                endTime, duration, actorYaw);
        }

        const Eigen::Vector3f total = SampleRootMotionTranslation(clip, endTime) -
            SampleRootMotionTranslation(clip, previousPosition);
        // UE 先判断窗口内的总根位移；动画没有位移时走 AddTranslation 分支，
        // VL 的 traversal 片段都有根位移，因此只实现 SkewWarp 平移路径。
        if (NearlyZero(total, 2e-4f)) continue;

        const float insideTime = std::min(currentPosition, endTime);
        const Eigen::Vector3f delta = SampleRootMotionTranslation(clip, insideTime) -
            SampleRootMotionTranslation(clip, previousPosition);
        if (NearlyZero(delta, 2e-4f)) continue;

        Eigen::Vector3f extra = Eigen::Vector3f::Zero();
        if (currentPosition > endTime)
            extra = SampleRootMotionTranslation(clip, std::min(currentPosition, duration)) -
                SampleRootMotionTranslation(clip, endTime);

        Eigen::Vector3f targetModel = meshRotation.transpose() *
            (window.targetLocation - meshOrigin) - capsuleCenterInModel;
        // UE WarpPointAnimProvider=Bone：CachedOffsetFromWarpPoint = Root * WarpPoint^-1，
        // 在窗口 EndTime 采样一次（此处每 tick 重算同一时刻，结果相同），把“根相对该骨”
        // 的平移加到目标上，等价于 TargetTransform = offset * WarpTarget 后再换到模型空间。
        if (!window.warpPointBone.empty())
        {
            Eigen::Matrix4f rootModel;
            Eigen::Matrix4f warpPointModel;
            if (SampleJointModelByIndex(mesh, clip, 0, endTime, rootModel) &&
                SampleBoneModelTransform(mesh, clip, window.warpPointBone, endTime, warpPointModel))
            {
                const Eigen::Matrix4f offset = rootModel * warpPointModel.inverse();
                targetModel += offset.block<3, 1>(0, 3);
            }
        }
        // UE WarpPointAnimProvider=Static：WarpPointAnimTransform 是 Montage 资产里的
        // mesh/组件空间静态变换。UE 计算 RootTransform.GetRelativeTransform(WarpPoint)
        // （MotionWarpingComponent.cpp:245-251，RootMotionModifier.cpp:432-435），而
        // FTransform::GetRelativeTransform 的约定是 this*Other^-1
        // （TransformVectorized.h:549 / TransformNonVectorized.h:519），
        // 因此 offset = Root(EndTime) * StaticPose^-1；只消费其平移（与 Bone 路径一致）。
        // 采样失败/静态旋转退化为零四元数时保持纯 target，不静默造一个 offset。
        else if (window.hasStaticWarpPoint)
        {
            Eigen::Matrix4f rootModel;
            const float rotationNorm = window.staticWarpPointRotation.squaredNorm();
            if (rotationNorm > 1e-8f && std::isfinite(rotationNorm) &&
                SampleJointModelByIndex(mesh, clip, 0, endTime, rootModel))
            {
                Eigen::Matrix4f warpPointModel = Eigen::Matrix4f::Identity();
                warpPointModel.block<3, 3>(0, 0) =
                    window.staticWarpPointRotation.normalized().toRotationMatrix();
                warpPointModel.block<3, 1>(0, 3) = window.staticWarpPointLocation;
                const Eigen::Matrix4f offset = rootModel * warpPointModel.inverse();
                targetModel += offset.block<3, 1>(0, 3);
            }
        }
        if (window.subtractRemainingRootMotion)
            targetModel -= SampleRootMotionTranslation(clip, duration) -
                SampleRootMotionTranslation(clip, endTime);
        if (window.ignoreVertical) targetModel.y() = 0;

        // UE SkewWarp 以 FTransform::Identity 进入 WarpTranslation：CurrentLocation=0，
        // 目标已在上面的模型空间换算中表达；ExtraRootMotion 只补齐 EndTime 之后的部分。
        finalTranslation = WarpTranslationToTarget(Eigen::Vector3f::Zero(),
            Eigen::Quaternionf::Identity(), delta, total, targetModel) + extra;
    }
    return finalTranslation;
}

// 对应 UE URootMotionModifier_SkewWarp::WarpTranslation：根同步空间
// scale + shear，保留动作路径；不是逐轴终点比例。
Eigen::Vector3f WarpTranslationToTarget(const Eigen::Vector3f& currentLocation,
    const Eigen::Quaternionf& currentRotation, const Eigen::Vector3f& delta,
    const Eigen::Vector3f& total, const Eigen::Vector3f& targetLocation)
{
    if (delta.squaredNorm() <= 1e-12f || total.squaredNorm() <= 1e-12f)
        return Eigen::Vector3f::Zero();

    const Eigen::Vector3f currentToWorld = targetLocation - currentLocation;
    const Eigen::Vector3f rootDirection = total.normalized();

    const Eigen::Vector3f axisX = currentRotation * Eigen::Vector3f::UnitX();
    const Eigen::Vector3f axisY = currentRotation * Eigen::Vector3f::UnitY();
    const Eigen::Vector3f axisZ = currentRotation * Eigen::Vector3f::UnitZ();
    float best = std::abs(rootDirection.dot(axisX));
    Eigen::Vector3f up = axisZ;
    if (std::abs(rootDirection.dot(axisZ)) > best)
    {
        best = std::abs(rootDirection.dot(axisZ));
        up = axisX;
    }
    if (std::abs(rootDirection.dot(axisY)) > best)
        up = axisZ;
    const Eigen::Matrix3f sync = MakeFromXZ(rootDirection, up);

    const Eigen::Vector3f rootInSync = sync.transpose() * delta;
    const Eigen::Vector3f worldInSync = sync.transpose() * currentToWorld;
    const Eigen::Vector3f remainingInSync = sync.transpose() * total;
    if (remainingInSync.squaredNorm() <= 1e-12f)
        return Eigen::Vector3f::Zero();

    const Eigen::Vector3f worldNorm = worldInSync.normalized();
    const Eigen::Vector3f rootNorm = remainingInSync.normalized();

    Eigen::Vector3f flatWorld(worldNorm.x(), worldNorm.y(), 0);
    Eigen::Vector3f flatRoot(rootNorm.x(), rootNorm.y(), 0);
    float angleZ = 0;
    if (flatWorld.squaredNorm() > 1e-12f && flatRoot.squaredNorm() > 1e-12f)
    {
        flatWorld.normalize();
        flatRoot.normalize();
        angleZ = std::acos(std::clamp(flatWorld.dot(flatRoot), -1.0f, 1.0f));
        if (flatWorld.y() < 0) angleZ = -angleZ;
    }

    Eigen::Vector3f worldNoY(worldNorm.x(), 0, worldNorm.z());
    Eigen::Vector3f rootNoY(rootNorm.x(), 0, rootNorm.z());
    float angleY = 0;
    if (worldNoY.squaredNorm() > 1e-12f && rootNoY.squaredNorm() > 1e-12f)
    {
        worldNoY.normalize();
        rootNoY.normalize();
        angleY = std::acos(std::clamp(worldNoY.dot(rootNoY), -1.0f, 1.0f));
        if (worldNoY.z() < 0) angleY = -angleY;
    }

    const float projectedScale = worldInSync.dot(rootNorm) / remainingInSync.norm();
    if (!std::isfinite(projectedScale))
        return Eigen::Vector3f::Zero();

    // UE SetAxis(0, ...) 写的是矩阵列 0；shear 必须放在第 0 列。
    Eigen::Matrix3f scale = Eigen::Matrix3f::Identity();
    scale(0, 0) = projectedScale;
    Eigen::Matrix3f shearXY = Eigen::Matrix3f::Identity();
    shearXY(1, 0) = std::tan(angleZ);
    Eigen::Matrix3f shearXZ = Eigen::Matrix3f::Identity();
    shearXZ(2, 0) = std::tan(angleY);
    const Eigen::Vector3f warpedSync = scale * shearXY * shearXZ * rootInSync;
    return sync * warpedSync;
}
}
