#include "motionRuntimeDatabase.h"
#include "animation/motionDatabase.h"
#include "mesh/loader/animation/assimpSkeletalImporter.h"
#include "shader/build/contentHash.h"
#include "shader/build/atomicFile.h"
#include "skeletalPlayback.h"
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>

namespace VL::Animation
{
namespace
{
std::filesystem::path Resolve(const std::filesystem::path& root, const std::string& relative)
{
    const auto path = std::filesystem::u8path(relative);
    if (path.is_absolute() || path.has_root_name() || relative.find('\\') != std::string::npos ||
        relative.find(':') != std::string::npos)
        throw std::invalid_argument("Motion source requires a resource-relative path");
    const auto canonicalRoot = std::filesystem::canonical(root);
    const auto resolved = std::filesystem::weakly_canonical(canonicalRoot / path);
    const auto inside = resolved.lexically_relative(canonicalRoot);
    if (inside.empty() || *inside.begin() == "..")
        throw std::invalid_argument("Motion source escapes resource root");
    return resolved;
}

nlohmann::json ReadJson(const std::filesystem::path& path, std::string* digest = nullptr)
{
    if (std::filesystem::file_size(path) > 64 * 1024 * 1024)
        throw std::invalid_argument("Motion JSON exceeds 64 MiB budget");
    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input || bytes.size() > 64 * 1024 * 1024)
        throw std::runtime_error("Cannot capture motion JSON input");
    if (digest) *digest = ContentHasher::HashString(bytes).ToHex();
    return nlohmann::json::parse(bytes);
}

void AddInput(nlohmann::json& inputs, const std::filesystem::path& root, const std::string& path)
{
    const auto resolved = Resolve(root, path);
    inputs.push_back({{"path", path}, {"blake3", ContentHasher::HashFile(resolved).ToHex()}});
}

void ValidateCacheBinding(const nlohmann::json& cache, const nlohmann::json& capture)
{
    const auto& payload = cache.at("database");
    if (cache.at("identity") != capture.at("identity") || cache.at("inputs") != capture.at("inputs") ||
        payload.at("schema") != capture.at("schema"))
        throw std::invalid_argument("Motion cache input/schema binding mismatch");
    auto ids = nlohmann::json::array(), loops = nlohmann::json::array();
    for (const auto& member : capture.at("source").at("members"))
    {
        ids.push_back(member.at("clip"));
        loops.push_back(member.at("loop"));
    }
    if (payload.at("clipIds") != ids || payload.at("looping") != loops)
        throw std::invalid_argument("Motion cache member binding mismatch");
}
}

MotionLoopVelocity ExtractLoopRootVelocity(const SkeletonMesh& mesh, const Clip& clip, bool loop)
{
    MotionLoopVelocity result;
    if (!loop) return result;
    const float duration = clip.runtime->duration();
    const auto ends = SampleModels(mesh, clip, {0, duration});
    const auto& start = ends.front().front();
    const auto& end = ends.back().front();
    const Eigen::Matrix4f delta = end * start.inverse();
    const Eigen::Matrix3f rotation = delta.block<3, 3>(0, 0);
    // 绑定时只接受接近纯平移的周期根变换，不把转身或缩放周期当作直线稳态速度。
    // 周期均值仍是近似：不额外密采样检查片段内部弯曲或速度变化。
    constexpr float radiansPerDegree = 3.14159265358979323846f / 180;
    if ((rotation.transpose() * rotation - Eigen::Matrix3f::Identity()).norm() > 0.001f ||
        std::abs(rotation.determinant() - 1) > 0.001f ||
        std::abs(Eigen::AngleAxisf(rotation).angle()) >= radiansPerDegree)
        return result;
    result.velocity = (end.block<3, 1>(0, 3) - start.block<3, 1>(0, 3)) / duration;
    result.velocity.y() = 0;
    result.available = result.velocity.squaredNorm() > 1.0e-8f;
    return result;
}

