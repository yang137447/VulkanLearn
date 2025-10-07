#pragma once
#include <Eigen/Dense>
#include <cstddef>
#include <vulkan/vulkan.hpp>

struct Vertex
{
    Eigen::Vector3f position; // 顶点位置
    Eigen::Vector3f color;    // 顶点颜色
    Eigen::Vector2f texCoord; // 纹理坐标

    Vertex() {}
    Vertex(Eigen::Vector3f pos, Eigen::Vector3f col, Eigen::Vector2f tex)
        : position(pos), color(col), texCoord(tex) {}
};

namespace VertexInfo{
    static vk::VertexInputBindingDescription vertexInputBindingDescription = {
        0, // binding
        sizeof(Vertex), // stride
        vk::VertexInputRate::eVertex, // inputRate
    };

    static std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions = {
        // 位置
        vk::VertexInputAttributeDescription(
            0, // location
            0, // binding
            vk::Format::eR32G32B32Sfloat, // format
            offsetof(Vertex, position) // offset
        ),
        // 颜色
        vk::VertexInputAttributeDescription(
            1, // location
            0, // binding
            vk::Format::eR32G32B32Sfloat, // format
            offsetof(Vertex, color) // offset
        ),
        // 纹理坐标
        vk::VertexInputAttributeDescription(
            2, // location
            0, // binding
            vk::Format::eR32G32Sfloat, // format
            offsetof(Vertex, texCoord) // offset
        )
    };
} // namespace VertexFormat