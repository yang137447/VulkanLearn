#include "assimpSkeletalImporter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/scene.h>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>

#include "../common/mikkTSpaceMeshGenerator.h"

namespace VL::Animation
{
namespace
{
constexpr unsigned MaxNodes = 4096;
constexpr unsigned MaxVertices = 1000000;
constexpr unsigned MaxKeys = 1000000;
constexpr float WeightTolerance = 0.0001f;
using Nodes = std::map<std::string, const aiNode*>;

Eigen::Matrix4f Matrix(const aiMatrix4x4& value)
{
    Eigen::Matrix4f result;
    result << value.a1, value.a2, value.a3, value.a4,
        value.b1, value.b2, value.b3, value.b4,
        value.c1, value.c2, value.c3, value.c4,
        value.d1, value.d2, value.d3, value.d4;
    if (!result.allFinite())
    {
        throw std::runtime_error("Non-finite source transform");
    }
    return result;
}

Eigen::Matrix4f Convert(const aiMatrix4x4& value)
{
    const Eigen::Matrix4f basis = FbxToVlBasis();
    return basis * Matrix(value) * basis.transpose();
}

Eigen::Matrix4f Model(const aiNode& node)
{
    aiMatrix4x4 result = node.mTransformation;
    for (auto parent = node.mParent; parent; parent = parent->mParent)
    {
        result = parent->mTransformation * result;
    }
    return Convert(result);
}

ozz::math::Transform Transform(const Eigen::Matrix4f& matrix)
{
    ozz::math::Transform result;
    const Eigen::Vector3f translation = matrix.block<3, 1>(0, 3);
    Eigen::Matrix3f rotation = matrix.block<3, 3>(0, 0);
    const Eigen::Vector3f scale(rotation.col(0).norm(), rotation.col(1).norm(), rotation.col(2).norm());
    if (!matrix.allFinite() || scale.minCoeff() < 0.000001f)
    {
        throw std::runtime_error("Non-finite or singular TRS");
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        rotation.col(axis) /= scale[axis];
    }
    // ozz 片段是 TRS；不把 shear/镜像悄悄分解成另一种姿态。
    if ((rotation.transpose() * rotation - Eigen::Matrix3f::Identity()).cwiseAbs().maxCoeff() > 0.0001f ||
        rotation.determinant() < 0 ||
        (matrix.row(3) - Eigen::RowVector4f(0, 0, 0, 1)).cwiseAbs().maxCoeff() > 0.00001f)
    {
        throw std::runtime_error("Sheared, reflected or non-affine skeleton transform");
    }
    const Eigen::Quaternionf quaternion(rotation);
    result.translation = {translation.x(), translation.y(), translation.z()};
    result.rotation = {quaternion.x(), quaternion.y(), quaternion.z(), quaternion.w()};
    result.scale = {scale.x(), scale.y(), scale.z()};
    return result;
}

void CheckTree(const aiNode& node, unsigned depth, unsigned& count)
{
    if (depth > 256 || ++count > MaxNodes)
    {
        throw std::runtime_error("Skeleton node/depth budget exceeded");
    }
    Matrix(node.mTransformation);
    for (unsigned child = 0; child < node.mNumChildren; ++child)
    {
        CheckTree(*node.mChildren[child], depth + 1, count);
    }
}

void FindNodes(const aiNode& node, const std::string& name, std::vector<const aiNode*>& matches)
{
    if (node.mName.C_Str() == name)
    {
        matches.push_back(&node);
    }
    for (unsigned child = 0; child < node.mNumChildren; ++child)
    {
        FindNodes(*node.mChildren[child], name, matches);
    }
}

const aiNode& FindRoot(const aiScene& scene, const std::string& name)
{
    if (!scene.mRootNode || name.empty())
    {
        throw std::runtime_error("Missing skeletal scene/root name");
    }
    unsigned count = 0;
    CheckTree(*scene.mRootNode, 0, count);
    std::vector<const aiNode*> matches;
    FindNodes(*scene.mRootNode, name, matches);
    if (matches.size() != 1)
    {
        throw std::runtime_error("Missing or ambiguous skeleton root: " + name);
    }
    return *matches.front();
}

void ReadHierarchy(
    const aiNode& node, int parent, SkeletonMesh& mesh,
    ozz::animation::offline::RawSkeleton::Joint& raw, Nodes& nodes)
{
    const std::string name = node.mName.C_Str();
    if (name.empty() || name.rfind("VB ", 0) == 0 || !nodes.emplace(name, &node).second ||
        mesh.joints.size() >= ozz::animation::Skeleton::kMaxJoints)
    {
        throw std::runtime_error("Invalid, virtual, duplicate or excessive physical joint: " + name);
    }
    Joint joint;
    joint.name = name;
    joint.parent = parent;
    // 包装节点的静态变换折入骨架根；子骨只转换局部基底，避免每层重复套包装。
    joint.restLocal = parent < 0 ? Model(node) : Convert(node.mTransformation);
    joint.restModel = parent < 0 ? joint.restLocal : mesh.joints[parent].restModel * joint.restLocal;
    raw.name = name.c_str();
    raw.transform = Transform(joint.restLocal);
    const int index = static_cast<int>(mesh.joints.size());
    mesh.joints.push_back(joint);
    raw.children.resize(node.mNumChildren);
    for (unsigned child = 0; child < node.mNumChildren; ++child)
    {
        ReadHierarchy(*node.mChildren[child], index, mesh, raw.children[child], nodes);
    }
}

std::map<std::string, uint32_t> JointIndices(const SkeletonMesh& mesh)
{
    std::map<std::string, uint32_t> result;
    for (uint32_t index = 0; index < mesh.joints.size(); ++index)
    {
        result.emplace(mesh.joints[index].name, index);
    }
    return result;
}

Eigen::Vector3f Vector(const aiVector3D& value)
{
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
    {
        throw std::runtime_error("Non-finite vertex or animation value");
    }
    return {value.x, value.z, -value.y};
}

SkinSection ReadSection(
    const aiMesh& source, const aiNode& owner, const aiScene& scene, const SkeletonMesh& mesh)
{
    if (!source.mVertices || !source.HasNormals() || !source.HasTextureCoords(0) ||
        !source.HasBones() || source.mNumVertices > MaxVertices || source.mNumFaces > MaxVertices ||
        source.mNumBones > mesh.joints.size())
    {
        throw std::runtime_error("Skinned section requires bounded geometry, normals, UV0 and weights");
    }
    SkinSection section;
    section.name = source.mName.C_Str();
    aiString materialName;
    if (source.mMaterialIndex < scene.mNumMaterials)
    {
        scene.mMaterials[source.mMaterialIndex]->Get(AI_MATKEY_NAME, materialName);
    }
    section.materialSlot = materialName.length ? materialName.C_Str() : "Default";
    section.meshToModel = Model(owner);
    Transform(section.meshToModel);
    section.inverseBinds.resize(mesh.joints.size(), Eigen::Matrix4f::Identity());
    const auto indices = JointIndices(mesh);
    std::set<uint32_t> weightedJoints;
    std::vector<std::vector<Influence>> weights(source.mNumVertices);
    for (unsigned boneIndex = 0; boneIndex < source.mNumBones; ++boneIndex)
    {
        const auto& bone = *source.mBones[boneIndex];
        const auto found = indices.find(bone.mName.C_Str());
        if (found == indices.end() || !weightedJoints.insert(found->second).second)
        {
            throw std::runtime_error("Missing or duplicate skin joint: " + std::string(bone.mName.C_Str()));
        }
        const uint32_t joint = found->second;
        section.inverseBinds[joint] = Convert(bone.mOffsetMatrix);
        if ((section.inverseBinds[joint].row(3) -
            Eigen::RowVector4f(0, 0, 0, 1)).cwiseAbs().maxCoeff() > 0.00001f)
        {
            throw std::runtime_error("Non-affine inverse bind");
        }
        if (bone.mNumWeights)
        {
            const Eigen::Matrix4f bound = mesh.joints[joint].restModel * section.inverseBinds[joint];
            section.maxBindPositionErrorCm = std::max(section.maxBindPositionErrorCm,
                (bound.block<3, 1>(0, 3) - section.meshToModel.block<3, 1>(0, 3)).norm());
            section.maxBindLinearError = std::max(section.maxBindLinearError,
                (bound.block<3, 3>(0, 0) - section.meshToModel.block<3, 3>(0, 0)).cwiseAbs().maxCoeff());
        }
        std::set<unsigned> vertices;
        for (unsigned weightIndex = 0; weightIndex < bone.mNumWeights; ++weightIndex)
        {
            const auto& weight = bone.mWeights[weightIndex];
            if (weight.mVertexId >= source.mNumVertices || !std::isfinite(weight.mWeight) ||
                weight.mWeight < 0 || !vertices.insert(weight.mVertexId).second)
            {
                throw std::runtime_error("Invalid/duplicate vertex weight");
            }
            if (weight.mWeight > 0)
            {
                weights[weight.mVertexId].push_back({joint, weight.mWeight});
            }
        }
    }
    if (section.maxBindPositionErrorCm > 0.1f || section.maxBindLinearError > 0.0001f)
    {
        throw std::runtime_error("Inverse bind does not reconstruct mesh model space");
    }
    for (unsigned vertexIndex = 0; vertexIndex < source.mNumVertices; ++vertexIndex)
    {
        const auto& influences = weights[vertexIndex];
        double sum = 0;
        for (const auto& influence : influences)
        {
            sum += influence.weight;
        }
        section.maxWeightSumError = std::max(section.maxWeightSumError, static_cast<float>(std::abs(sum - 1)));
        section.maxInfluences = std::max(section.maxInfluences, static_cast<uint32_t>(influences.size()));
        if (influences.empty() || std::abs(sum - 1) > WeightTolerance)
        {
            throw std::runtime_error("Unweighted vertex or non-normalized source weights");
        }
        const auto position = Vector(source.mVertices[vertexIndex]);
        const auto normal = Vector(source.mNormals[vertexIndex]);
        if (normal.squaredNorm() < 0.000001f)
        {
            throw std::runtime_error("Degenerate source normal");
        }
        // 颜色与两套 UV 保留源语义；不翻 V，也不拿 UV0 伪造 UV1。
        Vertex vertex(position, normal, Eigen::Vector4f::Ones(),
            {source.mTextureCoords[0][vertexIndex].x, source.mTextureCoords[0][vertexIndex].y});
        if (source.HasTextureCoords(1))
        {
            vertex.texCoord1 = {source.mTextureCoords[1][vertexIndex].x, source.mTextureCoords[1][vertexIndex].y};
        }
        if (source.HasVertexColors(0))
        {
            const auto& color = source.mColors[0][vertexIndex];
            vertex.color = {color.r, color.g, color.b, color.a};
        }
        if (!vertex.texCoord.allFinite() || !vertex.texCoord1.allFinite() || !vertex.color.allFinite())
        {
            throw std::runtime_error("Non-finite source color/UV");
        }
        section.vertices.push_back(vertex);
    }
    for (unsigned faceIndex = 0; faceIndex < source.mNumFaces; ++faceIndex)
    {
        const auto& face = source.mFaces[faceIndex];
        if (face.mNumIndices != 3)
        {
            throw std::runtime_error("Skeletal import requires triangulated source faces");
        }
        for (unsigned corner = 0; corner < 3; ++corner)
        {
            if (face.mIndices[corner] >= source.mNumVertices)
            {
                throw std::runtime_error("Invalid triangle index");
            }
            section.indices.push_back(face.mIndices[corner]);
        }
    }
    // 沿用静态导入的意图：MikkTSpace 按源绕序生成 tangent.w，最后才适配 Vulkan CW。
    if (!MikkTSpaceMeshGenerator::Generate(section.vertices, section.indices, &section.sourceVertices))
    {
        throw std::runtime_error("Skeletal MikkTSpace generation failed");
    }
    section.influenceOffsets.push_back(0);
    for (size_t vertex = 0; vertex < section.vertices.size(); ++vertex)
    {
        if (!section.vertices[vertex].tangent.allFinite())
        {
            throw std::runtime_error("Non-finite generated tangent");
        }
        const auto& sourceWeights = weights[section.sourceVertices[vertex]];
        section.influences.insert(section.influences.end(), sourceWeights.begin(), sourceWeights.end());
        section.influenceOffsets.push_back(static_cast<uint32_t>(section.influences.size()));
        const Eigen::Vector4f point = section.vertices[vertex].position.homogeneous();
        Eigen::Vector4f skinned = Eigen::Vector4f::Zero();
        for (const auto& influence : sourceWeights)
        {
            skinned += influence.weight * mesh.joints[influence.joint].restModel *
                section.inverseBinds[influence.joint] * point;
        }
        section.maxBindSkinErrorCm = std::max(section.maxBindSkinErrorCm,
            (skinned - section.meshToModel * point).head<3>().norm());
    }
    if (section.maxBindSkinErrorCm > 0.1f)
    {
        throw std::runtime_error("Bind-pose CPU skinning residual exceeds 0.1 cm");
    }
    for (size_t face = 0; face < section.indices.size(); face += 3)
    {
        std::swap(section.indices[face], section.indices[face + 2]);
    }
    // 导入期一次算好按关节聚类的绑定空间 AABB；运行期姿态包围盒只需变换
    // O(joints×8) 个角点，不再逐顶点蒙皮。失败会直接拒绝资产。
    ComputeJointBindBounds(section);
    return section;
}

void ReadSections(const aiNode& node, const aiScene& scene, SkeletonMesh& mesh)
{
    for (unsigned index = 0; index < node.mNumMeshes; ++index)
    {
        if (node.mMeshes[index] >= scene.mNumMeshes)
        {
            throw std::runtime_error("Invalid mesh owner index");
        }
        mesh.sections.push_back(ReadSection(*scene.mMeshes[node.mMeshes[index]], node, scene, mesh));
    }
    for (unsigned child = 0; child < node.mNumChildren; ++child)
    {
        ReadSections(*node.mChildren[child], scene, mesh);
    }
}

void Configure(Assimp::Importer& importer, const std::filesystem::path& path)
{
    if (path.extension() != ".fbx" || !std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > 512ull * 1024 * 1024)
    {
        throw std::runtime_error("Expected a bounded FBX source file");
    }
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_REMOVE_EMPTY_BONES, false);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_IGNORE_UP_DIRECTION, true);
}

