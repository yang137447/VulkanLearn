#include "animationMontage.h"

#include "animationSequenceData.h"
#include "animationSkeleton.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

namespace VL::Animation
{
namespace
{
constexpr size_t MaxSections = 64;
constexpr size_t MaxNotifies = 64;
constexpr size_t MaxWarpWindows = 8;
constexpr size_t MaxNameLength = 64;
constexpr size_t MaxIdLength = 256;
// Montage 时间同样是秒；上限只防止明显异常的资产拖垮导入，不参与播放语义。
constexpr float MaxMontageLength = 600.0f;
// 与 schema/导出工具一致的时间容差（秒）。
constexpr double TimeTolerance = 1e-3;
// 普通 notify 只校验起点在 [0,length]；UE 允许浮点序列化的末帧误差。
constexpr double NotifyStartTolerance = 1e-5;

[[noreturn]] void Fail(
    std::string_view basePath, std::string_view field, const std::string& reason)
{
    throw std::runtime_error("Animation montage asset '" + std::string(basePath) +
        "': field \"" + std::string(field) + "\" " + reason);
}

char FoldAscii(char value)
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index)
    {
        if (FoldAscii(left[index]) != FoldAscii(right[index]))
        {
            return false;
        }
    }
    return true;
}

bool IsHexDigest(std::string_view value)
{
    if (value.size() != 64)
    {
        return false;
    }
    for (const char character : value)
    {
        if (!std::isxdigit(static_cast<unsigned char>(character)))
        {
            return false;
        }
    }
    return true;
}

bool IsKnownProvider(std::string_view value)
{
    // UE EWarpPointAnimProvider；GASP traversal 只使用 None/Bone，Static 数据层保留。
    return value == "None" || value == "Static" || value == "Bone";
}

bool IsKnownEasing(std::string_view value)
{
    // 与 tool/animation/import_gasp_montages.py 的 KNOWN_EASINGS 保持一致：
    // 已知 UE EEasingFunc 全部保留，第一切片只执行 Linear。
    static const std::set<std::string> known = {
        "Linear", "Step", "SinusoidalIn", "SinusoidalOut", "SinusoidalInOut",
        "EaseIn", "EaseOut", "EaseInOut", "ExpoIn", "ExpoOut", "ExpoInOut",
        "CircularIn", "CircularOut", "CircularInOut"};
    return known.find(std::string(value)) != known.end();
}

bool IsKnownLinkMethod(std::string_view value)
{
    // 空字符串表示未设置；已知 UE section link 方法原样保留，第一切片不执行。
    static const std::set<std::string> known = {"Absolute", "Relative", "Ratio"};
    return known.find(std::string(value)) != known.end();
}

bool IsKnownNotifyKind(std::string_view value)
{
    return value == "notify" || value == "notifyState";
}

std::string ReadRequiredString(const nlohmann::json& json, std::string_view field,
    std::string_view basePath, std::string_view fieldLabel = {})
{
    const std::string key(field);
    const std::string label = fieldLabel.empty() ? key : std::string(fieldLabel);
    if (!json.contains(key))
    {
        Fail(basePath, label, "is required");
    }
    if (!json.at(key).is_string())
    {
        Fail(basePath, label, "must be a string");
    }
    return json.at(key).get<std::string>();
}

std::string ReadOptionalString(const nlohmann::json& json, std::string_view field,
    std::string_view basePath, std::string_view fieldLabel = {})
{
    const std::string key(field);
    const std::string label = fieldLabel.empty() ? key : std::string(fieldLabel);
    if (!json.contains(key))
    {
        return {};
    }
    if (!json.at(key).is_string())
    {
        Fail(basePath, label, "must be a string");
    }
    return json.at(key).get<std::string>();
}

