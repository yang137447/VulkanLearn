#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace VL::Animation
{
struct SkeletonAsset;

// 一条 float 曲线。times 为秒、clip-local（允许保留 [0,duration] 外的 pre/post roll）；
// values 与 times 等长并逐点对应。数据所有权在 SequenceData，Montage 不复制。
struct FloatCurve
{
    std::string name;
    std::vector<float> times;
    std::vector<float> values;
};

// 序列级 notify。time/duration 为秒、clip-local；duration==0 表示瞬时 notify。
// UE notifyState 是任意子区间，duration 独立于序列长度，可以越过片段末尾。
// 这里保留作者原始区间，不做按片段时长的裁剪；EndTime() 可能大于 SequenceData::duration，
// 运行时负责按实际播放范围 clamp 或中断。
struct SequenceNotify
{
    std::string track;
    std::string name;
    float time = 0;
    float duration = 0;

    // 只读派生值，不写回资产；不 clamp 到序列时长。
    float EndTime() const { return time + duration; }
};

// Sequence sidecar 是曲线与序列级 notify 的唯一运行时来源；骨骼 TRS 仍由 FBX 承载。
// 时间语义：秒、clip-local；duration 是片段时长，采样/notify 都用同一时间轴。
struct SequenceData
{
    // sidecar 的 `sequence` 字段：clip id（由 SM skinning.clips[].id 命中）。
    std::string id;
    std::string skeletonId;
    std::string skeletonDigest;
    float duration = 0;
    int frameRateNumerator = 0;
    int frameRateDenominator = 1;
    int numFrames = 0;
    int numKeys = 0;
    float rateScale = 1;
    bool enableRootMotion = false;
    bool forceRootLock = false;
    std::string rootMotionRootLock = "RefPose";
    std::string additiveAnimType = "None";
    // 作者轨道名，导入期用于对照 SK 关节；不参与采样。
    std::vector<std::string> tracks;
    std::vector<FloatCurve> curves;
    std::vector<SequenceNotify> notifies;
    // syncMarkers / source 第一切片原样保留，不执行；dump 后的 JSON 文本。
    std::string syncMarkersJson;
    std::string sourceJson;
    // sidecar 文件 BLAKE3-256 hex；由 loader 在读取文件字节后写入。
    std::string sourceDigest;
};

// basePath 只用于诊断与资源路径展示；解析器不读文件、不计算摘要。
SequenceData ParseSequence(const nlohmann::json& json, std::string_view basePath);

// 曲线名查找大小写不敏感；缺失返回 nullptr。
const FloatCurve* FindCurve(const SequenceData& sequence, std::string_view name);
bool HasCurve(const SequenceData& sequence, std::string_view name);

// 秒、clip-local、键间线性、区间外 clamp 到首/末值、单键为常量。
// 该函数是 C# CurveValue 的参考实现：缺失曲线返回 nullopt，非有限时间抛 invalid_argument。
std::optional<float> SampleFloatCurve(
    const SequenceData& sequence, std::string_view name, float time);
// 需要把“曲线缺失”当成硬错误的调用点使用该重载。
float SampleFloatCurveOrThrow(
    const SequenceData& sequence, std::string_view name, float time);

// 导入期校验：skeleton id/摘要必须匹配；每条曲线必须已在 SK curves 注册；
// tracks（若给）必须都是 SK 关节。未注册曲线是拒绝，不做静默空曲线降级。
void ValidateSequenceAgainstSkeleton(
    const SequenceData& sequence, const SkeletonAsset& skeleton, std::string_view assetPath);
}
