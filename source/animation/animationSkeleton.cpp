#include "animationSkeleton.h"

#include "animationAssets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>

namespace VL::Animation
{
namespace
{
constexpr size_t MaxJoints = 1024;
constexpr size_t MaxSlots = 64;
constexpr size_t MaxCurves = 256;
constexpr size_t MaxNameLength = 64;
// SK 与 FBX 的 restLocal 对照容差；schema 固定为 1e-5。
constexpr double RestLocalTolerance = 1e-5;
// restLocal 末行必须是 (0,0,0,1)；给浮点序列化留极小表示误差。
constexpr double AffineRowTolerance = 1e-5;

[[noreturn]] void Fail(
    std::string_view basePath, std::string_view field, const std::string& reason)
{
    throw std::runtime_error("Skeleton asset '" + std::string(basePath) + "': field \"" +
        std::string(field) + "\" " + reason);
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

std::string ReadRequiredString(
    const nlohmann::json& json, std::string_view field, std::string_view basePath)
{
    const std::string key(field);
    if (!json.contains(key))
    {
        Fail(basePath, field, "is required");
    }
    if (!json.at(key).is_string())
    {
        Fail(basePath, field, "must be a string");
    }
    return json.at(key).get<std::string>();
}

std::string ReadName(
    const nlohmann::json& value, std::string_view field, std::string_view basePath)
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
        Fail(basePath, field, "exceeds " + std::to_string(MaxNameLength) + " characters");
    }
    return name;
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

// row-major 4×4 → Eigen 默认列主序存储；数值语义保持 JSON 中的行/列位置。
Eigen::Matrix4f ReadRestLocal(
    const nlohmann::json& value, size_t jointIndex, std::string_view basePath)
{
    const std::string field = "joints[" + std::to_string(jointIndex) + "].restLocal";
    if (!value.is_array() || value.size() != 16)
    {
        Fail(basePath, field, "must be a 16-number row-major 4x4 array");
    }

    std::array<float, 16> elements{};
    for (size_t element = 0; element < elements.size(); ++element)
    {
        if (!value.at(element).is_number())
        {
            Fail(basePath, field, "element " + std::to_string(element) + " must be a number");
        }
        elements[element] = value.at(element).get<float>();
        if (!std::isfinite(elements[element]))
        {
            Fail(basePath, field, "element " + std::to_string(element) + " must be finite");
        }
    }

    // 末行固定 (0,0,0,1)：其余行承载旋转/缩放与厘米平移。
    if (std::abs(static_cast<double>(elements[12])) > AffineRowTolerance ||
        std::abs(static_cast<double>(elements[13])) > AffineRowTolerance ||
        std::abs(static_cast<double>(elements[14])) > AffineRowTolerance ||
        std::abs(static_cast<double>(elements[15]) - 1.0) > AffineRowTolerance)
    {
        Fail(basePath, field, "last row must be (0,0,0,1)");
    }

    Eigen::Matrix4f matrix;
    matrix << elements[0], elements[1], elements[2], elements[3],
        elements[4], elements[5], elements[6], elements[7],
        elements[8], elements[9], elements[10], elements[11],
        elements[12], elements[13], elements[14], elements[15];

    // 与 FBX 导入器一致：拒绝 shear/反射/奇异；缩放允许，但线性部分必须可分解为 TRS。
    const Eigen::Matrix3f linear = matrix.block<3, 3>(0, 0);
    Eigen::Matrix3f normalized = linear;
    for (int column = 0; column < 3; ++column)
    {
        const float scale = linear.col(column).norm();
        if (scale < 0.000001f)
        {
            Fail(basePath, field, "linear part is singular");
        }
        normalized.col(column) /= scale;
    }
    if ((normalized.transpose() * normalized - Eigen::Matrix3f::Identity()).cwiseAbs().maxCoeff() >
        0.0001f)
    {
        Fail(basePath, field, "linear part contains shear or non-unit axes");
    }
    if (linear.determinant() <= 0)
    {
        Fail(basePath, field, "linear part is reflected");
    }
    return matrix;
}

std::string MeshParentName(const SkeletonMesh& mesh, int parent)
{
    if (parent < 0)
    {
        return "<root>";
    }
    if (static_cast<size_t>(parent) >= mesh.joints.size())
    {
        return "<out-of-range:" + std::to_string(parent) + ">";
    }
    return mesh.joints[static_cast<size_t>(parent)].name;
}
}

int SkeletonAsset::JointIndex(std::string_view name) const
{
    for (size_t index = 0; index < joints.size(); ++index)
    {
        if (joints[index].name == name)
        {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool SkeletonAsset::HasSlot(std::string_view name) const
{
    return std::find(slots.begin(), slots.end(), name) != slots.end();
}

bool SkeletonAsset::HasCurve(std::string_view name) const
{
    for (const auto& curve : curves)
    {
        if (EqualsIgnoreCase(curve.first, name))
        {
            return true;
        }
    }
    return false;
}

SkeletonAsset ParseSkeleton(const nlohmann::json& json, std::string_view basePath)
{
    if (!json.is_object())
    {
        throw std::runtime_error(
            "Skeleton asset '" + std::string(basePath) + "' must be a JSON object");
    }

    SkeletonAsset skeleton;
    const std::string format = ReadRequiredString(json, "format", basePath);
    if (format != "vl.skeleton.v1")
    {
        Fail(basePath, "format", "must be \"vl.skeleton.v1\", got \"" + format + "\"");
    }
    skeleton.id = ReadRequiredString(json, "skeleton", basePath);
    if (skeleton.id.empty())
    {
        Fail(basePath, "skeleton", "must not be empty");
    }
    skeleton.root = ReadRequiredString(json, "root", basePath);
    if (skeleton.root.empty())
    {
        Fail(basePath, "root", "must not be empty");
    }

    if (!json.contains("joints"))
    {
        Fail(basePath, "joints", "is required");
    }
    const auto& joints = json.at("joints");
    if (!joints.is_array() || joints.empty())
    {
        Fail(basePath, "joints", "must be a non-empty array");
    }
    if (joints.size() > MaxJoints)
    {
        Fail(basePath, "joints", "exceeds " + std::to_string(MaxJoints) + " joints");
    }

    std::set<std::string> jointNames;
    int rootIndex = -1;
    skeleton.joints.reserve(joints.size());
    for (size_t index = 0; index < joints.size(); ++index)
    {
        const auto& joint = joints.at(index);
        const std::string prefix = "joints[" + std::to_string(index) + "]";
        if (!joint.is_object())
        {
            Fail(basePath, prefix, "must be an object");
        }

        SkeletonJoint parsed;
        if (!joint.contains("name"))
        {
            Fail(basePath, prefix + ".name", "is required");
        }
        parsed.name = ReadName(joint.at("name"), prefix + ".name", basePath);
        if (!jointNames.insert(parsed.name).second)
        {
            Fail(basePath, prefix + ".name", "duplicates joint name '" + parsed.name + "'");
        }

        if (!joint.contains("parent") || !joint.at("parent").is_number_integer())
        {
            Fail(basePath, prefix + ".parent", "must be an integer");
        }
        const int64_t parent = joint.at("parent").get<int64_t>();
        if (parent < -1)
        {
            Fail(basePath, prefix + ".parent", "must be -1 or a preceding joint index");
        }
        if (parent >= static_cast<int64_t>(index))
        {
            Fail(basePath, prefix + ".parent",
                "must be less than the joint index (topological order), got " +
                std::to_string(parent));
        }
        parsed.parent = static_cast<int>(parent);

        if (!joint.contains("restLocal"))
        {
            Fail(basePath, prefix + ".restLocal", "is required");
        }
        parsed.restLocal = ReadRestLocal(joint.at("restLocal"), index, basePath);

        if (parsed.parent == -1)
        {
            if (rootIndex >= 0)
            {
                Fail(basePath, "root",
                    "must identify the single joint with parent -1; found '" +
                    skeleton.joints[static_cast<size_t>(rootIndex)].name + "' and '" +
                    parsed.name + "'");
            }
            rootIndex = static_cast<int>(index);
        }
        skeleton.joints.push_back(std::move(parsed));
    }

    if (rootIndex < 0)
    {
        Fail(basePath, "root", "must reference a joint with parent -1");
    }
    if (skeleton.joints[static_cast<size_t>(rootIndex)].name != skeleton.root)
    {
        Fail(basePath, "root",
            "must match the joint with parent -1; root='" + skeleton.root +
            "' but parent=-1 joint is '" +
            skeleton.joints[static_cast<size_t>(rootIndex)].name + "'");
    }

    if (!json.contains("slots"))
    {
        Fail(basePath, "slots", "is required");
    }
    const auto& slots = json.at("slots");
    if (!slots.is_array() || slots.empty())
    {
        Fail(basePath, "slots", "must be a non-empty array");
    }
    if (slots.size() > MaxSlots)
    {
        Fail(basePath, "slots", "exceeds " + std::to_string(MaxSlots) + " entries");
    }
    std::set<std::string> slotNames;
    skeleton.slots.reserve(slots.size());
    for (size_t index = 0; index < slots.size(); ++index)
    {
        const std::string field = "slots[" + std::to_string(index) + "]";
        const std::string slot = ReadName(slots.at(index), field, basePath);
        if (!slotNames.insert(slot).second)
        {
            Fail(basePath, field, "duplicates slot name '" + slot + "'");
        }
        skeleton.slots.push_back(slot);
    }

    if (json.contains("curves"))
    {
        const auto& curves = json.at("curves");
        if (!curves.is_object())
        {
            Fail(basePath, "curves", "must be an object mapping curve names to types");
        }
        if (curves.size() > MaxCurves)
        {
            Fail(basePath, "curves", "exceeds " + std::to_string(MaxCurves) + " entries");
        }
        std::set<std::string> curveNames;
        for (auto entry = curves.begin(); entry != curves.end(); ++entry)
        {
            const std::string field = "curves." + entry.key();
            if (entry.key().empty() || entry.key().size() > MaxNameLength)
            {
                Fail(basePath, field, "curve name must be 1.." +
                    std::to_string(MaxNameLength) + " characters");
            }
            if (!curveNames.insert(LowerAscii(entry.key())).second)
            {
                Fail(basePath, field, "duplicates a curve name case-insensitively");
            }
            if (!entry.value().is_string())
            {
                Fail(basePath, field, "curve type must be a string");
            }
            const std::string type = entry.value().get<std::string>();
            if (type != "Float")
            {
                Fail(basePath, field,
                    "unsupported curve type \"" + type + "\"; only \"Float\" is accepted");
            }
            skeleton.curves.emplace(entry.key(), type);
        }
    }

    if (json.contains("virtualBones"))
    {
        if (!json.at("virtualBones").is_array())
        {
            Fail(basePath, "virtualBones", "must be an array when present");
        }
        skeleton.virtualBonesJson = json.at("virtualBones").dump();
    }
    if (json.contains("additiveReferencePose") && !json.at("additiveReferencePose").is_null())
    {
        if (!json.at("additiveReferencePose").is_object())
        {
            Fail(basePath, "additiveReferencePose", "must be an object or null");
        }
        skeleton.additiveReferencePoseJson = json.at("additiveReferencePose").dump();
    }

    if (json.contains("source"))
    {
        const auto& source = json.at("source");
        if (!source.is_object())
        {
            Fail(basePath, "source", "must be an object");
        }
        if (source.contains("fbx"))
        {
            if (!source.at("fbx").is_string() || source.at("fbx").get<std::string>().empty())
            {
                Fail(basePath, "source.fbx", "must be a non-empty string");
            }
            skeleton.sourceFbx = source.at("fbx").get<std::string>();
        }
        if (source.contains("fbxBlake3"))
        {
            if (!source.at("fbxBlake3").is_string() ||
                !IsHexDigest(source.at("fbxBlake3").get<std::string>()))
            {
                Fail(basePath, "source.fbxBlake3", "must be a 64-character hex BLAKE3 digest");
            }
            skeleton.sourceFbxDigest = source.at("fbxBlake3").get<std::string>();
        }
        if (source.contains("tool"))
        {
            if (!source.at("tool").is_string() || source.at("tool").get<std::string>().empty())
            {
                Fail(basePath, "source.tool", "must be a non-empty string");
            }
            skeleton.sourceTool = source.at("tool").get<std::string>();
        }
    }

    return skeleton;
}

void ValidateSkeletonAgainstMesh(
    const SkeletonAsset& skeleton, const SkeletonMesh& mesh, std::string_view fbxPath)
{
    if (skeleton.joints.size() != mesh.joints.size())
    {
        throw std::runtime_error(
            "Skeleton '" + skeleton.id + "' joint count " +
            std::to_string(skeleton.joints.size()) + " does not match FBX '" +
            std::string(fbxPath) + "' joint count " + std::to_string(mesh.joints.size()));
    }
    if (skeleton.joints.empty())
    {
        throw std::runtime_error(
            "Skeleton '" + skeleton.id + "' has no joints for FBX '" +
            std::string(fbxPath) + "'");
    }
    if (skeleton.root != mesh.joints.front().name)
    {
        throw std::runtime_error(
            "Skeleton '" + skeleton.id + "' root '" + skeleton.root +
            "' does not match FBX root joint '" + mesh.joints.front().name + "'");
    }

    for (size_t index = 0; index < skeleton.joints.size(); ++index)
    {
        const SkeletonJoint& expected = skeleton.joints[index];
        const Joint& actual = mesh.joints[index];
        const std::string prefix =
            "Skeleton joint " + std::to_string(index) + " '" + expected.name + "'";

        if (expected.name != actual.name)
        {
            throw std::runtime_error(prefix + " name mismatch against FBX '" +
                std::string(fbxPath) + "': skeleton name='" + expected.name +
                "' fbx name='" + actual.name + "'");
        }
        if (expected.parent != actual.parent)
        {
            throw std::runtime_error(prefix + " parent mismatch against FBX '" +
                std::string(fbxPath) + "': skeleton parent=" +
                std::to_string(expected.parent) + " '" +
                MeshParentName(mesh, expected.parent) + "' fbx parent=" +
                std::to_string(actual.parent) + " '" +
                MeshParentName(mesh, actual.parent) + "'");
        }

        double maxDifference = 0.0;
        int differenceRow = 0;
        int differenceColumn = 0;
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                const double difference = std::abs(
                    static_cast<double>(expected.restLocal(row, column)) -
                    static_cast<double>(actual.restLocal(row, column)));
                if (difference > maxDifference)
                {
                    maxDifference = difference;
                    differenceRow = row;
                    differenceColumn = column;
                }
            }
        }
        if (maxDifference > RestLocalTolerance)
        {
            throw std::runtime_error(prefix + " restLocal mismatch against FBX '" +
                std::string(fbxPath) + "': max |difference|=" +
                std::to_string(maxDifference) + " at [" + std::to_string(differenceRow) +
                "][" + std::to_string(differenceColumn) + "], skeleton=" +
                std::to_string(expected.restLocal(differenceRow, differenceColumn)) +
                " fbx=" +
                std::to_string(actual.restLocal(differenceRow, differenceColumn)) +
                ", tolerance=" + std::to_string(RestLocalTolerance));
        }
    }
}
}