std::string ReadName(const nlohmann::json& json, std::string_view field,
    std::string_view basePath, std::string_view fieldLabel = {})
{
    const std::string label = fieldLabel.empty() ? std::string(field) : std::string(fieldLabel);
    const std::string name = ReadRequiredString(json, field, basePath, label);
    if (name.empty())
    {
        Fail(basePath, label, "must not be empty");
    }
    if (name.size() > MaxNameLength)
    {
        Fail(basePath, label,
            "exceeds " + std::to_string(MaxNameLength) + " characters");
    }
    return name;
}

float ReadFiniteNumber(const nlohmann::json& json, std::string_view field,
    std::string_view basePath, std::string_view fieldLabel = {})
{
    const std::string key(field);
    const std::string label = fieldLabel.empty() ? key : std::string(fieldLabel);
    if (!json.contains(key))
    {
        Fail(basePath, label, "is required");
    }
    if (!json.at(key).is_number())
    {
        Fail(basePath, label, "must be a number");
    }
    const float value = json.at(key).get<float>();
    if (!std::isfinite(value))
    {
        Fail(basePath, label, "must be finite");
    }
    return value;
}

bool ReadBool(const nlohmann::json& json, std::string_view field,
    std::string_view basePath, std::string_view fieldLabel = {})
{
    const std::string key(field);
    const std::string label = fieldLabel.empty() ? key : std::string(fieldLabel);
    if (!json.contains(key))
    {
        Fail(basePath, label, "is required");
    }
    if (!json.at(key).is_boolean())
    {
        Fail(basePath, label, "must be a boolean");
    }
    return json.at(key).get<bool>();
}

template <size_t Count>
std::array<float, Count> ReadFiniteArray(const nlohmann::json& json, std::string_view field,
    std::string_view basePath, std::string_view fieldLabel)
{
    const std::string key(field);
    const std::string label(fieldLabel);
    if (!json.contains(key))
    {
        Fail(basePath, label, "is required");
    }
    const auto& values = json.at(key);
    if (!values.is_array() || values.size() != Count)
    {
        Fail(basePath, label,
            "must be an array of " + std::to_string(Count) + " numbers");
    }
    std::array<float, Count> result{};
    for (size_t index = 0; index < Count; ++index)
    {
        const std::string elementField =
            label + "[" + std::to_string(index) + "]";
        if (!values.at(index).is_number())
        {
            Fail(basePath, elementField, "must be a number");
        }
        result[index] = values.at(index).get<float>();
        if (!std::isfinite(result[index]))
        {
            Fail(basePath, elementField, "must be finite");
        }
    }
    return result;
}

MontageStaticPose ParseStaticPose(const nlohmann::json& value, const std::string& field,
    std::string_view basePath)
{
    if (!value.is_object())
    {
        Fail(basePath, field, "must be an object");
    }
    MontageStaticPose pose;
    pose.location = ReadFiniteArray<3>(value, "location", basePath, field + ".location");
    pose.rotation = ReadFiniteArray<4>(value, "rotation", basePath, field + ".rotation");
    return pose;
}

