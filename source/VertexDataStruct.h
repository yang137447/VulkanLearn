#pragma once
#include <Eigen/Dense>

struct Vertex
{
    Eigen::Vector3f position; // 顶点位置
    Eigen::Vector3f color;    // 顶点颜色
    Eigen::Vector2f texCoord; // 纹理坐标

    Vertex() {}
    Vertex(Eigen::Vector3f pos, Eigen::Vector3f col, Eigen::Vector2f tex)
        : position(pos), color(col), texCoord(tex) {}
};