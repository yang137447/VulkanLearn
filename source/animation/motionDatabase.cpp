#include "motionDatabase.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace VL::Animation
{
std::vector<float> MotionDatabase::Normalize(const std::vector<float>& features) const
{
    if (features.size() != means.size()) throw std::invalid_argument("Motion query dimension mismatch");
    std::vector<float> result;
    for (size_t i = 0; i < features.size(); ++i)
    {
        if (!std::isfinite(features[i])) throw std::invalid_argument("Nonfinite motion query");
        const float value = static_cast<float>((features[i] - means[i]) / scales[i]);
        if (!std::isfinite(value)) throw std::invalid_argument("Normalized motion query exceeds float range");
        result.push_back(value);
    }
    return result;
}

MotionDatabase BuildMotionDatabase(const SkeletonMesh& mesh, const std::vector<Clip>& clips,
    const FeatureSchema& inputSchema, const std::vector<std::pair<size_t, bool>>& members)
{
    MotionDatabase result;
    result.schema = ParseFeatureSchema(FeatureSchemaJson(inputSchema));
    ResolveFeatureJoints(mesh, result.schema);
    if (members.empty() || members.size() > 256) throw std::invalid_argument("Motion database member budget");
    std::set<size_t> unique;
    for (const auto& member : members)
    {
        if (member.first >= clips.size() || !unique.insert(member.first).second)
            throw std::invalid_argument("Invalid or duplicate motion database member");
        const auto& clip = clips[member.first];
        const auto index = static_cast<uint32_t>(result.clipIds.size());
        result.clipIds.push_back(clip.id);
        result.looping.push_back(member.second);
        const size_t count = static_cast<size_t>(std::ceil(double(clip.runtime->duration()) * result.schema.sampleRate));
        if (result.frames.size() + count > 100000)
            throw std::invalid_argument("Motion database exceeds 100000 frame budget");
        for (size_t frame = 0; frame < count; ++frame)
        {
            const float time = static_cast<float>(frame / double(result.schema.sampleRate));
            if (!CanExtractClipFeatures(clip, result.schema, time, member.second)) continue;
            result.frames.push_back({index, time, ExtractClipFeatures(mesh, clip, result.schema, time, member.second)});
        }
    }
    if (result.frames.empty()) throw std::invalid_argument("Motion database has no complete feature windows");
    const size_t dimensions = FeatureChannelNames(result.schema).size();
    result.means.resize(dimensions);
    result.scales.resize(dimensions);
    // 双精度 Welford 总体方差，按固定成员/采样顺序累积；常量通道 scale=1。
    size_t count = 0;
    for (const auto& frame : result.frames)
    {
        ++count;
        for (size_t column = 0; column < dimensions; ++column)
        {
            const double delta = frame.features[column] - result.means[column];
            result.means[column] += delta / count;
            result.scales[column] += delta * (frame.features[column] - result.means[column]);
        }
    }
    for (auto& scale : result.scales)
    {
        const double variance = scale / count;
        scale = variance <= 1e-12 ? 1 : std::sqrt(variance);
    }
    return result;
}

nlohmann::json MotionDatabaseJson(const MotionDatabase& database)
{
    auto frames = nlohmann::json::array();
    for (const auto& frame : database.frames)
        frames.push_back({{"clip", frame.clip}, {"time", frame.time}, {"features", frame.features}});
    return {{"type", "motionFeatureDatabase"}, {"version", 1},
        {"schema", FeatureSchemaJson(database.schema)}, {"clipIds", database.clipIds},
        {"looping", database.looping}, {"channels", FeatureChannelNames(database.schema)},
        {"weights", FeatureChannelWeights(database.schema)},
        {"means", database.means}, {"scales", database.scales}, {"frames", frames}};
}

MotionDatabase ParseMotionDatabase(const nlohmann::json& json)
{
    if (json.at("type") != "motionFeatureDatabase" || json.at("version") != 1)
        throw std::invalid_argument("Unsupported motion database format");
    MotionDatabase result;
    result.schema = ParseFeatureSchema(json.at("schema"));
    result.clipIds = json.at("clipIds").get<std::vector<std::string>>();
    result.looping = json.at("looping").get<std::vector<bool>>();
    const auto channels = FeatureChannelNames(result.schema);
    if (json.at("channels") != channels || json.at("weights") != FeatureChannelWeights(result.schema) ||
        result.clipIds.empty() || result.clipIds.size() > 256 || result.clipIds.size() != result.looping.size() ||
        std::set<std::string>(result.clipIds.begin(), result.clipIds.end()).size() != result.clipIds.size() ||
        std::find(result.clipIds.begin(), result.clipIds.end(), "") != result.clipIds.end())
        throw std::invalid_argument("Invalid motion database channel/member layout");
    result.means = json.at("means").get<std::vector<double>>();
    result.scales = json.at("scales").get<std::vector<double>>();
    if (result.means.size() != channels.size() || result.scales.size() != channels.size())
        throw std::invalid_argument("Invalid motion database statistics dimension");
    for (size_t i = 0; i < channels.size(); ++i)
        if (!std::isfinite(result.means[i]) || !std::isfinite(result.scales[i]) || result.scales[i] <= 0)
            throw std::invalid_argument("Invalid motion database statistics");
    const auto& frames = json.at("frames");
    if (!frames.is_array() || frames.empty() || frames.size() > 100000)
        throw std::invalid_argument("Invalid motion database frame budget");
    for (const auto& frame : frames)
    {
        if (!frame.at("clip").is_number_integer() || frame.at("clip").get<int64_t>() < 0 ||
            frame.at("clip").get<uint64_t>() >= result.clipIds.size())
            throw std::invalid_argument("Motion frame clip requires a member index");
        MotionDatabase::Frame value{frame.at("clip").get<uint32_t>(), frame.at("time").get<float>(),
            frame.at("features").get<std::vector<float>>()};
        if (value.clip >= result.clipIds.size() || !std::isfinite(value.time) || value.time < 0 ||
            value.time > 600 || value.features.size() != channels.size())
            throw std::invalid_argument("Invalid motion database frame shape");
        for (float feature : value.features)
            if (!std::isfinite(feature)) throw std::invalid_argument("Nonfinite database feature");
        if (!result.frames.empty() && (value.clip < result.frames.back().clip ||
            (value.clip == result.frames.back().clip && value.time <= result.frames.back().time)))
            throw std::invalid_argument("Motion frames must have stable clip/time order");
        result.frames.push_back(std::move(value));
    }
    return result;
}
}
