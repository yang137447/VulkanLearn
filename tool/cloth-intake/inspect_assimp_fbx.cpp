#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::string Quote(std::string value)
{
    std::string result = "\"";
    for (const char character : value)
    {
        if (character == '"')
        {
            result += "\"\"";
        }
        else if (character == '\r')
        {
            result += "\\r";
        }
        else if (character == '\n')
        {
            result += "\\n";
        }
        else
        {
            result += character;
        }
    }
    result += '"';
    return result;
}

std::string MaterialName(const aiScene& scene, unsigned int index)
{
    if (index >= scene.mNumMaterials || scene.mMaterials[index] == nullptr)
    {
        return "<invalid>";
    }
    aiString name;
    if (scene.mMaterials[index]->Get(AI_MATKEY_NAME, name) != AI_SUCCESS)
    {
        return "<unnamed>";
    }
    return name.C_Str();
}

std::string TextureTypeName(aiTextureType type)
{
    switch (type)
    {
        case aiTextureType_DIFFUSE: return "diffuse";
        case aiTextureType_SPECULAR: return "specular";
        case aiTextureType_AMBIENT: return "ambient";
        case aiTextureType_EMISSIVE: return "emissive";
        case aiTextureType_HEIGHT: return "height";
        case aiTextureType_NORMALS: return "normals";
        case aiTextureType_SHININESS: return "shininess";
        case aiTextureType_OPACITY: return "opacity";
        case aiTextureType_DISPLACEMENT: return "displacement";
        case aiTextureType_LIGHTMAP: return "lightmap";
        case aiTextureType_REFLECTION: return "reflection";
        case aiTextureType_BASE_COLOR: return "baseColor";
        case aiTextureType_NORMAL_CAMERA: return "normalCamera";
        case aiTextureType_EMISSION_COLOR: return "emissionColor";
        case aiTextureType_METALNESS: return "metalness";
        case aiTextureType_DIFFUSE_ROUGHNESS: return "diffuseRoughness";
        case aiTextureType_AMBIENT_OCCLUSION: return "ambientOcclusion";
        case aiTextureType_SHEEN: return "sheen";
        case aiTextureType_CLEARCOAT: return "clearcoat";
        case aiTextureType_TRANSMISSION: return "transmission";
        case aiTextureType_UNKNOWN: return "unknown";
        default: return "type" + std::to_string(static_cast<int>(type));
    }
}

std::string Join(const std::vector<std::string>& values, char separator)
{
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
        {
            result += separator;
        }
        result += values[index];
    }
    return result;
}

void WriteNode(
    const aiScene& scene,
    const aiNode& node,
    const std::string& path,
    unsigned int depth,
    std::ofstream& nodes)
{
    std::vector<std::string> meshNames;
    std::set<unsigned int> materialIndices;
    for (unsigned int index = 0; index < node.mNumMeshes; ++index)
    {
        const unsigned int meshIndex = node.mMeshes[index];
        if (meshIndex >= scene.mNumMeshes || scene.mMeshes[meshIndex] == nullptr)
        {
            continue;
        }
        const aiMesh& mesh = *scene.mMeshes[meshIndex];
        meshNames.emplace_back(mesh.mName.C_Str());
        materialIndices.insert(mesh.mMaterialIndex);
    }

    std::vector<std::string> materialNames;
    for (const unsigned int materialIndex : materialIndices)
    {
        materialNames.push_back(MaterialName(scene, materialIndex));
    }

    nodes << depth << ',' << Quote(path) << ',' << Quote(node.mName.C_Str()) << ','
          << node.mNumMeshes << ',' << node.mNumChildren << ','
          << Quote(Join(meshNames, '|')) << ','
          << Quote(Join(materialNames, '|')) << '\n';

    const std::string childPrefix = path.empty() ? "" : path + "/";
    for (unsigned int index = 0; index < node.mNumChildren; ++index)
    {
        const aiNode* child = node.mChildren[index];
        if (child == nullptr)
        {
            continue;
        }
        WriteNode(
            scene,
            *child,
            childPrefix + child->mName.C_Str(),
            depth + 1,
            nodes);
    }
}

struct Bounds
{
    bool valid = false;
    float minX = 0.0f;
    float minY = 0.0f;
    float minZ = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    float maxZ = 0.0f;

    void Add(const aiVector3D& point)
    {
        if (!valid)
        {
            minX = maxX = point.x;
            minY = maxY = point.y;
            minZ = maxZ = point.z;
            valid = true;
            return;
        }
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        minZ = std::min(minZ, point.z);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
        maxZ = std::max(maxZ, point.z);
    }

