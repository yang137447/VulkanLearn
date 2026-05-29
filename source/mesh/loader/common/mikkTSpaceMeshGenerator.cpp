#include "mikkTSpaceMeshGenerator.h"

#include "mikktspace.h"

namespace
{
    struct MikkUserData
    {
        const std::vector<Vertex>* sourceVertices = nullptr;
        const std::vector<uint32_t>* sourceIndices = nullptr;
        std::vector<Vertex>* outputVertices = nullptr;
        std::vector<uint32_t>* outputIndices = nullptr;
        std::vector<uint32_t>* outputSourceVertexIndices = nullptr;
    };

    uint32_t GetSourceVertexIndex(const MikkUserData& userData, int faceIndex, int vertexIndex)
    {
        return (*userData.sourceIndices)[faceIndex * 3 + vertexIndex];
    }

    const Vertex& GetFaceVertex(const MikkUserData& userData, int faceIndex, int vertexIndex)
    {
        return (*userData.sourceVertices)[GetSourceVertexIndex(userData, faceIndex, vertexIndex)];
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

    void MikkSetTSpaceBasic(
        const SMikkTSpaceContext* context,
        const float tangent[],
        const float sign,
        const int faceIndex,
        const int vertexIndex)
    {
        auto* userData = static_cast<MikkUserData*>(context->m_pUserData);
        const uint32_t sourceVertexIndex = GetSourceVertexIndex(*userData, faceIndex, vertexIndex);
        const Vertex& sourceVertex = (*userData->sourceVertices)[sourceVertexIndex];

        Vertex vertex = sourceVertex;
        vertex.tangent = Eigen::Vector4f(tangent[0], tangent[1], tangent[2], sign);

        const int outputIndex = faceIndex * 3 + vertexIndex;
        (*userData->outputVertices)[outputIndex] = vertex;
        (*userData->outputIndices)[outputIndex] = static_cast<uint32_t>(outputIndex);
        if (userData->outputSourceVertexIndices != nullptr)
        {
            (*userData->outputSourceVertexIndices)[outputIndex] = sourceVertexIndex;
        }
    }
}

bool MikkTSpaceMeshGenerator::Generate(
    std::vector<Vertex>& vertices,
    std::vector<uint32_t>& indices,
    std::vector<uint32_t>* outputSourceVertexIndices)
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
    std::vector<uint32_t> generatedSourceVertexIndices;
    if (outputSourceVertexIndices != nullptr)
    {
        generatedSourceVertexIndices.resize(indices.size());
        userData.outputSourceVertexIndices = &generatedSourceVertexIndices;
    }
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
    if (outputSourceVertexIndices != nullptr)
    {
        *outputSourceVertexIndices = std::move(generatedSourceVertexIndices);
    }
    return true;
}