void ValidateUnits(const aiScene& scene)
{
    double unit = 0;
    float unitFloat = 0;
    int up = -1, upSign = 0, front = -1, frontSign = 0, coord = -1, coordSign = 0;
    // 当前 Assimp FBX 以 float 发布单位；合成场景也允许语义相同的 double，不猜缺失值。
    if (!scene.mMetaData) throw std::runtime_error("Missing FBX axis/unit metadata");
    if (!scene.mMetaData->Get("UnitScaleFactor", unit))
    {
        if (!scene.mMetaData->Get("UnitScaleFactor", unitFloat))
        {
            throw std::runtime_error("Missing FBX UnitScaleFactor");
        }
        unit = unitFloat;
    }
    if (unit != 1 ||
        !scene.mMetaData->Get("UpAxis", up) || up != 2 ||
        !scene.mMetaData->Get("UpAxisSign", upSign) || upSign != 1 ||
        !scene.mMetaData->Get("FrontAxis", front) || front != 1 ||
        !scene.mMetaData->Get("FrontAxisSign", frontSign) || frontSign != -1 ||
        !scene.mMetaData->Get("CoordAxis", coord) || coord != 0 ||
        !scene.mMetaData->Get("CoordAxisSign", coordSign) || coordSign != 1)
    {
        throw std::runtime_error("Source does not match ue-fbx-cm-yup-v1 axis/unit metadata");
    }
}

