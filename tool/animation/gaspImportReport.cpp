#include "gaspImportReport.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include "mesh/loader/animation/assimpSkeletalImporter.h"
#include "shader/build/contentHash.h"

namespace VL::AnimationPreflight
{
namespace
{
using Json = nlohmann::json;

Json Read(const std::filesystem::path& path)
{
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 32 * 1024 * 1024)
    {
        throw std::runtime_error("Missing or excessive audit JSON: " + path.u8string());
    }
    std::ifstream stream(path, std::ios::binary);
    return Json::parse(stream);
}

Json Matrix(const Eigen::Matrix4f& value)
{
    Json result = Json::array();
    for (int row = 0; row < 4; ++row)
    {
        result.push_back({value(row, 0), value(row, 1), value(row, 2), value(row, 3)});
    }
    return result;
}

double Finite(const Json& node)
{
    const double value = node.get<double>();
    if (!std::isfinite(value)) throw std::runtime_error("Non-finite audit metadata");
    return value;
}

void ValidateMetadata(const Json& sidecar)
{
    const double duration = Finite(sidecar.at("duration"));
    if (duration <= 0 || duration > 600) throw std::runtime_error("Invalid sidecar duration");
    std::set<std::string> names;
    for (const auto& curve : sidecar.at("floatCurveKeys"))
    {
        if (!names.insert(curve.at("name").get<std::string>()).second ||
            curve.at("times").size() != curve.at("values").size())
        {
            throw std::runtime_error("Duplicate curve or curve key/value mismatch");
        }
        double previous = -std::numeric_limits<double>::infinity();
        for (size_t key = 0; key < curve.at("times").size(); ++key)
        {
            const double time = Finite(curve.at("times")[key]);
            Finite(curve.at("values")[key]);
            if (time <= previous) throw std::runtime_error("Unsorted curve times");
            previous = time;
        }
        // UE 曲线允许片段端点外的作者 key；原样保留，不以 clip 时长裁掉 pre/post roll。
    }
    for (const auto& event : sidecar.at("notifies"))
    {
        const double time = Finite(event.at("time"));
        const double length = Finite(event.at("duration"));
        if (time < 0 || time > duration + 0.00001 || length < 0)
        {
            throw std::runtime_error("Invalid notify time/duration");
        }
    }
}

void AddInput(const std::filesystem::path& root, const std::string& name, Json& inputs)
{
    const auto path = root / name;
    inputs.push_back({{"file", name}, {"bytes", std::filesystem::file_size(path)},
        {"blake3", ContentHasher::HashFile(path).ToHex()}});
}

Json ClipReport(
    const std::filesystem::path& directory, const std::string& stem,
    const Animation::SkeletonMesh& mesh, Json& inputs)
{
    const auto sidecar = Read(directory / (stem + ".json"));
    ValidateMetadata(sidecar);
    const auto clip = Animation::AssimpSkeletalImporter::ReadClip(
        directory / (stem + ".fbx"), mesh, sidecar.at("asset").get<std::string>());
    const double duration = sidecar.at("duration").get<double>();
    if (std::abs(duration - clip.raw.duration) > 0.00001)
    {
        throw std::runtime_error("Sidecar and FBX duration mismatch: " + stem);
    }
    const std::set<std::string> fallback(clip.restFallbackJoints.begin(), clip.restFallbackJoints.end());
    std::set<std::string> joints;
    for (const auto& joint : mesh.joints) joints.insert(joint.name);
    for (const auto& track : sidecar.at("tracks"))
    {
        const auto name = track.get<std::string>();
        if (!joints.count(name) || fallback.count(name))
        {
            throw std::runtime_error("Authored UE track missing from imported clip: " + name);
        }
    }
    Json result = {{"id", clip.id}, {"stem", stem}, {"duration", clip.raw.duration},
        {"authoredTracks", sidecar.at("tracks")}, {"restFallbackJoints", clip.restFallbackJoints},
        {"frames", Json::array()}, {"rootSamples", Json::array()},
        {"metadata", {{"floatCurveKeys", sidecar.at("floatCurveKeys")},
            {"notifies", sidecar.at("notifies")}, {"syncMarkers", sidecar.at("syncMarkers")},
            {"settings", sidecar.at("settings")}, {"rateScale", sidecar.at("rateScale")},
            {"curveLimit", sidecar.at("curveLimit")}}},
        {"metadataScope", "Preserved UE values/text; no curve interpolation or gameplay event execution"}};
    std::vector<float> times;
    for (const auto& frame : sidecar.at("referenceFrames"))
    {
        const double time = Finite(frame.at("time"));
        if (time < 0 || time > duration) throw std::runtime_error("Invalid reference frame time");
        times.push_back(static_cast<float>(time / duration) * clip.raw.duration);
    }
    const auto frames = Animation::SampleModels(mesh, clip, times);
    for (size_t frame = 0; frame < times.size(); ++frame)
    {
        Json models = Json::array();
        for (const auto& model : frames[frame]) models.push_back(Matrix(model));
        result["frames"].push_back({{"time", sidecar.at("referenceFrames")[frame].at("time")},
            {"models", std::move(models)}});
    }
    times.clear();
    for (const auto& sample : sidecar.at("rawRootSamples"))
    {
        const double time = Finite(sample.at("time"));
        if (time < 0 || time > duration) throw std::runtime_error("Invalid root sample time");
        times.push_back(static_cast<float>(time / duration) * clip.raw.duration);
    }
    const auto rootFrames = Animation::SampleModels(mesh, clip, times);
    for (size_t sample = 0; sample < times.size(); ++sample)
    {
        result["rootSamples"].push_back({{"time", sidecar.at("rawRootSamples")[sample].at("time")},
            {"model", Matrix(rootFrames[sample][0])}});
    }
    AddInput(directory, stem + ".fbx", inputs);
    AddInput(directory, stem + ".json", inputs);
    AddInput(directory, stem + ".copy", inputs);
    return result;
}
}

