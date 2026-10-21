#pragma once

#include <string>
#include <vector>
#include <Eigen/Core>
#include <Eigen/Geometry>

namespace VL::Animation
{
struct Clip;
struct SkeletonMesh;

// 与 SkeletalPlayback 共用同一份根轨道采样：区间位移 = Root(end) - Root(start)，
// 模型空间、厘米，不含 Actor/组件朝向。实现位于 motionWarping.cpp。
Eigen::Vector3f SampleRootMotionTranslation(const Clip& clip, float time);

// 与 SkeletalPlayback 的层级求值同源的只读采样：从根沿父链拼到目标骨，返回模型空间
// 变换；没有轨道的关节使用绑定局部变换（等价 SampleLocals 的 rest 回退）。
// 只服务 UE WarpPointAnimProvider=Bone 的目标空间换算，不推进时钟、不改姿态。
bool SampleBoneModelTransform(const SkeletonMesh& mesh, const Clip& clip,
    const std::string& boneName, float time, Eigen::Matrix4f& outTransform);

// 一个 Montage warp target：世界空间胶囊中心 + 可选世界旋转。
// location 与 MotionWarpWindow::targetLocation 同义（胶囊中心，不是 Mesh 原点/脚底）。
// rotation 对应 UE FMotionWarpingTarget::Rotation，表示目标 Actor 世界朝向；VL 的
// 根旋转通道只有 yaw，因此只消费它的 Y-up yaw，pitch/roll 原样保留但不执行。
// hasRotation=false 时即使窗口 warpRotation=true 也不做旋转 warp：旧的 C# 3 分量
// 调用没有朝向信息，不能把单位四元数当成“面向世界 +X”。
struct MotionWarpTarget
{
    Eigen::Vector3f location = Eigen::Vector3f::Zero();
    bool hasRotation = false;
    Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
};

// UE URootMotionModifier_SkewWarp 的 warp 参数。targetLocation 是 VL 世界空间
// 目标，约定为胶囊中心（不是 Mesh 原点/脚底）；窗口时间与片段本地时间同单位。
// C# 只提交 target/window，不做任何坐标或 warp 换算；提交方必须用“障碍顶面 +
// 胶囊半高 90”表达胶囊中心，否则原生会把根位移拉向顶面/地面。
struct MotionWarpWindow
{
    std::string targetName;
    Eigen::Vector3f targetLocation = Eigen::Vector3f::Zero();
    float startTime = 0;
    float endTime = 0;
    // UE bSubtractRemainingRootMotion：把 EndTime 之后剩余根位移/根旋转从目标中扣除，
    // 使角色在片段结束时（而不是 notify 结束时）到达目标。
    bool subtractRemainingRootMotion = false;
    // UE bIgnoreZAxis 的 VL Y-up 等价：目标高度归零，垂直 delta 原样保留。
    // 旋转通道只消费 yaw，因此该开关不影响旋转 warp。
    bool ignoreVertical = false;
    // 窗口内的 warp point 偏移来源（UE EWarpPointAnimProvider）：
    //   warpPointBone 非空 = Bone，在 EndTime 采样该骨（Hurdle 的 FrontLedge 窗口用
    //     attach：固定在障碍前沿上方 100 cm 的 helper 骨，使该骨而不是根落在 targetLocation）；
    //   hasStaticWarpPoint = Static，使用 Montage 资产里的静态 mesh 空间变换；
    //   两者都缺省 = None。
    // Static/Bone 的 offset 都是 Root * WarpPoint^-1（UE FTransform::GetRelativeTransform
    // 是 this*Other^-1，见 Math/TransformVectorized.h:549 与
    // TransformNonVectorized.h:519 的注释；调用点在 MotionWarpingComponent.cpp:245-251 与
    // RootMotionModifier.cpp:426-454）。
    // 执行层只消费 offset 的平移：TargetTransform = offset * TargetGame，VL 用
    // targetModel += offset.translation 表达同一“根落到目标”的平移通道；offset 的旋转
    // 不参与现有旋转 warp（旋转仍只由 targetRotation 决定）。
    std::string warpPointBone;
    // UE WarpPointAnimProvider=Static 的静态 warp point：mesh/组件空间、厘米。
    bool hasStaticWarpPoint = false;
    Eigen::Vector3f staticWarpPointLocation = Eigen::Vector3f::Zero();
    Eigen::Quaternionf staticWarpPointRotation = Eigen::Quaternionf::Identity();

