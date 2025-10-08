#pragma once
#include <vector>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/mesh.h>
#include "vertexDataStruct.h"

struct Mesh
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    Mesh() {}
    Mesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices)
        : vertices(vertices), indices(indices) {}
};

//定义一个单例类，用于加载模型
class ModelLoader
{
public:
    static ModelLoader& GetInstance()
    {
        static ModelLoader instance;
        return instance;
    }
    //加载模型
    void LoadModel(const std::string& filename);
    std::vector<Vertex>& GetVertexData() { return meshes[0].vertices; }
    std::vector<uint32_t>& GetIndexData()  { return meshes[0].indices; }
private:
    ModelLoader() {}
    ModelLoader(const ModelLoader&) = delete;
    ModelLoader& operator=(const ModelLoader&) = delete;
    ~ModelLoader();

    void ProcessNode(aiNode* node, const aiScene* scene);
    Mesh ProcessMesh(aiMesh* mesh, const aiScene* scene);


    std::vector<Mesh> meshes;
};