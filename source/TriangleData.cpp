#include "TriangleData.h"
#include "VertexDataStruct.h"

void TriangleData::GenVertexData()
{
    vertices = {
        Vertex(Eigen::Vector3f(-1.0f, 0.5f, -1.0f), Eigen::Vector3f(1.0f, 1.0f, 1.0f), Eigen::Vector2f(0.0f, 1.0f)),
        Vertex(Eigen::Vector3f(1.0f, 0.5f, -1.0f), Eigen::Vector3f(1.0f, 1.0f, 1.0f), Eigen::Vector2f(1.0f, 1.0f)),
        Vertex(Eigen::Vector3f(1.0f, 0.5f, 1.0f), Eigen::Vector3f(1.0f, 1.0f, 1.0f), Eigen::Vector2f(1.0f, 0.0f)),
        Vertex(Eigen::Vector3f(-1.0f, 0.5f, 1.0f), Eigen::Vector3f(1.0f, 1.0f, 1.0f), Eigen::Vector2f(0.0f, 0.0f)),

        Vertex(Eigen::Vector3f(-1.0f, -0.5f, -1.0f), Eigen::Vector3f(1.0f, 1.0f, 1.0f), Eigen::Vector2f(0.0f, 1.0f)),
        Vertex(Eigen::Vector3f(1.0f, -0.5f, -1.0f), Eigen::Vector3f(1.0f, 1.0f, 1.0f), Eigen::Vector2f(1.0f, 1.0f)),
        Vertex(Eigen::Vector3f(1.0f, -0.5f, 1.0f), Eigen::Vector3f(1.0f, 1.0f, 1.0f), Eigen::Vector2f(1.0f, 0.0f)),
        Vertex(Eigen::Vector3f(-1.0f, -0.5f, 1.0f), Eigen::Vector3f(1.0f, 1.0f, 1.0f), Eigen::Vector2f(0.0f, 0.0f))
    };
    indices = { 
        0, 1, 2, 
        0, 2, 3,
        4, 5, 6,
        4, 6, 7,
    };
}

std::vector<Vertex>& TriangleData::GetVertexData()
{
    if (vertices.empty())
    {
        GenVertexData();
    }
    
    return vertices;
}

std::vector<uint32_t>& TriangleData::GetIndexData()
{
    if (indices.empty())
    {
        GenVertexData();
    }
    
    return indices;
}