    void Merge(const Bounds& other)
    {
        if (!other.valid)
        {
            return;
        }
        Add(aiVector3D(other.minX, other.minY, other.minZ));
        Add(aiVector3D(other.maxX, other.maxY, other.maxZ));
    }
};

void AccumulateNodeBounds(
    const aiScene& scene,
    const aiNode& node,
    const aiMatrix4x4& parent,
    std::vector<Bounds>& localBounds,
    std::vector<Bounds>& worldMeshBounds,
    Bounds& worldBounds)
{
    const aiMatrix4x4 world = parent * node.mTransformation;
    for (unsigned int index = 0; index < node.mNumMeshes; ++index)
    {
        const unsigned int meshIndex = node.mMeshes[index];
        if (meshIndex >= scene.mNumMeshes || scene.mMeshes[meshIndex] == nullptr)
        {
            continue;
        }
        const aiMesh& mesh = *scene.mMeshes[meshIndex];
        Bounds transformed;
        for (unsigned int vertexIndex = 0; vertexIndex < mesh.mNumVertices; ++vertexIndex)
        {
            localBounds[meshIndex].Add(mesh.mVertices[vertexIndex]);
            const aiVector3D point = world * mesh.mVertices[vertexIndex];
            transformed.Add(point);
        }
        worldBounds.Merge(transformed);
        worldMeshBounds[meshIndex].Merge(transformed);
    }
    for (unsigned int index = 0; index < node.mNumChildren; ++index)
    {
        if (node.mChildren[index] != nullptr)
        {
            AccumulateNodeBounds(
                scene,
                *node.mChildren[index],
                world,
                localBounds,
                worldMeshBounds,
                worldBounds);
        }
    }
}

