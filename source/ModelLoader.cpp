#include "modelLoader.h"
#include <cstdint>
#include <iostream>
#include "mikktspace.h"

namespace
{
    float ComputeBitangentSign(
        const Eigen::Vector3f& normal,
        const Eigen::Vector3f& tangent,
        const Eigen::Vector3f& bitangent)
    {
        return normal.cross(tangent).dot(bitangent) < 0.0f ? -1.0f : 1.0f;
    }

    struct MikkUserData
    {
        const std::vector<Vertex>* sourceVertices = nullptr;
        const std::vector<uint32_t>* sourceIndices = nullptr;
        std::vector<Vertex>* outputVertices = nullptr;
        std::vector<uint32_t>* outputIndices = nullptr;
    };

    const Vertex& GetFaceVertex(const MikkUserData& userData, int faceIndex, int vertexIndex)
    {
        const uint32_t sourceVertexIndex = (*userData.sourceIndices)[faceIndex * 3 + vertexIndex];
        return (*userData.sourceVertices)[sourceVertexIndex];
    }

    int MikkGetNumFaces(const SMikkTSpaceContext* context)
    {
        const auto* userData = static_cast<const MikkUserData*>(context->m_pUserData);
        return static_cast<int>(userData->sourceIndices->size() / 3);
    }

    int MikkGetNumVerticesOfFace(const SMikkTSpaceContext*, const int)
    {
        return 3;
    }

    void MikkGetPosition(const SMikkTSpaceContext* context, float positionOut[], const int faceIndex, const int vertexIndex)
    {
        const auto* userData = static_cast<const MikkUserData*>(context->m_pUserData);
        const Vertex& vertex = GetFaceVertex(*userData, faceIndex, vertexIndex);
        positionOut[0] = vertex.position.x();
        positionOut[1] = vertex.position.y();
        positionOut[2] = vertex.position.z();
    }

    void MikkGetNormal(const SMikkTSpaceContext* context, float normalOut[], const int faceIndex, const int vertexIndex)
    {
        const auto* userData = static_cast<const MikkUserData*>(context->m_pUserData);
        const Vertex& vertex = GetFaceVertex(*userData, faceIndex, vertexIndex);
        normalOut[0] = vertex.normal.x();
        normalOut[1] = vertex.normal.y();
        normalOut[2] = vertex.normal.z();
    }

    void MikkGetTexCoord(const SMikkTSpaceContext* context, float texCoordOut[], const int faceIndex, const int vertexIndex)
    {
        const auto* userData = static_cast<const MikkUserData*>(context->m_pUserData);
        const Vertex& vertex = GetFaceVertex(*userData, faceIndex, vertexIndex);
        texCoordOut[0] = vertex.texCoord.x();
        texCoordOut[1] = vertex.texCoord.y();
    }

    // MikkTSpace 对法线贴图只需要输出 tangent.xyz + handedness(sign)，
    // 副切线在 shader 中按 cross(N, T) * sign 重建即可，不再单独存顶点属性。
    void MikkSetTSpaceBasic(
        const SMikkTSpaceContext* context,
        const float tangent[],
        const float sign,
        const int faceIndex,
        const int vertexIndex)
    {
        auto* userData = static_cast<MikkUserData*>(context->m_pUserData);
        const Vertex& sourceVertex = GetFaceVertex(*userData, faceIndex, vertexIndex);

        Vertex vertex = sourceVertex;
        vertex.tangent = Eigen::Vector4f(tangent[0], tangent[1], tangent[2], sign);

        const int outputIndex = faceIndex * 3 + vertexIndex;
        (*userData->outputVertices)[outputIndex] = vertex;
        (*userData->outputIndices)[outputIndex] = static_cast<uint32_t>(outputIndex);
    }

    bool GenerateMikkTSpaceMesh(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
    {
        if (vertices.empty() || indices.empty())
        {
            return false;
        }

        MikkUserData userData;
        userData.sourceVertices = &vertices;
        userData.sourceIndices = &indices;

        std::vector<Vertex> generatedVertices(indices.size());
        std::vector<uint32_t> generatedIndices(indices.size());
        userData.outputVertices = &generatedVertices;
        userData.outputIndices = &generatedIndices;

        SMikkTSpaceInterface interface{};
        interface.m_getNumFaces = MikkGetNumFaces;
        interface.m_getNumVerticesOfFace = MikkGetNumVerticesOfFace;
        interface.m_getPosition = MikkGetPosition;
        interface.m_getNormal = MikkGetNormal;
        interface.m_getTexCoord = MikkGetTexCoord;
        interface.m_setTSpaceBasic = MikkSetTSpaceBasic;

        SMikkTSpaceContext context{};
        context.m_pInterface = &interface;
        context.m_pUserData = &userData;

        if (!genTangSpaceDefault(&context))
        {
            return false;
        }

        vertices = std::move(generatedVertices);
        indices = std::move(generatedIndices);
        return true;
    }
}

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
        else
        {
            vertex.normal = Eigen::Vector3f::Zero();
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
    // 处理索引
    for(uint32_t i = 0; i < mesh->mNumFaces; i++)
    {
        aiFace face = mesh->mFaces[i];
        for(uint32_t j = 0; j < face.mNumIndices; j++)
        {
            indices.push_back(face.mIndices[face.mNumIndices-1-j]);
        }
    }

    if (mesh->HasNormals() && mesh->mTextureCoords[0])
    {
        GenerateMikkTSpaceMesh(vertices, indices);
    }

    return Mesh(vertices, indices);
}
