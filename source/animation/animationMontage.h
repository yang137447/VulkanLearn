#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace VL::Animation
{
struct SkeletonAsset;
struct SequenceData;

// 一个 Montage section。时间单位秒、Montage-local。
// 执行层从 section 0 开始播放，段末尾（endTime）按 nextSection/linkMethod 决定下一段起点；
// nextSection 为空表示播放结束。链接时间换算见 ResolveMontageSectionLinkTime。
struct MontageSection
{
    std::string name;
    float startTime = 0;
    float endTime = 0;
    // 空字符串表示不连接；非空时必须命中同一 Montage 的某个 section 名。
    std::string nextSection;
    // 下一段起点的链接值；含义由 linkMethod 决定（见 ResolveMontageSectionLinkTime）。
    // 未设置 linkMethod 时按 Absolute，因此默认 0 表示时间轴起点，而不是“下一段起点”。
    float nextTime = 0;
    // UE link 方法：Absolute/Relative/Ratio（Ratio = UE EAnimLinkMethod::Proportional）；
    // "" 表示未设置，按 UE 默认的 Absolute 执行。
    std::string linkMethod;
};

// 把 section 的 nextSection 链接值换算成 Montage 时间轴上的绝对时间（秒）：
//   Absolute（"" 也按 UE 默认的 Absolute）: nextTime 即绝对时间；
//   Relative: section.startTime + nextTime；
//   Ratio（UE EAnimLinkMethod::Proportional）:
//       section.startTime + nextTime * (section.endTime - section.startTime)。
// 公式来自 UE FAnimLinkableElement 的 GetTimeFromAbsolute/GetTimeFromRelative/
// GetTimeFromProportional（Engine\Private\Animation\AnimLinkableElement.cpp:148-194，
// EAnimLinkMethod 定义在 AnimLinkableElement.h:14-26）。UE 的 SegmentBeginTime/SegmentLength
// 在 VL 数据里没有独立 slot segment 表，按当前 section 的 startTime/length 取值。
// 注意：UE 5.8 的 montage runtime 已不再保留 NextLink/NextTime，段结束时直接跳到下一段
// 的 section 起点（AnimMontage.cpp:2722-2728）；这里执行的是数据层保留的 NextLink 语义。
// 未知 linkMethod 抛 std::invalid_argument（执行层必须先拒绝，不能静默按 Absolute 处理）。
float ResolveMontageSectionLinkTime(const MontageSection& section);

// Static provider 的静态 warppoint transform：location 单位厘米，
// rotation 为 UE 四元数 (x,y,z,w)。它是动画/组件空间（mesh space）的静态变换，
// 执行层在窗口 EndTime 采样根后按 offset = Root * StaticPose^-1 求偏移（见 MotionWarpWindow）。
struct MontageStaticPose
{
    std::array<float, 3> location{0.0f, 0.0f, 0.0f};
    std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
};

// MotionWarping notifyState 的静态 modifier 定义。数据所有权在本结构；
// 不包含曲线或窗口 EndTime 预采样值——曲线只存在 Sequence sidecar 中。
struct MontageWarp
{
    // C# 动态 target 的键（不要求唯一：同名窗口共享一个世界位置）。
    std::string target;
    // UE EWarpPointAnimProvider：None/Static/Bone；执行层三种都支持。Static 需要 staticPose。
    std::string provider;
    // provider=Bone 时必须非空，并已在 Skeleton.joints 中注册。
    std::string bone;
    bool subtractRemainingRootMotion = false;
    bool ignoreZAxis = false;
    // UE bWarpRotation / WarpRotationTimeMultiplier：旋转 warp 的开关与时间缩放。
    // 目标旋转不来自 Montage 资产，而由 C# PlayMontage 的 target 提交。
    bool warpRotation = false;
    float rotationTimeMultiplier = 1.0f;
    // UE EMotionWarpRotationMethod 的枚举名（Slerp/SlerpWithClampedRate/ConstantRate/
    // Scale）。数据层按字符串原样保留；执行层当前只支持默认的 Slerp（最短路径球面
    // 插值），其它值在 PlayMontage 时拒绝并给诊断。
    std::string rotationMethod = "Slerp";
    // UE EEasingFunc 已知值原样保留；执行层只执行 Linear。
    std::string easing = "Linear";
    // 仅 provider=Static 必需；执行层在 EndTime 采样根并计算 offset = Root * staticPose^-1。
    std::optional<MontageStaticPose> staticPose;
};

// 一条 Montage notify。time/duration 单位秒、Montage-local；duration==0 为瞬时。
// warp 只有 MotionWarping notifyState 才有，缺省表示普通 notify。
struct MontageNotify
{
    // UE AnimNotifyEvent 类型："notify" 或 "notifyState"。
    std::string kind;
    std::string track;
    std::string name;
    float time = 0;
    float duration = 0;
    std::optional<MontageWarp> warp;
};

// 由 warp notify 派生的播放窗口：target + [startTime,endTime]（秒）。
// 顺序与 notifies 一致，不排序、不去重；同名 target 可以出现多次。
struct MontageWindow
{
    std::string target;
    float startTime = 0;
    float endTime = 0;
};

// AnimMontage 的数据层表示。所有字符串/数组归本结构所有；Montage 不复制骨骼、
// 不复制曲线、不持有播放时钟或 notify 回调。身份字段：
//   id              Montage JSON 的 `montage`（同一 SM 内唯一，catalog/C# 播放键）
//   sequencePath    指向 Sequence sidecar 的资源相对路径（唯一权威，不写 clip id）
//   sequenceDigest  可选的 `sequenceBlake3` 声明（64 hex；空表示未声明）
//   derivedClipId   由 loader 从命中的 sidecar `sequence` 字段派生，仅供运行时诊断/播放
//   sourcePath      Montage 自身资源相对路径（loader 填充；用于按路径索引）
//   sourceDigest    Montage 文件 BLAKE3-256 hex（loader 读取字节后填充）
struct Montage
{
    std::string id;
    std::string sequencePath;
    std::string sequenceDigest;
    std::string derivedClipId;
    std::string slot;
    float length = 0;
    std::vector<MontageSection> sections;
    std::vector<MontageNotify> notifies;
    std::vector<MontageWindow> windows;
    std::string sourcePath;
    std::string sourceJson;
    std::string sourceDigest;
};

// basePath 只用于诊断与资源路径展示；解析器不读文件、不计算摘要、不接触骨架/Sequence。
// 结构/数值校验：format、id、slot、length、section 时间与 next 引用、notify 类型/
// 时间窗口、warp provider/easing/staticPose、窗口数量。普通 notify/notifyState 的
// duration 独立于 Montage 长度（UE 语义，可越过末尾，原值保留不裁剪）；带 warp 的
// 窗口必须完整落在 [0,length+1e-3] 内。未知枚举与越界一律抛出带 basePath 与字段
// （含数组索引）的异常。已知但执行层尚不执行的值（非 Linear easing、非 Slerp
// rotationMethod）只保留不拒绝，由 PlayMontage 装配期显式报错。
Montage ParseMontage(const nlohmann::json& json, std::string_view basePath);

// 本次 SM 导入中某个 clip 已加载的 Sequence sidecar。path 与 SM
// skinning.clips[].sequence 逐字相同；sequence/digest 由 loader 拥有，生命周期覆盖绑定调用。
struct MontageSequenceBinding
{
    std::string path;
    const SequenceData* sequence = nullptr;
    std::string digest;
};

// Montage 导入上下文；skeleton/sequences/clipIds 均借用，Montage 不取得所有权。
struct MontageImportContext
{
    const SkeletonAsset* skeleton = nullptr;
    std::vector<MontageSequenceBinding> sequences;
    std::vector<std::string> clipIds;
};

// 跨文件绑定校验，成功后返回 sidecar 的 `sequence` 字段作为 derivedClipId：
//   - sequence 路径必须命中本次已加载的 sidecar（按路径，不是 clip id）；
//   - sequenceBlake3（若声明）必须等于 sidecar 文件摘要；
//   - sidecar 的 skeleton id/摘要必须等于当前 SK；
//   - slot 必须已在 SK slots 注册；
//   - provider=Bone 的骨名必须存在于 SK joints；
//   - 派生的 clip id 必须已在 SM skinning.clips[] 声明。
// 不采样曲线、不生成窗口曲线值；失败抛带 montage 路径与字段名的异常。
std::string BindMontage(const Montage& montage, const MontageImportContext& context,
    std::string_view assetPath);
}