float KeyTime(double tick, double rate, float duration)
{
    const double time = tick / rate;
    if (!std::isfinite(time) || time < 0 || time > static_cast<double>(duration) + 0.000001)
    {
        throw std::runtime_error("Animation key outside duration");
    }
    // FBX double 时间转 float 与时长共用舍入；只吸收端点的表示误差。
    return std::min(static_cast<float>(time), duration);
}

void ReadTrack(
    const aiNodeAnim& channel, ozz::animation::offline::RawAnimation::JointTrack& track,
    double rate, float duration)
{
    if (channel.mNumPositionKeys > MaxKeys || channel.mNumRotationKeys > MaxKeys ||
        channel.mNumScalingKeys > MaxKeys || channel.mPreState != aiAnimBehaviour_DEFAULT ||
        channel.mPostState != aiAnimBehaviour_DEFAULT)
    {
        throw std::runtime_error("Unsupported animation channel budget/behavior");
    }
    for (unsigned key = 0; key < channel.mNumPositionKeys; ++key)
    {
        const auto value = Vector(channel.mPositionKeys[key].mValue);
        track.translations.push_back({KeyTime(channel.mPositionKeys[key].mTime, rate, duration),
            {value.x(), value.y(), value.z()}});
    }
    const Eigen::Matrix3f basis = FbxToVlBasis().block<3, 3>(0, 0);
    for (unsigned key = 0; key < channel.mNumRotationKeys; ++key)
    {
        const auto& source = channel.mRotationKeys[key].mValue;
        const Eigen::Quaternionf quaternion(source.w, source.x, source.y, source.z);
        if (!quaternion.coeffs().allFinite() || std::abs(quaternion.norm() - 1) > 0.0001f)
        {
            throw std::runtime_error("Invalid source rotation quaternion");
        }
        const Eigen::Quaternionf value(basis * quaternion.toRotationMatrix() * basis.transpose());
        track.rotations.push_back({KeyTime(channel.mRotationKeys[key].mTime, rate, duration),
            {value.x(), value.y(), value.z(), value.w()}});
    }
    for (unsigned key = 0; key < channel.mNumScalingKeys; ++key)
    {
        const auto& value = channel.mScalingKeys[key].mValue;
        Vector(value);
        if (std::min({value.x, value.y, value.z}) <= 0)
        {
            throw std::runtime_error("Non-positive animated scale");
        }
        track.scales.push_back({KeyTime(channel.mScalingKeys[key].mTime, rate, duration),
            {value.x, value.z, value.y}});
    }
}

