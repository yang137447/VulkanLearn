#include "fbxInspection.h"

#include <algorithm>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/scene.h>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{
using Json = nlohmann::json;

Json Vector(const aiVector3D& value)
{
    return {value.x, value.y, value.z};
}

Json Matrix(const aiMatrix4x4& value)
{
    return {{value.a1, value.a2, value.a3, value.a4},
        {value.b1, value.b2, value.b3, value.b4},
        {value.c1, value.c2, value.c3, value.c4},
        {value.d1, value.d2, value.d3, value.d4}};
}

void ReadNode(const aiNode& node, const aiMatrix4x4& parent, int parentIndex, Json& nodes)
{
    const aiMatrix4x4 model = parent * node.mTransformation;
    const auto nodeIndex = static_cast<int>(nodes.size());
    Json meshIndices = Json::array();
    for (unsigned int index = 0; index < node.mNumMeshes; ++index)
    {
        meshIndices.push_back(node.mMeshes[index]);
    }
    // FBX 的网格节点与 armature 允许同名；数值身份保留父子与 mesh 绑定，名称仅供审阅。
    nodes.push_back({{"name", node.mName.C_Str()},
        {"index", nodeIndex}, {"parentIndex", parentIndex}, {"meshIndices", std::move(meshIndices)},
        {"parent", node.mParent ? node.mParent->mName.C_Str() : ""},
        {"local", Matrix(node.mTransformation)}, {"model", Matrix(model)}});
    for (unsigned int index = 0; index < node.mNumChildren; ++index)
    {
        ReadNode(*node.mChildren[index], model, nodeIndex, nodes);
    }
}

Json ReadMesh(const aiMesh& mesh)
{
    Json result = {{"name", mesh.mName.C_Str()}, {"vertices", mesh.mNumVertices},
        {"faces", mesh.mNumFaces}, {"materialIndex", mesh.mMaterialIndex},
        {"bones", Json::array()}};
    std::vector<double> sums(mesh.mNumVertices, 0.0);
    std::vector<unsigned int> influences(mesh.mNumVertices, 0);
    unsigned int invalidWeights = 0;
    for (unsigned int index = 0; index < mesh.mNumBones; ++index)
    {
        const auto& bone = *mesh.mBones[index];
        result["bones"].push_back({{"name", bone.mName.C_Str()},
            {"weights", bone.mNumWeights}, {"offset", Matrix(bone.mOffsetMatrix)}});
        for (unsigned int weightIndex = 0; weightIndex < bone.mNumWeights; ++weightIndex)
        {
            const auto& weight = bone.mWeights[weightIndex];
            if (weight.mVertexId >= mesh.mNumVertices ||
                !std::isfinite(weight.mWeight) || weight.mWeight < 0.0f)
            {
                ++invalidWeights;
                continue;
            }
            sums[weight.mVertexId] += weight.mWeight;
            if (weight.mWeight > 0.0f)
            {
                ++influences[weight.mVertexId];
            }
        }
    }
    double maxError = 0.0;
    unsigned int unweightedVertices = 0;
    unsigned int maxInfluences = 0;
    for (unsigned int index = 0; index < mesh.mNumVertices; ++index)
    {
        maxError = std::max(maxError, std::abs(sums[index] - 1.0));
        maxInfluences = std::max(maxInfluences, influences[index]);
        if (influences[index] == 0)
        {
            ++unweightedVertices;
        }
    }
    result["invalidWeights"] = invalidWeights;
    result["unweightedVertices"] = unweightedVertices;
    result["maxWeightSumError"] = maxError;
    result["maxInfluences"] = maxInfluences;
    return result;
}

