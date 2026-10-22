#include <assimp/Exporter.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

#include "animation/animationSkeleton.h"
#include "mesh/loader/animation/assimpSkeletalImporter.h"
#include "shader/build/contentHash.h"

namespace
{
using Json = nlohmann::json;
using VL::Animation::AssimpSkeletalImporter;

Eigen::Matrix4f Matrix(const Json& values)
{
    if (!values.is_array() || values.size() != 16)
        throw std::runtime_error("Expected row-major 4x4");
    Eigen::Matrix4f result;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            result(row, column) = values.at(row * 4 + column).get<float>();
    if (!result.allFinite()) throw std::runtime_error("Nonfinite matrix");
    return result;
}

aiMatrix4x4 FbxMatrix(const Json& values)
{
    // Maya 保存矩阵已由提取器转成 VL/Y-up 列向量；导出实际改轴，不能只修改 FBX metadata。
    const auto basis = VL::Animation::FbxToVlBasis();
    const Eigen::Matrix4f matrix = basis.transpose() * Matrix(values) * basis;
    return aiMatrix4x4(matrix(0, 0), matrix(0, 1), matrix(0, 2), matrix(0, 3),
        matrix(1, 0), matrix(1, 1), matrix(1, 2), matrix(1, 3),
        matrix(2, 0), matrix(2, 1), matrix(2, 2), matrix(2, 3),
        matrix(3, 0), matrix(3, 1), matrix(3, 2), matrix(3, 3));
}

void SetChildren(aiNode& parent, const std::vector<aiNode*>& children)
{
    parent.mNumChildren = static_cast<unsigned>(children.size());
    parent.mChildren = new aiNode*[children.size()];
    for (size_t index = 0; index < children.size(); ++index)
    {
        parent.mChildren[index] = children[index];
        children[index]->mParent = &parent;
    }
}

Json SplitMeshes(const Json& meshes)
{
    Json result = Json::array();
    for (const auto& source : meshes)
    {
        size_t firstFace = 0;
        while (firstFace < source.at("faces").size())
        {
            Json part = {{"name", source.at("name")}, {"inverseBinds", source.at("inverseBinds")},
                {"vertices", Json::array()}, {"uv", Json::array()},
                {"weights", Json::array()}, {"faces", Json::array()}};
            std::map<unsigned, unsigned> remap;
            size_t triangles = 0;
            // FBX 回读按三角形角点展开；每块最多 60 万角点，不改变引擎单 section 预算。
            while (firstFace < source.at("faces").size())
            {
                const auto& face = source.at("faces").at(firstFace);
                if (triangles + face.size() - 2 > 200000 && triangles > 0) break;
                triangles += face.size() - 2;
                Json indices = Json::array();
                for (const auto& value : face)
                {
                    const unsigned original = value.get<unsigned>();
                    const auto entry = remap.emplace(original, static_cast<unsigned>(remap.size()));
                    if (entry.second)
                    {
                        part["vertices"].push_back(source.at("vertices").at(original));
                        part["uv"].push_back(source.at("uv").at(original));
                        part["weights"].push_back(source.at("weights").at(original));
                    }
                    indices.push_back(entry.first->second);
                }
                part["faces"].push_back(std::move(indices));
                ++firstFace;
            }
            result.push_back(std::move(part));
        }
    }
    return result;
}

std::unique_ptr<aiScene> Scene(const Json& input)
{
    auto scene = std::make_unique<aiScene>();
    scene->mRootNode = new aiNode("FemaleBindScene");
    scene->mMetaData = aiMetadata::Alloc(7);
    scene->mMetaData->Set(0, "UnitScaleFactor", 1.0);
    scene->mMetaData->Set(1, "UpAxis", int32_t(2));
    scene->mMetaData->Set(2, "UpAxisSign", int32_t(1));
    scene->mMetaData->Set(3, "FrontAxis", int32_t(1));
    scene->mMetaData->Set(4, "FrontAxisSign", int32_t(-1));
    scene->mMetaData->Set(5, "CoordAxis", int32_t(0));
    scene->mMetaData->Set(6, "CoordAxisSign", int32_t(1));
    const auto& joints = input.at("joints");
    std::vector<aiNode*> nodes;
    std::vector<std::vector<aiNode*>> children(joints.size());
    std::vector<aiNode*> top;
    for (size_t index = 0; index < joints.size(); ++index)
    {
        const auto& joint = joints.at(index);
        auto node = new aiNode(joint.at("name").get<std::string>());
        node->mTransformation = FbxMatrix(joint.at("restLocal"));
        nodes.push_back(node);
        const int parent = joint.at("parent").get<int>();
        if (index == 0 && parent == -1) top.push_back(node);
        else if (parent >= 0 && parent < static_cast<int>(index))
            children.at(parent).push_back(node);
        else throw std::runtime_error("Invalid joint order");
    }
    for (size_t index = 0; index < nodes.size(); ++index)
        SetChildren(*nodes[index], children[index]);
    const auto meshes = SplitMeshes(input.at("meshes"));
    scene->mNumMeshes = static_cast<unsigned>(meshes.size());
    scene->mMeshes = new aiMesh*[meshes.size()]{};
    scene->mNumMaterials = scene->mNumMeshes;
    scene->mMaterials = new aiMaterial*[meshes.size()]{};
    for (size_t index = 0; index < meshes.size(); ++index)
    {
        const auto& source = meshes.at(index);
        const std::string name = source.at("name");
        auto mesh = new aiMesh();
        scene->mMeshes[index] = mesh;
        mesh->mName = aiString(name);
        mesh->mMaterialIndex = static_cast<unsigned>(index);
        auto material = new aiMaterial();
        scene->mMaterials[index] = material;
        const aiString slot(name);
        material->AddProperty(&slot, AI_MATKEY_NAME);
        mesh->mNumVertices = static_cast<unsigned>(source.at("vertices").size());
        mesh->mVertices = new aiVector3D[mesh->mNumVertices];
        mesh->mTextureCoords[0] = new aiVector3D[mesh->mNumVertices];
        mesh->mNumUVComponents[0] = 2;
        std::map<std::string, std::vector<aiVertexWeight>> weights;
        for (unsigned vertex = 0; vertex < mesh->mNumVertices; ++vertex)
        {
            const auto& position = source.at("vertices").at(vertex);
            mesh->mVertices[vertex] = aiVector3D(position.at(0).get<float>(),
                -position.at(2).get<float>(), position.at(1).get<float>());
            const auto& uv = source.at("uv").at(vertex);
            mesh->mTextureCoords[0][vertex] = aiVector3D(uv.at(0).get<float>(), uv.at(1).get<float>(), 0);
            for (const auto& influence : source.at("weights").at(vertex))
                weights[influence.at(0).get<std::string>()].emplace_back(
                    vertex, influence.at(1).get<float>());
        }
        mesh->mNumFaces = static_cast<unsigned>(source.at("faces").size());
        mesh->mFaces = new aiFace[mesh->mNumFaces];
        mesh->mPrimitiveTypes = aiPrimitiveType_POLYGON;
        for (unsigned face = 0; face < mesh->mNumFaces; ++face)
        {
            const auto& polygon = source.at("faces").at(face);
            auto& output = mesh->mFaces[face];
            output.mNumIndices = static_cast<unsigned>(polygon.size());
            output.mIndices = new unsigned[polygon.size()];
            for (size_t corner = 0; corner < polygon.size(); ++corner)
                output.mIndices[corner] = polygon.at(corner).get<unsigned>();
        }
        mesh->mNumBones = static_cast<unsigned>(weights.size());
        mesh->mBones = new aiBone*[weights.size()]{};
        unsigned boneIndex = 0;
        for (const auto& entry : weights)
        {
            auto bone = new aiBone();
            mesh->mBones[boneIndex++] = bone;
            bone->mName = aiString(entry.first);
            bone->mOffsetMatrix = FbxMatrix(source.at("inverseBinds").at(entry.first));
            bone->mNumWeights = static_cast<unsigned>(entry.second.size());
            bone->mWeights = new aiVertexWeight[entry.second.size()];
            std::copy(entry.second.begin(), entry.second.end(), bone->mWeights);
        }
        auto owner = new aiNode("Female_" + name + "_" + std::to_string(index));
        owner->mNumMeshes = 1;
        owner->mMeshes = new unsigned[1]{static_cast<unsigned>(index)};
        top.push_back(owner);
    }
    SetChildren(*scene->mRootNode, top);
    return scene;
}

Json Manifest(const VL::Animation::SkeletonMesh& mesh, const std::filesystem::path& fbx)
{
    Json joints = Json::array();
    for (const auto& joint : mesh.joints)
    {
        Json matrix = Json::array();
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                matrix.push_back(joint.restLocal(row, column));
        joints.push_back({{"name", joint.name}, {"parent", joint.parent}, {"restLocal", matrix}});
    }
    return {{"format", "vl.skeleton.v1"}, {"skeleton", "SK_Female_Outside"},
        {"root", "FemaleRoot"}, {"joints", joints}, {"slots", {"DefaultSlot"}},
        {"curves", Json::object()}, {"source", {
            {"fbx", "Maps/SC_tressfx_female/Source/" + fbx.filename().string()},
            {"fbxBlake3", VL::ContentHasher::HashFile(fbx).ToHex()},
            {"tool", "extract_female_skin.py + vl_female_fbx (Assimp)"}}}};
}

void ValidateVertexRoundtrip(const Json& input, const VL::Animation::SkeletonMesh& imported)
{
    using Key = std::array<float, 5>;
    std::map<std::string, std::map<Key, std::vector<std::map<std::string, float>>>> expected;
    std::map<std::string, std::set<Key>> seen;
    const auto& meshes = input.at("meshes");
    for (const auto& source : meshes)
        for (size_t vertex = 0; vertex < source.at("vertices").size(); ++vertex)
        {
            const auto& position = source.at("vertices").at(vertex);
            const auto& uv = source.at("uv").at(vertex);
            const Key key{position.at(0).get<float>(), position.at(1).get<float>(),
                position.at(2).get<float>(), uv.at(0).get<float>(), uv.at(1).get<float>()};
            std::map<std::string, float> weights;
            for (const auto& pair : source.at("weights").at(vertex))
                weights.emplace(pair.at(0).get<std::string>(), pair.at(1).get<float>());
            expected[source.at("name").get<std::string>()][key].push_back(std::move(weights));
        }
    for (const auto& section : imported.sections)
    {
        const auto material = expected.find(section.materialSlot);
        if (material == expected.end()) throw std::runtime_error("Unexpected FBX material slot");
        for (size_t vertex = 0; vertex < section.vertices.size(); ++vertex)
        {
            const auto& actual = section.vertices[vertex];
            const Key key{actual.position.x(), actual.position.y(), actual.position.z(),
                actual.texCoord.x(), actual.texCoord.y()};
            const auto found = material->second.find(key);
            if (found == material->second.end())
                throw std::runtime_error("FBX vertex position/UV changed");
            std::map<std::string, float> weights;
            const auto first = section.influenceOffsets.at(vertex);
            const auto last = section.influenceOffsets.at(vertex + 1);
            for (auto index = first; index < last; ++index)
            {
                const auto& influence = section.influences[index];
                weights.emplace(imported.joints.at(influence.joint).name, influence.weight);
            }
            // FBX 与 MikkTSpace 都会改顶点索引；用完整位置/UV/权重身份对照，不能假设索引不变。
            // 同位置/UV 若含不同蒙皮则拒绝，不允许 exporter 把重合但异权重的点合并。
            for (const auto& candidate : found->second)
                if (candidate != weights) throw std::runtime_error("FBX skin weight changed/ambiguous");
            seen[section.materialSlot].insert(key);
        }
    }
    for (const auto& material : expected)
        if (seen[material.first].size() != material.second.size())
            throw std::runtime_error("FBX lost a source vertex identity");
}

void Convert(const std::filesystem::path& inputPath, const std::filesystem::path& fbx,
    const std::filesystem::path& skeleton)
{
    if (std::filesystem::exists(fbx) || std::filesystem::exists(skeleton))
        throw std::runtime_error("Output exists; select fresh output paths");
    std::ifstream stream(inputPath, std::ios::binary);
    const Json input = Json::parse(stream);
    if (input.at("format") != "vl.female-bind-interchange.v1" ||
        input.at("sourceSha256") !=
            "ee1e944273f4b27004e65b2915117f281a9b585ed757f1e0a7f40f27eb71ed65")
        throw std::runtime_error("Unsupported Female interchange/source");
    auto scene = Scene(input);
    Assimp::ExportProperties properties;
    // Assimp 默认按位置合并 FBX 顶点，隐含假设同位置有相同权重；此处必须关闭，保留 UV 缝与蒙皮。
    properties.SetPropertyBool("bJoinIdenticalVertices", false);
    Assimp::Exporter exporter;
    std::filesystem::create_directories(fbx.parent_path());
    if (exporter.Export(scene.get(), "fbx", fbx.string(),
        aiProcess_Triangulate | aiProcess_GenSmoothNormals, &properties) != AI_SUCCESS)
        throw std::runtime_error(exporter.GetErrorString());
    std::cout << "FBX written; validating through production importer..." << std::endl;
    const auto imported = AssimpSkeletalImporter::ReadMesh(fbx, "FemaleRoot");
    if (imported.joints.size() != input.at("joints").size() || imported.sections.size() != 4)
        throw std::runtime_error("FBX roundtrip changed skeleton/section count");
    // SK 从往返后的浮点矩阵生成，但独立对照原始 Maya restLocal，防止清单掩盖导出轴向/层级错误。
    for (size_t index = 0; index < imported.joints.size(); ++index)
    {
        const auto& expected = input.at("joints").at(index);
        const auto& actual = imported.joints[index];
        if (actual.name != expected.at("name") || actual.parent != expected.at("parent") ||
            (actual.restLocal - Matrix(expected.at("restLocal"))).cwiseAbs().maxCoeff() > 0.0001f)
            throw std::runtime_error("Maya/FBX rest hierarchy mismatch: " + actual.name);
    }
    // 独立逐顶点核对；绑定残差很小本身并不能证明权重被完整保存。
    ValidateVertexRoundtrip(input, imported);
    const auto manifest = Manifest(imported, fbx);
    VL::Animation::ValidateSkeletonAgainstMesh(
        VL::Animation::ParseSkeleton(manifest, skeleton.string()), imported, fbx.string());
    std::filesystem::create_directories(skeleton.parent_path());
    std::ofstream output(skeleton, std::ios::binary);
    output << manifest.dump(4) << '\n';
    Json audit = input.at("audit");
    audit["vertexWeightsAndUvVerified"] = true;
    for (const auto& section : imported.sections)
        audit["roundtrip"].push_back({
            {"material", section.materialSlot},
            {"vertices", section.vertices.size()}, {"triangles", section.indices.size() / 3},
            {"maxInfluences", section.maxInfluences}, {"weightSumError", section.maxWeightSumError},
            {"bindSkinErrorCm", section.maxBindSkinErrorCm},
            {"bindPositionErrorCm", section.maxBindPositionErrorCm}});
    std::cout << audit.dump(2) << std::endl;
    std::ofstream report(inputPath.parent_path() / "fbx-roundtrip-report.json", std::ios::binary);
    report << audit.dump(2) << '\n';
}
}

int wmain(int argc, wchar_t* argv[])
{
    try
    {
        if (argc != 4)
            throw std::runtime_error("Usage: vl_female_fbx interchange.json output.fbx SK_output.json");
        Convert(argv[1], argv[2], argv[3]);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