void FillMissing(
    ozz::animation::offline::RawAnimation::JointTrack& track, const Eigen::Matrix4f& rest)
{
    const auto transform = Transform(rest);
    if (track.translations.empty()) track.translations.push_back({0, transform.translation});
    if (track.rotations.empty()) track.rotations.push_back({0, transform.rotation});
    if (track.scales.empty()) track.scales.push_back({0, transform.scale});
}

template<typename Keys>
void CheckCoverage(const Keys& keys, float duration)
{
    // ozz 对区间外取首尾值，Assimp DEFAULT 则可能回到绑定值。
    // 固定导出策略要求非空多 key 通道覆盖完整区间，拒绝两种外推语义不一致的源。
    if (!keys.empty() && (keys.front().time != 0 ||
        (keys.size() > 1 && keys.back().time != duration)))
    {
        throw std::runtime_error("Animated component does not cover the full clip interval");
    }
}
}

SkeletonMesh AssimpSkeletalImporter::ReadMesh(const std::filesystem::path& path, const std::string& root)
{
    Assimp::Importer importer;
    Configure(importer, path);
    // 不启用 GlobalScale、LimitBoneWeights、节点烘焙或轴向自动校正；转换由一个明确基底拥有。
    const auto* scene = importer.ReadFile(path.u8string(), 0);
    if (!scene) throw std::runtime_error("Skeletal FBX import failed: " + std::string(importer.GetErrorString()));
    return BuildMesh(*scene, root);
}