MontageWarp ParseWarp(const nlohmann::json& json, const std::string& field,
    std::string_view basePath)
{
    if (!json.is_object())
    {
        Fail(basePath, field, "must be an object");
    }

    MontageWarp warp;
    warp.target = ReadName(json, "target", basePath, field + ".target");
    warp.provider = ReadRequiredString(json, "provider", basePath, field + ".provider");
    if (!IsKnownProvider(warp.provider))
    {
        Fail(basePath, field + ".provider",
            "must be one of None/Static/Bone; got \"" + warp.provider + "\"");
    }
    warp.bone = ReadOptionalString(json, "bone", basePath, field + ".bone");
    if (warp.provider == "Bone" && warp.bone.empty())
    {
        Fail(basePath, field + ".bone",
            "must not be empty when provider is \"Bone\"");
    }
    if (warp.bone.size() > MaxNameLength)
    {
        Fail(basePath, field + ".bone",
            "exceeds " + std::to_string(MaxNameLength) + " characters");
    }

    warp.subtractRemainingRootMotion = ReadBool(json, "subtractRemainingRootMotion",
        basePath, field + ".subtractRemainingRootMotion");
    warp.ignoreZAxis = ReadBool(json, "ignoreZAxis", basePath, field + ".ignoreZAxis");
    warp.warpRotation = ReadBool(json, "warpRotation", basePath, field + ".warpRotation");
    warp.rotationTimeMultiplier = ReadFiniteNumber(json, "rotationTimeMultiplier",
        basePath, field + ".rotationTimeMultiplier");
    if (warp.rotationTimeMultiplier <= 0)
    {
        Fail(basePath, field + ".rotationTimeMultiplier", "must be > 0");
    }

    // UE EMotionWarpRotationMethod：可选字段，缺省是 UE 默认值 Slerp。已知但非 Slerp
    // 的方法与未知字符串都原样保留，由执行层在 PlayMontage 时拒绝（与 Static/easing
    // 的“数据层保留、执行层拒绝”分层一致）。
    warp.rotationMethod = ReadOptionalString(json, "rotationMethod",
        basePath, field + ".rotationMethod");
    if (warp.rotationMethod.empty())
    {
        warp.rotationMethod = "Slerp";
    }
    if (warp.rotationMethod.size() > MaxNameLength)
    {
        Fail(basePath, field + ".rotationMethod",
            "exceeds " + std::to_string(MaxNameLength) + " characters");
    }

    warp.easing = ReadRequiredString(json, "easing", basePath, field + ".easing");
    if (!IsKnownEasing(warp.easing))
    {
        Fail(basePath, field + ".easing",
            "unknown UE easing function \"" + warp.easing + "\"");
    }

    if (warp.provider == "Static" && !json.contains("staticPose"))
    {
        Fail(basePath, field + ".staticPose",
            "is required when provider is \"Static\"");
    }
    if (json.contains("staticPose"))
    {
        warp.staticPose = ParseStaticPose(
            json.at("staticPose"), field + ".staticPose", basePath);
    }
    return warp;
}

std::vector<MontageSection> ParseSections(const nlohmann::json& json, float length,
    std::string_view basePath)
{
    if (!json.contains("sections"))
    {
        Fail(basePath, "sections", "is required");
    }
    const auto& sectionsJson = json.at("sections");
    if (!sectionsJson.is_array())
    {
        Fail(basePath, "sections", "must be an array");
    }
    if (sectionsJson.empty())
    {
        Fail(basePath, "sections", "must contain at least one section");
    }
    if (sectionsJson.size() > MaxSections)
    {
        Fail(basePath, "sections",
            "exceeds " + std::to_string(MaxSections) + " sections");
    }

    const double endLimit = static_cast<double>(length) + TimeTolerance;
    std::set<std::string> names;
    std::vector<MontageSection> sections;
    sections.reserve(sectionsJson.size());
    for (size_t index = 0; index < sectionsJson.size(); ++index)
    {
        const std::string prefix = "sections[" + std::to_string(index) + "]";
        const auto& sectionJson = sectionsJson.at(index);
        if (!sectionJson.is_object())
        {
            Fail(basePath, prefix, "must be an object");
        }

        MontageSection section;
        section.name = ReadName(sectionJson, "name", basePath, prefix + ".name");
        if (!names.insert(section.name).second)
        {
            Fail(basePath, prefix + ".name",
                "duplicates section name '" + section.name + "'");
        }
        section.startTime = ReadFiniteNumber(
            sectionJson, "startTime", basePath, prefix + ".startTime");
        section.endTime = ReadFiniteNumber(
            sectionJson, "endTime", basePath, prefix + ".endTime");
        if (section.startTime < 0)
        {
            Fail(basePath, prefix + ".startTime", "must be >= 0");
        }
        if (!(section.endTime > section.startTime))
        {
            Fail(basePath, prefix + ".endTime",
                "must be greater than startTime=" + std::to_string(section.startTime));
        }
        if (static_cast<double>(section.endTime) > endLimit)
        {
            Fail(basePath, prefix + ".endTime",
                "must not exceed montage length " + std::to_string(length) +
                " (+" + std::to_string(TimeTolerance) + " s)");
        }

        section.nextSection = ReadOptionalString(
            sectionJson, "nextSection", basePath, prefix + ".nextSection");
        if (section.nextSection.size() > MaxNameLength)
        {
            Fail(basePath, prefix + ".nextSection",
                "exceeds " + std::to_string(MaxNameLength) + " characters");
        }
        if (sectionJson.contains("nextTime"))
        {
            section.nextTime = ReadFiniteNumber(
                sectionJson, "nextTime", basePath, prefix + ".nextTime");
        }
        if (static_cast<double>(section.nextTime) < 0 ||
            static_cast<double>(section.nextTime) > endLimit)
        {
            Fail(basePath, prefix + ".nextTime",
                "must be within [0, montage length " + std::to_string(length) + "]");
        }

        section.linkMethod = ReadOptionalString(
            sectionJson, "linkMethod", basePath, prefix + ".linkMethod");
        if (!section.linkMethod.empty() && !IsKnownLinkMethod(section.linkMethod))
        {
            Fail(basePath, prefix + ".linkMethod",
                "unknown UE link method \"" + section.linkMethod + "\"");
        }
        sections.push_back(std::move(section));
    }

    // nextSection 允许前向/后向引用，因此先收集全部名字再校验引用。
    for (size_t index = 0; index < sections.size(); ++index)
    {
        const MontageSection& section = sections[index];
        if (!section.nextSection.empty() &&
            names.find(section.nextSection) == names.end())
        {
            Fail(basePath, "sections[" + std::to_string(index) + "].nextSection",
                "references unknown section '" + section.nextSection + "'");
        }
    }
    return sections;
}

