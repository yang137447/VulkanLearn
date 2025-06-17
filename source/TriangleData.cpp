#include "TriangleData.h"
#include "VertexDataStruct.h"

void TriangleData::GenVertexData()
{
    vertices = {
        Vertex(Eigen::Vector3f(0.0f, -0.5f, 0.0f), Eigen::Vector3f(1.0f, 0.0f, 0.0f)), // 顶部红色
        Vertex(Eigen::Vector3f(0.5f, 0.5f, 0.0f), Eigen::Vector3f(0.0f, 1.0f, 0.0f)),  // 右下角绿色
        Vertex(Eigen::Vector3f(-0.5f, 0.5f, 0.0f), Eigen::Vector3f(0.0f, 0.0f, 1.0f))   // 左下角蓝色
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