SkeletonMesh AssimpSkeletalImporter::BuildMesh(const aiScene& scene, const std::string& root)
{
    ValidateUnits(scene);
    const auto& node = FindRoot(scene, root);
    SkeletonMesh mesh;
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.resize(1);
    Nodes nodes;
    ReadHierarchy(node, -1, mesh, raw.roots[0], nodes);
    ozz::animation::offline::SkeletonBuilder builder;
    mesh.runtime = builder(raw);
    if (!mesh.runtime) throw std::runtime_error("ozz skeleton build failed");
    // ozz 当前以深度优先排列；不把依赖的隐式顺序当作永久契约。
    for (size_t index = 0; index < mesh.joints.size(); ++index)
    {
        if (mesh.joints[index].name != mesh.runtime->joint_names()[index] ||
            mesh.joints[index].parent != mesh.runtime->joint_parents()[index])
        {
            throw std::runtime_error("ozz skeleton joint mapping changed");
        }
    }
    ReadSections(*scene.mRootNode, scene, mesh);
    if (mesh.sections.empty()) throw std::runtime_error("No skinned mesh sections");
    return mesh;
}

Clip AssimpSkeletalImporter::ReadClip(
    const std::filesystem::path& path, const SkeletonMesh& mesh, const std::string& id)
{
    Assimp::Importer importer;
    Configure(importer, path);
    const auto* scene = importer.ReadFile(path.u8string(), 0);
    if (!scene) throw std::runtime_error("Animation FBX import failed: " + std::string(importer.GetErrorString()));
    return BuildClip(*scene, mesh, id);
}