std::vector<MontageNotify> ParseNotifies(const nlohmann::json& json, float length,
    std::vector<MontageWindow>& windows, std::string_view basePath)
{
    std::vector<MontageNotify> notifies;
    if (!json.contains("notifies"))
    {
        return notifies;
    }
    const auto& notifiesJson = json.at("notifies");
    if (!notifiesJson.is_array())
    {
        Fail(basePath, "notifies", "must be an array");
    }
    if (notifiesJson.size() > MaxNotifies)
    {
        Fail(basePath, "notifies",
            "exceeds " + std::to_string(MaxNotifies) + " entries");
    }

    const double endLimit = static_cast<double>(length) + TimeTolerance;
    notifies.reserve(notifiesJson.size());
    for (size_t index = 0; index < notifiesJson.size(); ++index)
    {
        const std::string prefix = "notifies[" + std::to_string(index) + "]";
        const auto& notifyJson = notifiesJson.at(index);
        if (!notifyJson.is_object())
        {
            Fail(basePath, prefix, "must be an object");
        }

        MontageNotify notify;
        notify.kind = ReadRequiredString(
            notifyJson, "kind", basePath, prefix + ".kind");
        if (!IsKnownNotifyKind(notify.kind))
        {
            Fail(basePath, prefix + ".kind",
                "must be \"notify\" or \"notifyState\"; got \"" + notify.kind + "\"");
        }
        notify.track = ReadName(notifyJson, "track", basePath, prefix + ".track");
        notify.name = ReadName(notifyJson, "name", basePath, prefix + ".name");
        notify.time = ReadFiniteNumber(notifyJson, "time", basePath, prefix + ".time");
        notify.duration = ReadFiniteNumber(
            notifyJson, "duration", basePath, prefix + ".duration");

        const double time = static_cast<double>(notify.time);
        const double duration = static_cast<double>(notify.duration);
        // UE notifyState 是任意子区间：duration 独立于片段/Montage 长度，可以越过末尾，
        // 原始值必须保留、不能裁剪。这里只约束起点；warp 窗口例外见下。
        const bool hasWarp = notifyJson.contains("warp");
        const double startLimit = static_cast<double>(length) +
            (hasWarp ? TimeTolerance : NotifyStartTolerance);
        if (time < 0 || time > startLimit)
        {
            Fail(basePath, prefix + ".time",
                "must be within [0, montage length " + std::to_string(length) + "]");
        }
        if (duration < 0)
        {
            Fail(basePath, prefix + ".duration", "must be >= 0");
        }

        if (hasWarp)
        {
            if (windows.size() >= MaxWarpWindows)
            {
                Fail(basePath, prefix + ".warp",
                    "exceeds " + std::to_string(MaxWarpWindows) + " warp windows");
            }
            // warp 窗口必须完整落在 Montage 内：MotionWarpRuntime 的 endTime 需要
            // 是片段内可采样的区间；普通 notifyState 不受此限制。
            if (!(duration > 0))
            {
                Fail(basePath, prefix + ".duration",
                    "must be > 0 for a MotionWarping window");
            }
            if (time + duration > endLimit)
            {
                Fail(basePath, prefix + ".duration",
                    "time+duration must not exceed montage length " +
                    std::to_string(length) + "; time+duration=" +
                    std::to_string(time + duration));
            }
            notify.warp = ParseWarp(notifyJson.at("warp"), prefix + ".warp", basePath);
            MontageWindow window;
            window.target = notify.warp->target;
            window.startTime = notify.time;
            window.endTime = static_cast<float>(time + duration);
            windows.push_back(std::move(window));
        }
        notifies.push_back(std::move(notify));
    }
    return notifies;
}
}