Json BuildGaspImportReport(const std::filesystem::path& auditDirectory)
{
    const auto audit = Read(auditDirectory / "audit.json");
    if (audit.at("status") != "asset-export-complete" || !audit.at("errors").empty())
    {
        throw std::runtime_error("Expected successful fixed GASP P0 export");
    }
    // 此工具只读固定导出列表；不接受侧车里的任意磁盘路径作为新输入。
    const std::vector<std::string> stems{
        "M_Neutral_Stand_Idle_Loop", "M_Neutral_Crouch_Idle_Loop",
        "M_Neutral_Stand_Turn_180_L", "M_Neutral_Stand_Turn_180_R",
        "M_Neutral_Walk_Loop_F", "M_Neutral_Walk_Start_F_Lfoot", "M_Neutral_Walk_Stop_F_Lfoot",
        "M_Neutral_Run_Loop_F", "M_Neutral_Sprint_Loop_F",
        "M_Neutral_Jump_F_Start_Stand_Lfoot", "M_Neutral_Jump_Loop_Fall",
        "M_Neutral_Jump_F_Land_Stand_Light_Lfoot"};
    const auto mesh = Animation::AssimpSkeletalImporter::ReadMesh(
        auditDirectory / "SKM_UEFN_Mannequin.fbx", "root");
    Json report = {{"format", "vl.gasp.cpu-import.v1"}, {"policy", "ue-fbx-cm-yup-v1"},
        {"space", "VL = (FBX.X, FBX.Z, -FBX.Y) = (UE.X, UE.Z, UE.Y), cm; model forward +Z"},
        {"scope", "CPU assets and ozz sampling only; no World load, GPU skinning or formal asset promotion"},
        {"inputs", Json::array()}, {"joints", Json::array()}, {"sections", Json::array()},
        {"excludedVirtualBones", Json::array()}, {"clips", Json::array()}};
    AddInput(auditDirectory, "audit.json", report["inputs"]);
    AddInput(auditDirectory, "skeleton.json", report["inputs"]);
    AddInput(auditDirectory, "SKM_UEFN_Mannequin.fbx", report["inputs"]);
    const auto skeletonSidecar = Read(auditDirectory / "skeleton.json");
    for (const auto& bone : skeletonSidecar.at("bones"))
    {
        const auto name = bone.at("name").get<std::string>();
        if (name.rfind("VB ", 0) == 0) report["excludedVirtualBones"].push_back(name);
    }
    for (const auto& joint : mesh.joints)
    {
        report["joints"].push_back({{"name", joint.name}, {"parent", joint.parent},
            {"restLocal", Matrix(joint.restLocal)}, {"restModel", Matrix(joint.restModel)}});
    }
    for (const auto& section : mesh.sections)
    {
        report["sections"].push_back({{"name", section.name}, {"materialSlot", section.materialSlot},
            {"vertices", section.vertices.size()}, {"indices", section.indices.size()},
            {"maxInfluences", section.maxInfluences}, {"influenceCount", section.influences.size()},
            {"maxWeightSumError", section.maxWeightSumError},
            {"maxBindPositionErrorCm", section.maxBindPositionErrorCm},
            {"maxBindLinearError", section.maxBindLinearError},
            {"maxBindSkinErrorCm", section.maxBindSkinErrorCm}});
    }
    for (const auto& stem : stems)
    {
        report["clips"].push_back(ClipReport(auditDirectory, stem, mesh, report["inputs"]));
    }
    CanonicalFieldHasher identity("vl.gasp.cpu-import.v1");
    identity.AddString("policy", report["policy"].get<std::string>());
    identity.AddString("ozz", "0.16.0/6cbdc790123aa4731d82e255df187b3a8a808256");
    for (const auto& input : report["inputs"])
    {
        identity.AddString("file", input.at("file").get<std::string>());
        identity.AddDigest("content", ContentDigest::FromHex(input.at("blake3").get<std::string>()));
    }
    report["inputBlake3"] = identity.Finalize().ToHex();
    return report;
}

