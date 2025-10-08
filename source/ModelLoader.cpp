#include "modelLoader.h"
#include <cstdint>
#include <iostream>

ModelLoader::~ModelLoader()
{
    meshes.clear();
    std::cout << "ModelLoader destroyed" << std::endl;
}

void ModelLoader::LoadModel(const std::string& fileName)
{
    meshes.clear();

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(fileName, 
        aiProcess_Triangulate);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
    {
        std::cout << "ERROR::ASSIMP::" << importer.GetErrorString() << std::endl;
        return;
    }
    ProcessNode(scene->mRootNode, scene);
    std::cout << "Model loaded successfully:" << fileName << std::endl;
}

void ModelLoader::ProcessNode(aiNode* node, const aiScene* scene)
{
    // process all the node's meshes (if any)
    for (unsigned int i = 0; i < node->mNumMeshes; i++)
    {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        meshes.push_back(ProcessMesh(mesh, scene));
    }
    // then do the same for each of its children
    for (unsigned int i = 0; i < node->mNumChildren; i++)
    {
        ProcessNode(node->mChildren[i], scene);
    }
}

Mesh ModelLoader::ProcessMesh(aiMesh* mesh, const aiScene* scene)
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // 处理顶点位置、顶点法线、颜色、纹理坐标
    for(uint32_t i = 0; i < mesh->mNumVertices; i++)
    {
        Vertex vertex;
        vertex.position = Eigen::Vector3f(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z);
        if(mesh->mNormals)
        {
            vertex.normal = Eigen::Vector3f(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z);
        }
        if(mesh->mColors[0])
        {
            vertex.color = Eigen::Vector3f(mesh->mColors[0][i].r, mesh->mColors[0][i].g, mesh->mColors[0][i].b);
        }
        else
        {
            vertex.color = Eigen::Vector3f(1.0f, 1.0f, 1.0f);
        }
        if(mesh->mTextureCoords[0])
        {
            vertex.texCoord = Eigen::Vector2f(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
        }
        else
        {
            vertex.texCoord = Eigen::Vector2f(0.0f, 0.0f);
        }
        vertices.push_back(vertex);
    }
    // 处理索引
    for(uint32_t i = 0; i < mesh->mNumFaces; i++)
    {
        aiFace face = mesh->mFaces[i];
        for(uint32_t j = 0; j < face.mNumIndices; j++)
        {
            indices.push_back(face.mIndices[face.mNumIndices-1-j]);
        }
    }
    return Mesh(vertices, indices);
}
