#include "assimpSourceAdapter.h"

#include <algorithm>
#include <assimp/Importer.hpp>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "../common/mikkTSpaceMeshGenerator.h"

namespace
{
    float ComputeBitangentSign(
        const Eigen::Vector3f& normal,
        const Eigen::Vector3f& tangent,
        const Eigen::Vector3f& bitangent)
    {
        return normal.cross(tangent).dot(bitangent) < 0.0f ? -1.0f : 1.0f;
    }

    std::string ReadMaterialSlotName(aiMesh* mesh, const aiScene* scene)
    {
        std::string materialSlotName = "Default";
        if (scene != nullptr && mesh->mMaterialIndex < scene->mNumMaterials)
        {
            aiString materialName;
            if (scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS &&
                materialName.length > 0)
            {
                materialSlotName = materialName.C_Str();
            }
        }

        if (materialSlotName.empty() || materialSlotName == "DefaultMaterial")
        {
            return "Default";
        }
        return materialSlotName;
    }

    std::string ReadSectionName(aiMesh* mesh, const std::string& materialSlotName)
    {
        if (mesh->mName.length > 0)
        {
            return mesh->mName.C_Str();
        }
        return materialSlotName;
    }
}

void AssimpSourceAdapter::ValidateSource(const std::string& sourcePath, const std::string& modelDataPath) const
{
    if (!std::filesystem::exists(sourcePath))
    {
        throw std::runtime_error("Assimp source file not found: " + modelDataPath);
    }
}

ModelResource AssimpSourceAdapter::ReadSource(
    const std::string& sourcePath,
    const std::string& modelDataPath,
    const MeshImportOptions& importOptions) const
{
    ValidateSource(sourcePath, modelDataPath);

    Assimp::Importer importer;
    unsigned int postProcessFlags = aiProcess_Triangulate;
    if (importOptions.generateSmoothNormals)
    {
        postProcessFlags |= aiProcess_GenSmoothNormals;
    }
    const aiScene* scene = importer.ReadFile(sourcePath, postProcessFlags);
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
    {
        throw std::runtime_error(
            "Assimp import failed: " + modelDataPath + " error=" + importer.GetErrorString());
    }

    ModelResource modelResource;
    ProcessNode(scene->mRootNode, scene, modelResource);

    std::vector<std::string> sourceSlots;
    for (const MeshSection& section : modelResource.sections)
    {
        const auto it = std::find(sourceSlots.begin(), sourceSlots.end(), section.materialSlotName);
        if (it == sourceSlots.end())
        {
            sourceSlots.push_back(section.materialSlotName);
        }
    }
    modelResource.sourceMaterialSlotNames = std::move(sourceSlots);
    return modelResource;
}

void AssimpSourceAdapter::ProcessNode(aiNode* node, const aiScene* scene, ModelResource& outModelResource) const
{
    // process all the node's meshes (if any)
    for (unsigned int i = 0; i < node->mNumMeshes; i++)
    {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        outModelResource.sections.push_back(ProcessMesh(mesh, scene));
    }
    // then do the same for each of its children
    for (unsigned int i = 0; i < node->mNumChildren; i++)
    {
        ProcessNode(node->mChildren[i], scene, outModelResource);
    }
}

MeshSection AssimpSourceAdapter::ProcessMesh(aiMesh* mesh, const aiScene* scene) const
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    const std::string materialSlotName = ReadMaterialSlotName(mesh, scene);
    const std::string sectionName = ReadSectionName(mesh, materialSlotName);

    // 处理顶点位置、顶点法线、颜色、纹理坐标
    for(uint32_t i = 0; i < mesh->mNumVertices; i++)
    {
        Vertex vertex;
        vertex.position = Eigen::Vector3f(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);
        if(mesh->mNormals)
        {
            vertex.normal = Eigen::Vector3f(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z);
        }
        else
        {
            vertex.normal = Eigen::Vector3f::Zero();
        }
        if(mesh->mColors[0])
        {
            vertex.color = Eigen::Vector4f(
                mesh->mColors[0][i].r,
                mesh->mColors[0][i].g,
                mesh->mColors[0][i].b,
                mesh->mColors[0][i].a);
        }
        else
        {
            vertex.color = Eigen::Vector4f::Ones();
        }
        if(mesh->mTextureCoords[0])
        {
            vertex.texCoord = Eigen::Vector2f(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
        }
        else
        {
            vertex.texCoord = Eigen::Vector2f(0.0f, 0.0f);
        }
        // 第二套 UV 只接受源模型真实提供的数据；缺失时保留 Vertex 的零值，禁止复制 UV0 伪造 2U。
        if(mesh->mTextureCoords[1])
        {
            vertex.texCoord1 = Eigen::Vector2f(mesh->mTextureCoords[1][i].x, mesh->mTextureCoords[1][i].y);
        }
        if(mesh->HasTangentsAndBitangents())
        {
            const Eigen::Vector3f tangent(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z);
            const Eigen::Vector3f bitangent(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z);
            const float sign = mesh->mNormals ? ComputeBitangentSign(vertex.normal, tangent, bitangent) : 1.0f;
            vertex.tangent = Eigen::Vector4f(tangent.x(), tangent.y(), tangent.z(), sign);
        }
        else
        {
            vertex.tangent = Eigen::Vector4f::Zero();
        }
        vertices.push_back(vertex);
    }
    // 处理索引：先用源模型原始绕序收集索引，保证 MikkTSpace 的 handedness（tangent.w）
    // 与烘焙端（Blender/glTF 等）一致；切线生成后再统一反转绕序，满足 Vulkan CW front-face
    // 裁剪。绕序反转只作用于索引层，不能提前到 MikkTSpace 之前，否则 tangent.w 翻号，
    // 法线贴图 G 通道（副切线方向）会整体反号。
    for(uint32_t i = 0; i < mesh->mNumFaces; i++)
    {
        aiFace face = mesh->mFaces[i];
        for(uint32_t j = 0; j < face.mNumIndices; j++)
        {
            indices.push_back(face.mIndices[j]);
        }
    }

    if (mesh->HasNormals() && mesh->mTextureCoords[0])
    {
        MikkTSpaceMeshGenerator::Generate(vertices, indices);
    }

    // 反转三角形绕序（CCW→CW）：Vulkan 使用 CW front face，配合投影 Y 翻转。
    // 只交换索引，不修改 tangent/normal 数据。
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        std::swap(indices[i], indices[i + 2]);
    }

    MeshSection section;
    section.vertices = std::move(vertices);
    section.indices = std::move(indices);
    section.sectionName = sectionName;
    section.materialSlotName = materialSlotName;
    return section;
}