float ResolveMontageSectionLinkTime(const MontageSection& section)
{
    // UE FAnimLinkableElement 的三种 link 方法共用同一组 (SegmentBeginTime, SegmentLength)；
    // VL 的 section 自己就是链接段，因此取 section.startTime / section length。
    const float start = section.startTime;
    const float length = section.endTime - section.startTime;
    if (section.linkMethod.empty() || section.linkMethod == "Absolute")
        return section.nextTime;
    if (section.linkMethod == "Relative")
        return start + section.nextTime;
    if (section.linkMethod == "Ratio")
        return start + section.nextTime * length;
    throw std::invalid_argument("Animation montage section '" + section.name +
        "' uses unknown link method '" + section.linkMethod + "'");
}

Montage ParseMontage(const nlohmann::json& json, std::string_view basePath)
{
    if (!json.is_object())
    {
        throw std::runtime_error(
            "Animation montage asset '" + std::string(basePath) +
            "' must be a JSON object");
    }

    Montage montage;
    const std::string format = ReadRequiredString(json, "format", basePath);
    if (format != "vl.anim-montage.v1")
    {
        Fail(basePath, "format",
            "must be \"vl.anim-montage.v1\", got \"" + format + "\"");
    }

    montage.id = ReadRequiredString(json, "montage", basePath);
    if (montage.id.empty())
    {
        Fail(basePath, "montage", "must not be empty");
    }
    if (montage.id.size() > MaxIdLength)
    {
        Fail(basePath, "montage",
            "exceeds " + std::to_string(MaxIdLength) + " characters");
    }

    // 路径是唯一权威：Montage 不写 clip id 字段，clip id 由命中的 sidecar 派生。
    montage.sequencePath = ReadRequiredString(json, "sequence", basePath);
    if (montage.sequencePath.empty())
    {
        Fail(basePath, "sequence", "must not be empty");
    }
    if (json.contains("sequenceBlake3"))
    {
        montage.sequenceDigest = ReadRequiredString(json, "sequenceBlake3", basePath);
        if (!IsHexDigest(montage.sequenceDigest))
        {
            Fail(basePath, "sequenceBlake3",
                "must be a 64-character hex BLAKE3 digest");
        }
    }

    montage.slot = ReadName(json, "slot", basePath);
    montage.length = ReadFiniteNumber(json, "length", basePath);
    if (montage.length <= 0)
    {
        Fail(basePath, "length", "must be > 0");
    }
    if (montage.length > MaxMontageLength)
    {
        Fail(basePath, "length",
            "exceeds the " + std::to_string(static_cast<int>(MaxMontageLength)) +
            " second import budget");
    }

    montage.sections = ParseSections(json, montage.length, basePath);
    montage.notifies = ParseNotifies(json, montage.length, montage.windows, basePath);

    // source 只作追溯信息原样保留；不参与运行时身份（身份是文件摘要 + sequence 绑定）。
    if (json.contains("source"))
    {
        if (!json.at("source").is_object())
        {
            Fail(basePath, "source", "must be an object");
        }
        montage.sourceJson = json.at("source").dump();
    }
    return montage;
}