void WriteInstances(
    const aiScene& scene,
    const aiNode& node,
    const aiMatrix4x4& parent,
    const std::string& path,
    unsigned int& instanceIndex,
    std::ofstream& instances)
{
    const aiMatrix4x4 world = parent * node.mTransformation;
    const std::string nodePath = path.empty()
        ? node.mName.C_Str()
        : path + "/" + node.mName.C_Str();
    for (unsigned int index = 0; index < node.mNumMeshes; ++index)
    {
        const unsigned int meshIndex = node.mMeshes[index];
        if (meshIndex >= scene.mNumMeshes || scene.mMeshes[meshIndex] == nullptr)
        {
            continue;
        }
        const aiMesh& mesh = *scene.mMeshes[meshIndex];
        instances << instanceIndex++ << ','
                  << Quote(node.mName.C_Str()) << ','
                  << Quote(nodePath) << ','
                  << meshIndex << ','
                  << Quote(mesh.mName.C_Str()) << ','
                  << mesh.mMaterialIndex << ','
                  << Quote(MaterialName(scene, mesh.mMaterialIndex));
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                instances << ',' << world[row][column];
            }
        }
        instances << '\n';
    }
    for (unsigned int index = 0; index < node.mNumChildren; ++index)
    {
        if (node.mChildren[index] != nullptr)
        {
            WriteInstances(
                scene,
                *node.mChildren[index],
                world,
                nodePath,
                instanceIndex,
                instances);
        }
    }
}
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: inspect_assimp_fbx <input.fbx> <output-dir>\n";
        return 2;
    }

    const std::filesystem::path input = std::filesystem::u8path(argv[1]);
    const std::filesystem::path output = std::filesystem::u8path(argv[2]);
    std::filesystem::create_directories(output);

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(
        input.string(),
        aiProcess_Triangulate | aiProcess_GlobalScale);
    if (scene == nullptr || scene->mRootNode == nullptr)
    {
        std::cerr << "assimp import failed: " << importer.GetErrorString() << '\n';
        return 3;
    }

    std::ofstream nodes(output / "nodes.csv");
    nodes << "depth,path,name,node_meshes,children,mesh_names,material_names\n";
    WriteNode(*scene, *scene->mRootNode, scene->mRootNode->mName.C_Str(), 0, nodes);

    std::ofstream materials(output / "materials.csv");
    materials << "material_index,material_name,texture_count,diffuse_r,diffuse_g,diffuse_b,diffuse_a,opacity,shininess\n";
    std::ofstream textures(output / "textures.csv");
    textures << "material_index,material_name,type,path\n";

    for (unsigned int materialIndex = 0;
         materialIndex < scene->mNumMaterials;
         ++materialIndex)
    {
        const aiMaterial* material = scene->mMaterials[materialIndex];
        const std::string name = MaterialName(*scene, materialIndex);
        unsigned int textureCount = 0;
        aiColor4D diffuse(1.0f, 1.0f, 1.0f, 1.0f);
        material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);
        float opacity = 1.0f;
        material->Get(AI_MATKEY_OPACITY, opacity);
        float shininess = 0.0f;
        material->Get(AI_MATKEY_SHININESS, shininess);
        for (int typeIndex = 0; typeIndex <= static_cast<int>(aiTextureType_UNKNOWN);
             ++typeIndex)
        {
            const auto type = static_cast<aiTextureType>(typeIndex);
            const unsigned int count = material->GetTextureCount(type);
            for (unsigned int textureIndex = 0; textureIndex < count; ++textureIndex)
            {
                aiString path;
                if (material->GetTexture(type, textureIndex, &path) == AI_SUCCESS)
                {
                    textures << materialIndex << ',' << Quote(name) << ','
                             << Quote(TextureTypeName(type)) << ','
                             << Quote(path.C_Str()) << '\n';
                    ++textureCount;
                }
            }
        }
        materials << materialIndex << ',' << Quote(name) << ',' << textureCount << ','
                  << diffuse.r << ',' << diffuse.g << ',' << diffuse.b << ','
                  << diffuse.a << ',' << opacity << ',' << shininess << '\n';
    }

    std::vector<Bounds> localBounds(scene->mNumMeshes);
    std::vector<Bounds> worldMeshBounds(scene->mNumMeshes);
    Bounds worldBounds;
    AccumulateNodeBounds(
        *scene,
        *scene->mRootNode,
        aiMatrix4x4(),
        localBounds,
        worldMeshBounds,
        worldBounds);
    std::ofstream meshBounds(output / "mesh_bounds.csv");
    meshBounds << "mesh_index,mesh_name,material_index,vertices,"
                  "min_x,min_y,min_z,max_x,max_y,max_z,"
                  "world_min_x,world_min_y,world_min_z,world_max_x,world_max_y,world_max_z,"
                  "has_normals,has_tangents,has_uv0,has_uv1\n";
    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
    {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        const Bounds& bounds = localBounds[meshIndex];
        const Bounds& world = worldMeshBounds[meshIndex];
        if (mesh == nullptr || !bounds.valid || !world.valid)
        {
            continue;
        }
        meshBounds << meshIndex << ',' << Quote(mesh->mName.C_Str()) << ','
                   << mesh->mMaterialIndex << ',' << mesh->mNumVertices << ','
                   << bounds.minX << ',' << bounds.minY << ',' << bounds.minZ << ','
                   << bounds.maxX << ',' << bounds.maxY << ',' << bounds.maxZ << ','
                   << world.minX << ',' << world.minY << ',' << world.minZ << ','
                   << world.maxX << ',' << world.maxY << ',' << world.maxZ << ','
                   << (mesh->HasNormals() ? 1 : 0) << ','
                   << (mesh->HasTangentsAndBitangents() ? 1 : 0) << ','
                   << (mesh->HasTextureCoords(0) ? 1 : 0) << ','
                   << (mesh->HasTextureCoords(1) ? 1 : 0) << '\n';
    }

    std::ofstream summary(output / "summary.txt");
    summary << "meshes=" << scene->mNumMeshes << '\n';
    summary << "materials=" << scene->mNumMaterials << '\n';
    summary << "textures=" << scene->mNumTextures << '\n';
    summary << "top_level_children=" << scene->mRootNode->mNumChildren << '\n';
    summary << "animations=" << scene->mNumAnimations << '\n';
    if (worldBounds.valid)
    {
        summary << "world_bounds=" << worldBounds.minX << ',' << worldBounds.minY << ','
                << worldBounds.minZ << '|' << worldBounds.maxX << ','
                << worldBounds.maxY << ',' << worldBounds.maxZ << '\n';
    }

    std::ofstream instances(output / "instances.csv");
    instances << "instance_index,node_name,node_path,mesh_index,mesh_name,"
                 "material_index,material_name,"
                 "m00,m01,m02,m03,m10,m11,m12,m13,m20,m21,m22,m23,m30,m31,m32,m33\n";
    unsigned int instanceIndex = 0;
    WriteInstances(
        *scene,
        *scene->mRootNode,
        aiMatrix4x4(),
        "",
        instanceIndex,
        instances);

    std::cout << "imported " << scene->mNumMeshes << " meshes, "
              << scene->mNumMaterials << " materials, "
              << scene->mNumTextures << " embedded textures, "
              << scene->mRootNode->mNumChildren << " top-level nodes\n";
    return 0;
}