Json BuildGaspSkinReference(const std::filesystem::path& auditDirectory, float time)
{
    const auto audit = Read(auditDirectory / "audit.json");
    if (audit.at("status") != "asset-export-complete" || !audit.at("errors").empty())
        throw std::runtime_error("Expected successful fixed GASP P0 export");
    const auto mesh = Animation::AssimpSkeletalImporter::ReadMesh(
        auditDirectory / "SKM_UEFN_Mannequin.fbx", "root");
    const auto clip = Animation::AssimpSkeletalImporter::ReadClip(
        auditDirectory / "M_Neutral_Walk_Loop_F.fbx", mesh, "walk");
    const auto models = Animation::SampleModels(mesh, clip, {time}).front();
    Json result{{"format", "vl.gasp.cpu-skin-reference.v1"}, {"time", time},
        {"sections", Json::array()}, {"inputs", Json::array()}};
    AddInput(auditDirectory, "SKM_UEFN_Mannequin.fbx", result["inputs"]);
    AddInput(auditDirectory, "M_Neutral_Walk_Loop_F.fbx", result["inputs"]);
    for (const auto& section : mesh.sections)
    {
        Json vertices = Json::array();
        std::vector<Eigen::Matrix4f> palette;
        for (size_t joint = 0; joint < models.size(); ++joint)
            palette.push_back(models[joint] * section.inverseBinds[joint]);
        for (size_t vertex = 0; vertex < section.vertices.size(); ++vertex)
        {
            const auto& source = section.vertices[vertex];
            Eigen::Vector3f position = Eigen::Vector3f::Zero();
            Eigen::Matrix3f linear = Eigen::Matrix3f::Zero();
            for (uint32_t i = section.influenceOffsets[vertex];
                 i < section.influenceOffsets[vertex + 1]; ++i)
            {
                const auto& influence = section.influences[i];
                const auto& matrix = palette.at(influence.joint);
                // 独立按影响累加位置；不复用 GPU 的混合 mat4/打包 CSR 解码路径。
                position += influence.weight *
                    (matrix * source.position.homogeneous()).head<3>();
                linear += influence.weight * matrix.topLeftCorner<3, 3>();
            }
            const Eigen::Vector3f normal = (linear.inverse().transpose() * source.normal).normalized();
            vertices.push_back({position.x(), position.y(), position.z(),
                normal.x(), normal.y(), normal.z(), source.texCoord.x(), source.texCoord.y()});
        }
        result["sections"].push_back({{"name", section.name}, {"vertices", std::move(vertices)},
            {"indices", section.indices}, {"maxInfluences", section.maxInfluences}});
    }
    return result;
}
}