std::shared_ptr<const MotionRuntimeDatabase> LoadMotionRuntimeDatabase(
    const std::filesystem::path& root, const std::string& sourcePath,
    const std::string& modelPath, const SkeletalAsset& asset)
{
    // MM 观察历史当前仍按目标骨架记录；先拒绝，不能拿源索引读取目标姿态。
    // 此检查必须早于缓存构建/磁盘写入。
    if (asset.IsRetargeted())
        throw std::invalid_argument("Motion Matching on retargeted assets requires a source-space pose history");
    const auto report = BuildMotionDatabaseReport(root, sourcePath);
    const auto capture = CaptureMotionDatabaseInputs(root, sourcePath);
    if (report.at("identity") != capture.at("identity") || capture.at("source").at("model") != modelPath)
        throw std::invalid_argument("Motion database model/input identity changed while binding");
    // 骨名和轨道数不能证明同一代资产；数据库的模型/片段输入必须逐项属于实际导入的字节清单。
    const auto& inputs = capture.at("inputs");
    for (size_t index = 2; index < inputs.size(); ++index)
    {
        const std::string path = inputs[index].at("path");
        const std::string digest = inputs[index].at("blake3");
        bool matching = false;
        for (const auto& input : asset.sourceInputs)
            if (input.first == path && input.second == digest) matching = true;
        if (!matching) throw std::invalid_argument("Motion database does not match imported asset input: " + path);
    }
    const auto cache = ReadJson(Resolve(root, report.at("cache")));
    ValidateCacheBinding(cache, capture);
    const auto& payload = cache.at("database");
    if (cache.at("identity") != report.at("identity") || cache.at("payloadBlake3") != report.at("payloadBlake3") ||
        ContentHasher::HashString(payload.dump()).ToHex() != report.at("payloadBlake3"))
        throw std::invalid_argument("Motion database cache changed during binding");
    auto result = std::make_shared<MotionRuntimeDatabase>();
    result->sourcePath = sourcePath;
    result->identity = report.at("identity");
    result->payloadIdentity = report.at("payloadBlake3");
    result->modelPath = modelPath;
    result->search = FreezeMotionDatabase(ParseMotionDatabase(payload));
    result->joints = ResolveFeatureJoints(asset.mesh, result->search->Data().schema);
    for (const auto& id : result->search->Data().clipIds)
    {
        size_t index = 0;
        while (index < asset.clips.size() && asset.clips[index].id != id) ++index;
        if (index == asset.clips.size()) throw std::invalid_argument("Motion database clip missing from imported asset");
        result->assetClips.push_back(index);
        const size_t member = result->assetClips.size() - 1;
        result->loopVelocities.push_back(ExtractLoopRootVelocity(
            asset.mesh, asset.clips[index], result->search->Data().looping.at(member)));
    }
    const auto& skeleton = payload.at("skeleton");
    if (skeleton.size() != asset.mesh.joints.size())
        throw std::invalid_argument("Motion database skeleton does not match imported asset");
    for (size_t index = 0; index < skeleton.size(); ++index)
        if (skeleton[index].at("name") != asset.mesh.joints[index].name ||
            skeleton[index].at("parent") != asset.mesh.joints[index].parent)
            throw std::invalid_argument("Motion database skeleton ordering does not match imported asset");
    for (const auto& frame : result->search->Data().frames)
    {
        const auto& clip = asset.clips.at(result->assetClips.at(frame.clip));
        if (frame.time >= clip.raw.duration ||
            !CanExtractClipFeatures(clip, result->search->Data().schema, frame.time,
                result->search->Data().looping.at(frame.clip)))
            throw std::invalid_argument("Motion database frame is outside the imported clip complete window");
    }
    if (capture.at("identity") != CaptureMotionDatabaseInputs(root, sourcePath).at("identity"))
        throw std::invalid_argument("Motion database inputs changed before binding commit");
    return result;
}

nlohmann::json CaptureMotionDatabaseInputs(const std::filesystem::path& root, const std::string& sourcePath)
{
    std::string sourceDigest, schemaDigest, modelDigest;
    const auto source = ReadJson(Resolve(root, sourcePath), &sourceDigest);
    if (source.at("type") != "motionDatabaseSource" || source.at("version") != 1 ||
        source.at("boundaryPolicy") != "exclude-nonloop-extend-loop-v1")
        throw std::invalid_argument("Unsupported motion database source policy");
    const auto schemaPath = source.at("schema").get<std::string>();
    const auto modelPath = source.at("model").get<std::string>();
    const auto schema = Animation::ParseFeatureSchema(ReadJson(Resolve(root, schemaPath), &schemaDigest));
    const auto model = ReadJson(Resolve(root, modelPath), &modelDigest);
    if (model.at("type") != "mesh") throw std::invalid_argument("Motion source requires skeletal mesh");
    const auto& skinning = model.at("skinning");
    // 骨架根骨唯一来源是 SK；旧 skinning.root 已删除，缺失即视为源数据错误。
    if (!skinning.contains("skeleton") || !skinning.at("skeleton").is_string() ||
        skinning.at("skeleton").get<std::string>().empty())
        throw std::invalid_argument("Motion source requires skinning.skeleton: " + modelPath);
    const std::string skeletonPath = skinning.at("skeleton").get<std::string>();
    std::string skeletonDigest;
    const nlohmann::json skeleton = ReadJson(Resolve(root, skeletonPath), &skeletonDigest);
    if (!skeleton.contains("root") || !skeleton.at("root").is_string() ||
        skeleton.at("root").get<std::string>().empty())
        throw std::invalid_argument(
            "Skeleton asset '" + skeletonPath + "' requires a non-empty root field");
    const std::string rootJoint = skeleton.at("root").get<std::string>();
    if (!source.at("members").is_array() || source.at("members").empty() || source.at("members").size() > 256)
        throw std::invalid_argument("Motion member budget");
    auto inputs = nlohmann::json::array();
    // 描述和身份使用同一次读取的原始字节，禁止先解析旧内容、再摘要新文件。
    inputs.push_back({{"path", sourcePath}, {"blake3", sourceDigest}});
    inputs.push_back({{"path", schemaPath}, {"blake3", schemaDigest}});
    inputs.push_back({{"path", modelPath}, {"blake3", modelDigest}});
    AddInput(inputs, root, model.at("modelDataPath"));
    // SK 的 root 参与建库，必须进入身份；否则改根骨会错误命中旧缓存。
    inputs.push_back({{"path", skeletonPath}, {"blake3", skeletonDigest}});
    std::set<std::string> unique;
    for (const auto& member : source.at("members"))
    {
        const auto id = member.at("clip").get<std::string>();
        if (!member.at("loop").is_boolean() || !unique.insert(id).second)
            throw std::invalid_argument("Motion members require unique clip IDs and explicit loop");
        std::string path;
        for (const auto& clip : skinning.at("clips"))
            if (clip.at("id") == id) path = clip.at("path");
        if (path.empty()) throw std::invalid_argument("Motion member is not in mesh clip catalog: " + id);
        AddInput(inputs, root, path);
    }
    CanonicalFieldHasher identity("vl.motion-database.v1");
    identity.AddString("policy", "ue-fbx-cm-yup-v1;ozz-0.16.0;root-heading;raw-root;backward-velocity-v1;se3-loop-v1;welford-population-v1;pre-ik");
    identity.AddString("inputs", inputs.dump());
    return {{"identity", identity.Finalize().ToHex()}, {"inputs", inputs},
        {"source", source}, {"schema", Animation::FeatureSchemaJson(schema)}, {"model", model},
        {"rootJoint", rootJoint}};
}