std::string BindMontage(const Montage& montage, const MontageImportContext& context,
    std::string_view assetPath)
{
    if (context.skeleton == nullptr || context.skeleton->id.empty())
    {
        Fail(assetPath, "skeleton",
            "requires a loaded Skeleton asset for montage '" + montage.id + "'");
    }
    const SkeletonAsset& skeleton = *context.skeleton;

    // sequence 路径必须按字符串命中本次已加载的 sidecar；不按 clip id 猜。
    const MontageSequenceBinding* binding = nullptr;
    for (const MontageSequenceBinding& candidate : context.sequences)
    {
        if (candidate.path == montage.sequencePath)
        {
            binding = &candidate;
            break;
        }
    }
    if (binding == nullptr || binding->sequence == nullptr)
    {
        Fail(assetPath, "sequence",
            "path '" + montage.sequencePath +
            "' is not a loaded sequence sidecar of this mesh asset");
    }
    const SequenceData& sequence = *binding->sequence;

    if (sequence.skeletonId != skeleton.id)
    {
        Fail(assetPath, "sequence",
            "sidecar skeleton '" + sequence.skeletonId +
            "' does not match loaded skeleton '" + skeleton.id + "'");
    }
    if (!sequence.skeletonDigest.empty() && !skeleton.sourceDigest.empty() &&
        !EqualsIgnoreCase(sequence.skeletonDigest, skeleton.sourceDigest))
    {
        Fail(assetPath, "sequence",
            "sidecar skeletonBlake3 '" + sequence.skeletonDigest +
            "' does not match skeleton file digest '" + skeleton.sourceDigest + "'");
    }

    if (!montage.sequenceDigest.empty())
    {
        if (binding->digest.empty())
        {
            Fail(assetPath, "sequenceBlake3",
                "declares a digest but the loaded sidecar has no digest to verify");
        }
        if (!EqualsIgnoreCase(montage.sequenceDigest, binding->digest))
        {
            Fail(assetPath, "sequenceBlake3",
                "digest '" + montage.sequenceDigest +
                "' does not match sidecar digest '" + binding->digest + "'");
        }
    }

    if (!skeleton.HasSlot(montage.slot))
    {
        Fail(assetPath, "slot",
            "'" + montage.slot + "' is not registered in skeleton '" +
            skeleton.id + "' slots");
    }

    for (size_t index = 0; index < montage.notifies.size(); ++index)
    {
        const MontageNotify& notify = montage.notifies[index];
        if (!notify.warp.has_value() || notify.warp->provider != "Bone")
        {
            continue;
        }
        if (skeleton.JointIndex(notify.warp->bone) < 0)
        {
            Fail(assetPath, "notifies[" + std::to_string(index) + "].warp.bone",
                "bone '" + notify.warp->bone + "' is not a joint in skeleton '" +
                skeleton.id + "'");
        }
    }

    // clip id 只从 sidecar 的 `sequence` 字段派生；它必须已在 SM skinning.clips[] 声明，
    // 保证 Montage 播放目标与本次导入的片段一一对应。
    if (std::find(context.clipIds.begin(), context.clipIds.end(), sequence.id) ==
        context.clipIds.end())
    {
        Fail(assetPath, "sequence",
            "derived clip id '" + sequence.id +
            "' is not declared in skinning.clips[]");
    }
    return sequence.id;
}
}