Json ReadAnimation(const aiAnimation& animation)
{
    Json result = {{"name", animation.mName.C_Str()},
        {"durationTicks", animation.mDuration}, {"ticksPerSecond", animation.mTicksPerSecond},
        {"channels", Json::array()}};
    for (unsigned int index = 0; index < animation.mNumChannels; ++index)
    {
        const auto& channel = *animation.mChannels[index];
        Json positions = Json::array();
        Json rotations = Json::array();
        Json scales = Json::array();
        for (unsigned int key = 0; key < channel.mNumPositionKeys; ++key)
        {
            positions.push_back({{"tick", channel.mPositionKeys[key].mTime},
                {"value", Vector(channel.mPositionKeys[key].mValue)}});
        }
        for (unsigned int key = 0; key < channel.mNumRotationKeys; ++key)
        {
            const auto& value = channel.mRotationKeys[key].mValue;
            rotations.push_back({{"tick", channel.mRotationKeys[key].mTime},
                {"valueXyzw", {value.x, value.y, value.z, value.w}}});
        }
        for (unsigned int key = 0; key < channel.mNumScalingKeys; ++key)
        {
            scales.push_back({{"tick", channel.mScalingKeys[key].mTime},
                {"value", Vector(channel.mScalingKeys[key].mValue)}});
        }
        result["channels"].push_back({{"name", channel.mNodeName.C_Str()},
            {"positions", std::move(positions)}, {"rotations", std::move(rotations)},
            {"scales", std::move(scales)}});
    }
    return result;
}
}

namespace VL::AnimationPreflight
{
nlohmann::json InspectFbx(const std::filesystem::path& path)
{
    if (path.extension() != ".fbx")
    {
        throw std::runtime_error("FBX inspection requires a .fbx file");
    }
    Assimp::Importer importer;
    // 预检保留 FBX 原始单位/轴向，不套静态导入器的 GlobalScale 或裁剪权重；
    // 每个辅助节点和原始关键帧都输出，供 UE 侧车对照，不冒充正式骨骼导入。
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_REMOVE_EMPTY_BONES, false);
    // Assimp 6 默认只为含网格 FBX 校正根轴向；动画单独导出不会走该路径。
    // 预检统一保留文件基底，避免网格与动画被悄悄变换到两个不同空间。
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_IGNORE_UP_DIRECTION, true);
    const aiScene* scene = importer.ReadFile(path.u8string(), 0);
    // 上游明确用 INCOMPLETE 标记无几何的动画/骨架，不能照搬静态网格的拒绝条件。
    if (!scene || !scene->mRootNode)
    {
        throw std::runtime_error("FBX inspection failed: " + std::string(importer.GetErrorString()));
    }
    Json result = {{"format", "vl.animation.fbx-inspection.v1"},
        {"source", path.u8string()}, {"postprocessFlags", 0}, {"preservePivots", false},
        {"ignoreUpDirection", true}, {"removeEmptyBones", false}, {"sceneFlags", scene->mFlags},
        {"nodes", Json::array()}, {"meshes", Json::array()}, {"animations", Json::array()}};
    ReadNode(*scene->mRootNode, aiMatrix4x4(), -1, result["nodes"]);
    for (unsigned int index = 0; index < scene->mNumMeshes; ++index)
    {
        result["meshes"].push_back(ReadMesh(*scene->mMeshes[index]));
    }
    for (unsigned int index = 0; index < scene->mNumAnimations; ++index)
    {
        result["animations"].push_back(ReadAnimation(*scene->mAnimations[index]));
    }
    for (const char* key : {"UnitScaleFactor", "OriginalUnitScaleFactor",
        "UpAxis", "UpAxisSign", "FrontAxis", "FrontAxisSign", "CoordAxis", "CoordAxisSign"})
    {
        double real = 0.0;
        float scalar = 0.0f;
        int integer = 0;
        if (scene->mMetaData && scene->mMetaData->Get(key, real))
        {
            result["fbxMetadata"][key] = real;
        }
        else if (scene->mMetaData && scene->mMetaData->Get(key, scalar))
        {
            result["fbxMetadata"][key] = scalar;
        }
        else if (scene->mMetaData && scene->mMetaData->Get(key, integer))
        {
            result["fbxMetadata"][key] = integer;
        }
    }
    return result;
}
}