    // ---- UE URootMotionModifier_Warp 的旋转字段（bWarpRotation 分支）----
    // bWarpRotation：窗口内是否消费 targetRotation。
    bool warpRotation = false;
    // WarpRotationTimeMultiplier：旋转时间按窗口剩余时长缩放（>0，数据层已校验）。
    float rotationTimeMultiplier = 1.0f;
    // EMotionWarpRotationMethod 的原名字符串；执行层当前只支持 "Slerp"（UE 默认，
    // 最短路径球面插值）。其它已知/未知值在 BuildMontageWarpWindows 拒绝并给诊断。
    std::string rotationMethod = "Slerp";
    // 目标世界旋转。来自提交的 target；缺省时本窗口不做旋转 warp。
    bool hasTargetRotation = false;
    Eigen::Quaternionf targetRotation = Eigen::Quaternionf::Identity();
};

// 通用 modifier 队列：窗口按提交顺序保存（= UE Montage Notifies 顺序）；每个 tick 仅
// PreviousPosition ∈ [Start,End) 的窗口激活。每个激活窗口都从动画重新取
// ExtractRootMotion(PreviousPosition, EndTime)，覆盖根平移后再加 EndTime 之后的
// ExtraRootMotion，等价 UE URootMotionModifier_SkewWarp::ProcessRootMotion 的逐个
// 应用语义（同一 tick 多个窗口激活时最后一个生效）。平移目标是胶囊中心世界位置，
// 在应用时用可视 Mesh 变换换到模型空间，并按窗口处理 subtractRemaining/ignoreVertical
// 与 WarpPointAnimProvider=Bone/Static 偏移。旋转目标（可选）按同一个 UE modifier 的
// WarpRotation 公式单独写 yaw 通道，不参与平移空间换算。结果是
// SkeletalPlayback::RootMotionDelta / RootMotionYawDelta 的唯一来源；C# 只消费这两个
// 根运动通道并交给 Jolt/Actor 旋转，不参与窗口排序、目标换算或 warp 数学。
class MotionWarpRuntime
{
public:
    void SetWindows(std::vector<MotionWarpWindow> windows);
    void Clear();
    bool Empty() const { return windows.empty(); }
    const std::vector<MotionWarpWindow>& Windows() const { return windows; }
    // modelDelta 是上一 tick 的根轨道位移（模型空间）。meshTransform 是可视 Mesh 的
    // 世界矩阵（含视觉旋转/平移偏移），actorPosition 是胶囊中心世界位置，
    // actorYaw 是 Actor（胶囊）世界 yaw（弧度）。mesh/clip 只用于在窗口 EndTime
    // 采样 WarpPointAnimProvider 骨骼与根旋转；不写姿态。
    // outYawDelta 非空时：若本 tick 至少有一个激活窗口执行旋转 warp，就写入本 tick
    // 的模型空间根 yaw 增量（弧度）；没有旋转窗口时保持调用方传入值不变（调用方应先
    // 放入原始根 yaw）。平移结果始终由返回值给出，两者互不覆盖。
    Eigen::Vector3f Apply(const SkeletonMesh& mesh, const Clip& clip,
        float previousPosition, float currentPosition,
        const Eigen::Vector3f& modelDelta, const Eigen::Matrix4f& meshTransform,
        const Eigen::Vector3f& actorPosition, float actorYaw = 0.0f,
        float* outYawDelta = nullptr) const;
    // 诊断读数：name:waiting/active/done，逗号分隔。
    std::string DebugState(float previousPosition) const;

private:
    std::vector<MotionWarpWindow> windows;
};

// UE SkewWarp 的根同步空间 scale+shear 平移算法。输入/输出共享同一坐标系
// （X 前、Y 右、Z 上）；保留垂直分量与动作路径，不是逐轴终点比例。
Eigen::Vector3f WarpTranslationToTarget(const Eigen::Vector3f& currentLocation,
    const Eigen::Quaternionf& currentRotation, const Eigen::Vector3f& delta,
    const Eigen::Vector3f& total, const Eigen::Vector3f& targetLocation);
}
