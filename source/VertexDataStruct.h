#pragma once
#include <Eigen/Dense>
#include <cstddef>
#include <vulkan/vulkan.hpp>

struct Vertex
{
    Eigen::Vector3f position;   // 顶点位置
    Eigen::Vector3f normal;     // 顶点法线
    Eigen::Vector4f color;      // 顶点颜色，w 可承载 alpha / AO 等顶点附加通道
    Eigen::Vector2f texCoord;   // 纹理坐标
    Eigen::Vector4f tangent;    // xyz: 切线, w: MikkTSpace handedness

    Vertex() {}
    Vertex(
        Eigen::Vector3f pos,
        Eigen::Vector3f nor,
        Eigen::Vector4f col,
        Eigen::Vector2f tex,
        Eigen::Vector4f tan = Eigen::Vector4f::Zero())
        : position(pos), normal(nor), color(col), texCoord(tex), tangent(tan) {}
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
            vk::Format::eR32G32B32A32Sfloat, // format
            offsetof(Vertex, position) // offset
        ),
        // 法线
        vk::VertexInputAttributeDescription(
            1, // location
            0, // binding
            vk::Format::eR32G32B32Sfloat, // format
            offsetof(Vertex, normal) // offset
        ),
        // 颜色
        vk::VertexInputAttributeDescription(
            2, // location
            0, // binding
            vk::Format::eR32G32B32Sfloat, // format
            offsetof(Vertex, color) // offset
        ),
        // 纹理坐标
        vk::VertexInputAttributeDescription(
            3, // location
            0, // binding
            vk::Format::eR32G32Sfloat, // format
            offsetof(Vertex, texCoord) // offset
        ),
        // 切线
        vk::VertexInputAttributeDescription(
            4, // location
            0, // binding
            vk::Format::eR32G32B32A32Sfloat, // format
            offsetof(Vertex, tangent) // offset
        )
    };
} // namespace VertexFormat
