#include "animationSequenceData.h"

#include "animationSkeleton.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>

namespace VL::Animation
{
namespace
{
constexpr size_t MaxCurves = 64;
constexpr size_t MaxCurveKeys = 100000;
constexpr size_t MaxNotifies = 4096;
constexpr size_t MaxTracks = 4096;
constexpr size_t MaxNameLength = 64;
constexpr float MinCurveTime = -1.0e3f;
constexpr float MaxCurveTime = 1.0e3f;
constexpr double FrameDurationTolerance = 1e-3;

[[noreturn]] void Fail(
    std::string_view basePath, std::string_view field, const std::string& reason)
{
    throw std::runtime_error("Animation sequence asset '" + std::string(basePath) +
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

std::string LowerAscii(std::string_view value)
{
    std::string result(value);
    for (char& character : result)
    {
        character = FoldAscii(character);
    }
    return result;
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

std::string ReadRequiredString(
    const nlohmann::json& json, std::string_view field, std::string_view basePath,
    std::string_view fieldLabel = {})
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

std::string ReadName(const nlohmann::json& value, std::string_view field,
    std::string_view basePath)
{
    if (!value.is_string())
    {
        Fail(basePath, field, "must be a string");
    }
    const std::string name = value.get<std::string>();
    if (name.empty())
    {
        Fail(basePath, field, "must not be empty");
    }
    if (name.size() > MaxNameLength)
    {
        Fail(basePath, field,
            "exceeds " + std::to_string(MaxNameLength) + " characters");
    }
    return name;
}

bool ReadBool(
    const nlohmann::json& json, std::string_view field, std::string_view basePath,
    std::string_view fieldLabel = {})
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

float ReadFiniteNumber(
    const nlohmann::json& json, std::string_view field, std::string_view basePath,
    std::string_view fieldLabel = {})
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

int ReadNonNegativeInt(
    const nlohmann::json& json, std::string_view field, std::string_view basePath,
    std::string_view fieldLabel = {})
{
    const std::string key(field);
    const std::string label = fieldLabel.empty() ? key : std::string(fieldLabel);
    if (!json.contains(key))
    {
        Fail(basePath, label, "is required");
    }
    if (!json.at(key).is_number_integer())
    {
        Fail(basePath, label, "must be an integer");
    }
    const int64_t value = json.at(key).get<int64_t>();
    if (value < 0 || value > std::numeric_limits<int>::max())
    {
        Fail(basePath, label, "must be in [0, " +
            std::to_string(std::numeric_limits<int>::max()) + "]");
    }
    return static_cast<int>(value);
}

int ReadPositiveInt(
    const nlohmann::json& json, std::string_view field, std::string_view basePath,
    std::string_view fieldLabel = {})
{
    const std::string label = fieldLabel.empty() ? std::string(field) : std::string(fieldLabel);
    const int value = ReadNonNegativeInt(json, field, basePath, label);
    if (value <= 0)
    {
        Fail(basePath, label, "must be positive");
    }
    return value;
}

void ValidateRootMotion(
    const nlohmann::json& json, SequenceData& sequence, std::string_view basePath)
{
    if (!json.contains("rootMotion"))
    {
        return;
    }
    const auto& rootMotion = json.at("rootMotion");
    if (!rootMotion.is_object())
    {
        Fail(basePath, "rootMotion", "must be an object");
    }
    if (rootMotion.contains("enableRootMotion"))
    {
        sequence.enableRootMotion = ReadBool(
            rootMotion, "enableRootMotion", basePath, "rootMotion.enableRootMotion");
    }
    if (rootMotion.contains("forceRootLock"))
    {
        sequence.forceRootLock = ReadBool(
            rootMotion, "forceRootLock", basePath, "rootMotion.forceRootLock");
    }
    if (rootMotion.contains("rootMotionRootLock"))
    {
        const std::string value = ReadRequiredString(
            rootMotion, "rootMotionRootLock", basePath, "rootMotion.rootMotionRootLock");
        // UE ERootMotionRootLock 的已知值；未知枚举无法映射，直接拒绝。
        static const std::set<std::string> known = {
            "RefPose", "AnimFirstFrame", "Zero"};
        if (known.find(value) == known.end())
        {
            Fail(basePath, "rootMotion.rootMotionRootLock",
                "unknown UE root motion lock \"" + value + "\"");
        }
        sequence.rootMotionRootLock = value;
    }
    if (rootMotion.contains("additiveAnimType"))
    {
        const std::string value = ReadRequiredString(
            rootMotion, "additiveAnimType", basePath, "rootMotion.additiveAnimType");
        // 数据层保留已知 additive 值（第一切片不播放），未知值拒绝。
        static const std::set<std::string> known = {
            "None", "LocalSpace", "LocalSpaceBase", "MeshSpace",
            "RotationOffsetMeshSpace", "AAT_None", "AAT_LocalSpaceBase",
            "AAT_RotationOffsetMeshSpace"};
        if (known.find(value) == known.end())
        {
            Fail(basePath, "rootMotion.additiveAnimType",
                "unknown UE additive animation type \"" + value + "\"");
        }
        sequence.additiveAnimType = value;
    }
}

void ValidateSampling(
    const nlohmann::json& json, std::string_view basePath)
{
    if (!json.contains("sampling"))
    {
        return;
    }
    const auto& sampling = json.at("sampling");
    if (!sampling.is_object())
    {
        Fail(basePath, "sampling", "must be an object");
    }
    const std::string interpolation =
        ReadRequiredString(sampling, "interpolation", basePath, "sampling.interpolation");
    if (interpolation != "linear")
    {
        Fail(basePath, "sampling.interpolation",
            "must be \"linear\"; got \"" + interpolation + "\"");
    }
    const std::string outside = ReadRequiredString(
        sampling, "outside", basePath, "sampling.outside");
    if (outside != "clamp")
    {
        Fail(basePath, "sampling.outside",
            "must be \"clamp\"; got \"" + outside + "\"");
    }
}

void ValidateTracks(
    const nlohmann::json& json, SequenceData& sequence, std::string_view basePath)
{
    if (!json.contains("tracks"))
    {
        return;
    }
    const auto& tracks = json.at("tracks");
    if (!tracks.is_array())
    {
        Fail(basePath, "tracks", "must be an array");
    }
    if (tracks.size() > MaxTracks)
    {
        Fail(basePath, "tracks", "exceeds " + std::to_string(MaxTracks) + " entries");
    }
    sequence.tracks.reserve(tracks.size());
    for (size_t index = 0; index < tracks.size(); ++index)
    {
        const std::string field = "tracks[" + std::to_string(index) + "]";
        sequence.tracks.push_back(ReadName(tracks.at(index), field, basePath));
    }
}

void ValidateFloatCurves(
    const nlohmann::json& json, SequenceData& sequence, std::string_view basePath)
{
    if (!json.contains("floatCurveKeys"))
    {
        return;
    }
    const auto& curves = json.at("floatCurveKeys");
    if (!curves.is_array())
    {
        Fail(basePath, "floatCurveKeys", "must be an array");
    }
    if (curves.size() > MaxCurves)
    {
        Fail(basePath, "floatCurveKeys",
            "exceeds " + std::to_string(MaxCurves) + " curves");
    }

    std::set<std::string> curveNames;
    sequence.curves.reserve(curves.size());
    for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex)
    {
        const auto& curveJson = curves.at(curveIndex);
        const std::string prefix = "floatCurveKeys[" + std::to_string(curveIndex) + "]";
        if (!curveJson.is_object())
        {
            Fail(basePath, prefix, "must be an object");
        }
        if (!curveJson.contains("name"))
        {
            Fail(basePath, prefix + ".name", "is required");
        }
        FloatCurve curve;
        curve.name = ReadName(curveJson.at("name"), prefix + ".name", basePath);
        if (!curveNames.insert(LowerAscii(curve.name)).second)
        {
            Fail(basePath, prefix + ".name",
                "duplicates curve name '" + curve.name + "' case-insensitively");
        }

        if (!curveJson.contains("times") || !curveJson.at("times").is_array())
        {
            Fail(basePath, prefix + ".times", "must be an array");
        }
        if (!curveJson.contains("values") || !curveJson.at("values").is_array())
        {
            Fail(basePath, prefix + ".values", "must be an array");
        }
        const auto& times = curveJson.at("times");
        const auto& values = curveJson.at("values");
        if (times.size() != values.size())
        {
            Fail(basePath, prefix,
                "times and values must have equal length; times=" +
                std::to_string(times.size()) + " values=" +
                std::to_string(values.size()));
        }
        if (times.empty())
        {
            Fail(basePath, prefix, "must contain at least one key");
        }
        if (times.size() > MaxCurveKeys)
        {
            Fail(basePath, prefix,
                "exceeds " + std::to_string(MaxCurveKeys) + " keys");
        }

        curve.times.reserve(times.size());
        curve.values.reserve(values.size());
        for (size_t key = 0; key < times.size(); ++key)
        {
            const std::string timeField =
                prefix + ".times[" + std::to_string(key) + "]";
            if (!times.at(key).is_number())
            {
                Fail(basePath, timeField, "must be a number");
            }
            const float time = times.at(key).get<float>();
            if (!std::isfinite(time))
            {
                Fail(basePath, timeField, "must be finite");
            }
            if (time < MinCurveTime || time > MaxCurveTime)
            {
                Fail(basePath, timeField,
                    "must be within [-1e3, 1e3] seconds");
            }
            if (key > 0 && !(time > curve.times.back()))
            {
                Fail(basePath, timeField,
                    "must be strictly greater than the previous key time");
            }
            curve.times.push_back(time);

            const std::string valueField =
                prefix + ".values[" + std::to_string(key) + "]";
            if (!values.at(key).is_number())
            {
                Fail(basePath, valueField, "must be a number");
            }
            const float value = values.at(key).get<float>();
            if (!std::isfinite(value))
            {
                Fail(basePath, valueField, "must be finite");
            }
            curve.values.push_back(value);
        }
        sequence.curves.push_back(std::move(curve));
    }
}

void ValidateNotifies(
    const nlohmann::json& json, SequenceData& sequence, std::string_view basePath)
{
    if (!json.contains("notifies"))
    {
        return;
    }
    const auto& notifies = json.at("notifies");
    if (!notifies.is_array())
    {
        Fail(basePath, "notifies", "must be an array");
    }
    if (notifies.size() > MaxNotifies)
    {
        Fail(basePath, "notifies",
            "exceeds " + std::to_string(MaxNotifies) + " entries");
    }
    // 只限制 notify 的起点必须落在可播放区间；notifyState 的 duration 是独立子区间，
    // 可以越过片段末尾（UE 真实资产存在 end > sequence duration），因此不裁剪区间。
    const double latestStart = static_cast<double>(sequence.duration) + 1e-5;
    sequence.notifies.reserve(notifies.size());
    for (size_t index = 0; index < notifies.size(); ++index)
    {
        const auto& notifyJson = notifies.at(index);
        const std::string prefix = "notifies[" + std::to_string(index) + "]";
        if (!notifyJson.is_object())
        {
            Fail(basePath, prefix, "must be an object");
        }
        if (!notifyJson.contains("track"))
        {
            Fail(basePath, prefix + ".track", "is required");
        }
        if (!notifyJson.contains("name"))
        {
            Fail(basePath, prefix + ".name", "is required");
        }

        SequenceNotify notify;
        notify.track = ReadName(notifyJson.at("track"), prefix + ".track", basePath);
        notify.name = ReadName(notifyJson.at("name"), prefix + ".name", basePath);
        notify.time = ReadFiniteNumber(notifyJson, "time", basePath, prefix + ".time");
        notify.duration = ReadFiniteNumber(
            notifyJson, "duration", basePath, prefix + ".duration");

        const double time = static_cast<double>(notify.time);
        const double duration = static_cast<double>(notify.duration);
        if (time < 0 || time > latestStart)
        {
            Fail(basePath, prefix + ".time",
                "must be within [0, sequence duration]; time=" +
                std::to_string(time) + " duration=" +
                std::to_string(sequence.duration));
        }
        if (duration < 0)
        {
            Fail(basePath, prefix + ".duration", "must be >= 0");
        }
        // 故意不校验 time+duration <= sequence.duration：原始区间原样保留，
        // EndTime() 可能大于片段时长，由运行时按播放范围 clamp/中断。
        sequence.notifies.push_back(std::move(notify));
    }
}
}

SequenceData ParseSequence(const nlohmann::json& json, std::string_view basePath)
{
    if (!json.is_object())
    {
        throw std::runtime_error(
            "Animation sequence asset '" + std::string(basePath) +
            "' must be a JSON object");
    }

    SequenceData sequence;
    const std::string format = ReadRequiredString(json, "format", basePath);
    if (format != "vl.anim-sequence.v1")
    {
        Fail(basePath, "format",
            "must be \"vl.anim-sequence.v1\", got \"" + format + "\"");
    }
    sequence.id = ReadRequiredString(json, "sequence", basePath);
    if (sequence.id.empty())
    {
        Fail(basePath, "sequence", "must not be empty");
    }
    sequence.skeletonId = ReadRequiredString(json, "skeleton", basePath);
    if (sequence.skeletonId.empty())
    {
        Fail(basePath, "skeleton", "must not be empty");
    }
    sequence.skeletonDigest = ReadRequiredString(json, "skeletonBlake3", basePath);
    if (!IsHexDigest(sequence.skeletonDigest))
    {
        Fail(basePath, "skeletonBlake3",
            "must be a 64-character hex BLAKE3 digest");
    }

    sequence.duration = ReadFiniteNumber(json, "duration", basePath);
    if (sequence.duration <= 0)
    {
        Fail(basePath, "duration", "must be > 0");
    }
    if (sequence.duration > 600)
    {
        Fail(basePath, "duration", "exceeds the 600 second import budget");
    }

    if (!json.contains("frameRate") || !json.at("frameRate").is_object())
    {
        Fail(basePath, "frameRate", "must be an object");
    }
    sequence.frameRateNumerator = ReadPositiveInt(
        json.at("frameRate"), "numerator", basePath, "frameRate.numerator");
    sequence.frameRateDenominator = ReadPositiveInt(
        json.at("frameRate"), "denominator", basePath, "frameRate.denominator");
    sequence.numFrames = ReadNonNegativeInt(json, "numFrames", basePath);
    sequence.numKeys = ReadNonNegativeInt(json, "numKeys", basePath);

    sequence.rateScale = ReadFiniteNumber(json, "rateScale", basePath);
    if (sequence.rateScale <= 0)
    {
        Fail(basePath, "rateScale", "must be > 0");
    }

    // 帧信息只作元数据与一致性校验；采样仍只用秒。
    const double frameDuration = static_cast<double>(sequence.numFrames) *
        sequence.frameRateDenominator / sequence.frameRateNumerator;
    if (std::abs(frameDuration - static_cast<double>(sequence.duration)) >
        FrameDurationTolerance)
    {
        Fail(basePath, "frameRate",
            "numFrames/frameRate=" + std::to_string(frameDuration) +
            " does not match duration=" + std::to_string(sequence.duration) +
            " within " + std::to_string(FrameDurationTolerance) + " s");
    }

    ValidateRootMotion(json, sequence, basePath);
    ValidateSampling(json, basePath);
    ValidateTracks(json, sequence, basePath);
    ValidateFloatCurves(json, sequence, basePath);
    ValidateNotifies(json, sequence, basePath);

    if (json.contains("syncMarkers"))
    {
        if (!json.at("syncMarkers").is_array())
        {
            Fail(basePath, "syncMarkers", "must be an array");
        }
        sequence.syncMarkersJson = json.at("syncMarkers").dump();
    }
    if (json.contains("source"))
    {
        if (!json.at("source").is_object())
        {
            Fail(basePath, "source", "must be an object");
        }
        sequence.sourceJson = json.at("source").dump();
    }

    return sequence;
}

const FloatCurve* FindCurve(const SequenceData& sequence, std::string_view name)
{
    for (const FloatCurve& curve : sequence.curves)
    {
        if (EqualsIgnoreCase(curve.name, name))
        {
            return &curve;
        }
    }
    return nullptr;
}

bool HasCurve(const SequenceData& sequence, std::string_view name)
{
    return FindCurve(sequence, name) != nullptr;
}

std::optional<float> SampleFloatCurve(
    const SequenceData& sequence, std::string_view name, float time)
{
    if (!std::isfinite(time))
    {
        throw std::invalid_argument(
            "Float curve sample time must be finite; sequence=" + sequence.id);
    }
    const FloatCurve* curve = FindCurve(sequence, name);
    if (curve == nullptr)
    {
        return std::nullopt;
    }
    if (curve->times.empty() || curve->times.size() != curve->values.size())
    {
        throw std::invalid_argument(
            "Float curve '" + curve->name + "' in sequence '" + sequence.id +
            "' is malformed");
    }
    if (curve->times.size() == 1 || time <= curve->times.front())
    {
        return curve->values.front();
    }
    if (time >= curve->times.back())
    {
        return curve->values.back();
    }

    const auto upper = std::lower_bound(
        curve->times.begin(), curve->times.end(), time);
    const size_t high = static_cast<size_t>(upper - curve->times.begin());
    const size_t low = high - 1;
    const double lowerTime = static_cast<double>(curve->times[low]);
    const double upperTime = static_cast<double>(curve->times[high]);
    const double span = upperTime - lowerTime;
    if (!(span > 0))
    {
        return curve->values[high];
    }
    const double alpha = (static_cast<double>(time) - lowerTime) / span;
    const double value = static_cast<double>(curve->values[low]) +
        alpha * (static_cast<double>(curve->values[high]) -
            static_cast<double>(curve->values[low]));
    return static_cast<float>(value);
}

float SampleFloatCurveOrThrow(
    const SequenceData& sequence, std::string_view name, float time)
{
    const std::optional<float> value = SampleFloatCurve(sequence, name, time);
    if (!value.has_value())
    {
        throw std::invalid_argument(
            "Float curve '" + std::string(name) + "' is not present in sequence '" +
            sequence.id + "'");
    }
    return *value;
}

void ValidateSequenceAgainstSkeleton(
    const SequenceData& sequence, const SkeletonAsset& skeleton, std::string_view assetPath)
{
    if (skeleton.id.empty())
    {
        Fail(assetPath, "skeleton",
            "requires a loaded Skeleton asset; clip='" + sequence.id + "'");
    }
    if (sequence.skeletonId != skeleton.id)
    {
        Fail(assetPath, "skeleton",
            "id '" + sequence.skeletonId + "' does not match skeleton asset '" +
            skeleton.id + "'");
    }
    if (!sequence.skeletonDigest.empty() && !skeleton.sourceDigest.empty() &&
        !EqualsIgnoreCase(sequence.skeletonDigest, skeleton.sourceDigest))
    {
        Fail(assetPath, "skeletonBlake3",
            "digest '" + sequence.skeletonDigest +
            "' does not match skeleton file digest '" + skeleton.sourceDigest + "'");
    }
    for (const FloatCurve& curve : sequence.curves)
    {
        if (!skeleton.HasCurve(curve.name))
        {
            Fail(assetPath, "floatCurveKeys",
                "curve '" + curve.name + "' is not registered in skeleton '" +
                skeleton.id + "' (clip='" + sequence.id + "')");
        }
    }
    for (const std::string& track : sequence.tracks)
    {
        if (skeleton.JointIndex(track) < 0)
        {
            Fail(assetPath, "tracks",
                "track '" + track + "' is not a joint in skeleton '" +
                skeleton.id + "' (clip='" + sequence.id + "')");
        }
    }
}
}