nlohmann::json BuildMotionDatabaseReport(const std::filesystem::path& root, const std::string& sourcePath)
{
    const auto capture = CaptureMotionDatabaseInputs(root, sourcePath);
    const std::string identity = capture.at("identity");
    const auto cachePath = Resolve(root, "Generated/MotionMatching/" + identity + ".json");
    const std::string relative = cachePath.lexically_relative(std::filesystem::canonical(root)).generic_string();
    nlohmann::json payload;
    std::string reason = "missing";
    if (std::filesystem::exists(cachePath))
    {
        try
        {
            const auto cache = ReadJson(cachePath);
            ValidateCacheBinding(cache, capture);
            payload = cache.at("database");
            if (cache.at("identity") != identity || cache.at("payloadBlake3") !=
                ContentHasher::HashString(payload.dump()).ToHex())
                throw std::runtime_error("Motion cache content digest mismatch");
            Animation::ParseMotionDatabase(payload);
            reason = "verified-hit";
        }
        catch (const std::exception&)
        {
            payload = nullptr;
            reason = "invalid-cache-rebuilt";
        }
    }
    if (reason != "verified-hit")
    {
        const auto& model = capture.at("model");
        const auto& skinning = model.at("skinning");
        // 根骨取自 capture 阶段同一代 SK 字节，避免二次读取命中另一版本。
        const auto mesh = Animation::AssimpSkeletalImporter::ReadMesh(
            Resolve(root, model.at("modelDataPath")),
            capture.at("rootJoint").get<std::string>());
        std::vector<Animation::Clip> clips;
        std::vector<std::pair<size_t, bool>> members;
        for (const auto& member : capture.at("source").at("members"))
            for (const auto& clip : skinning.at("clips"))
                if (clip.at("id") == member.at("clip"))
                {
                    members.emplace_back(clips.size(), member.at("loop").get<bool>());
                    clips.push_back(Animation::AssimpSkeletalImporter::ReadClip(
                        Resolve(root, clip.at("path")), mesh, clip.at("id")));
                }
        const auto database = Animation::BuildMotionDatabase(mesh, clips,
            Animation::ParseFeatureSchema(capture.at("schema")), members);
        payload = Animation::MotionDatabaseJson(database);
        auto jointNames = nlohmann::json::array();
        for (const auto& joint : mesh.joints) jointNames.push_back({{"name", joint.name}, {"parent", joint.parent}});
        payload["skeleton"] = std::move(jointNames);
    }
    // 导入器会重新打开 FBX；发布前复核整组源字节，不能混用两个输入世代。
    if (capture.at("identity") != CaptureMotionDatabaseInputs(root, sourcePath).at("identity"))
        throw std::runtime_error("Motion inputs changed during build; candidate was not published");
    const auto digest = ContentHasher::HashString(payload.dump()).ToHex();
    if (reason != "verified-hit")
    {
        std::filesystem::create_directories(cachePath.parent_path());
        WriteTextFileAtomically(cachePath, nlohmann::json{
            {"identity", identity}, {"inputs", capture.at("inputs")},
            {"payloadBlake3", digest}, {"database", payload}}.dump());
    }
    return {{"identity", identity}, {"payloadBlake3", digest}, {"cache", relative},
        {"cacheStatus", reason}, {"inputs", capture.at("inputs")},
        {"frames", payload.at("frames").size()}, {"dimensions", payload.at("channels").size()},
        {"clips", payload.at("clipIds")}};
}
}