Clip AssimpSkeletalImporter::BuildClip(const aiScene& scene, const SkeletonMesh& mesh, const std::string& id)
{
    if (mesh.joints.empty() || id.empty() || scene.mNumAnimations != 1)
    {
        throw std::runtime_error("Expected named clip, imported skeleton and one animation");
    }
    ValidateUnits(scene);
    const auto& root = FindRoot(scene, mesh.joints.front().name);
    SkeletonMesh clipHierarchy;
    ozz::animation::offline::RawSkeleton rawSkeleton;
    rawSkeleton.roots.resize(1);
    Nodes nodes;
    ReadHierarchy(root, -1, clipHierarchy, rawSkeleton.roots[0], nodes);
    const auto indices = JointIndices(mesh);
    if (nodes.size() != indices.size()) throw std::runtime_error("Clip skeleton size mismatch");
    for (size_t joint = 0; joint < mesh.joints.size(); ++joint)
    {
        const auto found = nodes.find(mesh.joints[joint].name);
        if (found == nodes.end() || (mesh.joints[joint].parent >= 0 &&
            found->second->mParent->mName.C_Str() != mesh.joints[mesh.joints[joint].parent].name))
        {
            throw std::runtime_error("Clip skeleton name/parent mismatch");
        }
    }
    const auto& source = *scene.mAnimations[0];
    if (!std::isfinite(source.mDuration) || !std::isfinite(source.mTicksPerSecond) ||
        source.mDuration <= 0 || source.mTicksPerSecond <= 0 ||
        source.mDuration / source.mTicksPerSecond > 600 ||
        source.mNumMeshChannels || source.mNumMorphMeshChannels ||
        source.mNumChannels > mesh.joints.size())
    {
        throw std::runtime_error("Invalid animation time unit/budget or unsupported mesh channels");
    }
    Clip clip;
    clip.id = id;
    clip.raw.name = id.c_str();
    clip.raw.duration = static_cast<float>(source.mDuration / source.mTicksPerSecond);
    clip.raw.tracks.resize(mesh.joints.size());
    std::set<uint32_t> authored;
    for (unsigned channelIndex = 0; channelIndex < source.mNumChannels; ++channelIndex)
    {
        const auto& channel = *source.mChannels[channelIndex];
        const auto found = indices.find(channel.mNodeName.C_Str());
        if (found == indices.end() || !authored.insert(found->second).second)
        {
            throw std::runtime_error("Unknown or duplicate animation channel");
        }
        ReadTrack(channel, clip.raw.tracks[found->second], source.mTicksPerSecond, clip.raw.duration);
        const auto& track = clip.raw.tracks[found->second];
        CheckCoverage(track.translations, clip.raw.duration);
        CheckCoverage(track.rotations, clip.raw.duration);
        CheckCoverage(track.scales, clip.raw.duration);
    }
    // 首版只允许 identity 包装：否则根轨道的 TRS 需要烘焙父矩阵，不能只变换平移。
    const Eigen::Matrix4f wrapper = root.mParent ? Model(*root.mParent) : Eigen::Matrix4f::Identity();
    if ((wrapper - Eigen::Matrix4f::Identity()).cwiseAbs().maxCoeff() > 0.0001f)
    {
        throw std::runtime_error("Animated clip requires identity root wrapper");
    }
    for (size_t joint = 0; joint < mesh.joints.size(); ++joint)
    {
        if (!authored.count(static_cast<uint32_t>(joint)))
        {
            clip.restFallbackJoints.push_back(mesh.joints[joint].name);
        }
        // 源文件缺轨/缺 TRS 分量时回退绑定姿态，不能使用 ozz 空通道默认的 identity。
        FillMissing(clip.raw.tracks[joint], mesh.joints[joint].restLocal);
    }
    if (!clip.raw.Validate()) throw std::runtime_error("Unsorted, duplicate or out-of-range animation keys");
    ozz::animation::offline::AnimationBuilder builder;
    clip.runtime = builder(clip.raw);
    if (!clip.runtime) throw std::runtime_error("ozz animation build failed");
    return clip;
}
}
