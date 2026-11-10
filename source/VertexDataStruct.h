#pragma once
#include <Eigen/Dense>
#include <cstddef>
#include <vulkan/vulkan.hpp>

struct Vertex
{
    Eigen::Vector3f position;   // 顶点位置
    Eigen::Vector3f normal;     // 顶点法线
    Eigen::Vector4f color;      // 顶点颜色，w 可承载 alpha / AO 等顶点附加通道
    Eigen::Vector2f texCoord;   // 第一套纹理坐标
    // 第二套 UV 是可选通道；没有 TEXCOORD_1 的源网格保持零值。
    Eigen::Vector2f texCoord1 = Eigen::Vector2f::Zero();
    Eigen::Vector4f tangent;    // xyz: 切线, w: MikkTSpace handedness
    // SpeedTree Runtime SDK attributes remain normalized exactly as the
    // source packer declares them. Non-SpeedTree importers leave them zero.
    // Current limitation: these attributes remain in the shared vertex stream.
    // A SpeedTree-only auxiliary stream would remove their stride and buffer
    // cost from ordinary meshes.
    Eigen::Vector4f speedTreeWindBranch1 = Eigen::Vector4f::Zero();
    Eigen::Vector4f speedTreeWindBranch2 = Eigen::Vector4f::Zero();

    Vertex() {}
    Vertex(
        Eigen::Vector3f pos,
        Eigen::Vector3f nor,
        Eigen::Vector4f col,
        Eigen::Vector2f tex,
        Eigen::Vector4f tan = Eigen::Vector4f::Zero())
        : position(pos), normal(nor), color(col), texCoord(tex), tangent(tan) {}
};

// 静态植被/石头实例流。矩阵按列主序存放，四列对应 location 8..11 的四个 vec4；
// params 留给逐实例参数：x=windPhase, y=lodBias, z=variant, w=seed。
// 风系统后续可以直接消费 windPhase/seed，不需要再改顶点 ABI。
struct InstanceVertex
{
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    Eigen::Vector4f params = Eigen::Vector4f::Zero();
};

static_assert(sizeof(InstanceVertex) == 80, "InstanceVertex ABI must stay 80 bytes");
static_assert(offsetof(InstanceVertex, params) == 64, "InstanceVertex params offset must stay 64");

namespace VertexInfo{
    static std::vector<vk::VertexInputBindingDescription> vertexInputBindingDescriptions = {
        vk::VertexInputBindingDescription(
            0, sizeof(Vertex), vk::VertexInputRate::eVertex),
        vk::VertexInputBindingDescription(
            1, sizeof(InstanceVertex), vk::VertexInputRate::eInstance),
    };

    static std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions = {
        // 位置
        vk::VertexInputAttributeDescription(
            0, // location
            0, // binding
            vk::Format::eR32G32B32Sfloat, // format
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
            vk::Format::eR32G32B32A32Sfloat, // format
            offsetof(Vertex, color) // offset
        ),
        // 纹理坐标
        vk::VertexInputAttributeDescription(
            3, // location
            0, // binding
            vk::Format::eR32G32Sfloat, // format
            offsetof(Vertex, texCoord) // offset
        ),
        // 第二套纹理坐标；仅有 2U 的源网格会写入导入数据
        vk::VertexInputAttributeDescription(
            7, // location
            0, // binding
            vk::Format::eR32G32Sfloat, // format
            offsetof(Vertex, texCoord1) // offset
        ),
        // 切线
        vk::VertexInputAttributeDescription(
            4, // location
            0, // binding
            vk::Format::eR32G32B32A32Sfloat, // format
            offsetof(Vertex, tangent) // offset
        ),
        // SpeedTree branch 1: weight, packed direction, packed noise offset, ripple
        vk::VertexInputAttributeDescription(
            5, // location
            0, // binding
            vk::Format::eR32G32B32A32Sfloat, // format
            offsetof(Vertex, speedTreeWindBranch1) // offset
        ),
        // SpeedTree branch 2: weight, packed direction, packed noise offset, blend
        vk::VertexInputAttributeDescription(
            6, // location
            0, // binding
            vk::Format::eR32G32B32A32Sfloat, // format
            offsetof(Vertex, speedTreeWindBranch2) // offset
        ),
        // per-instance model：四列 mat4。
        vk::VertexInputAttributeDescription(
            8, 1, vk::Format::eR32G32B32A32Sfloat,
            offsetof(InstanceVertex, model) + 0),
        vk::VertexInputAttributeDescription(
            9, 1, vk::Format::eR32G32B32A32Sfloat,
            offsetof(InstanceVertex, model) + 16),
        vk::VertexInputAttributeDescription(
            10, 1, vk::Format::eR32G32B32A32Sfloat,
            offsetof(InstanceVertex, model) + 32),
        vk::VertexInputAttributeDescription(
            11, 1, vk::Format::eR32G32B32A32Sfloat,
            offsetof(InstanceVertex, model) + 48),
        // per-instance 参数，当前只透传给材质；风接入时直接使用。
        vk::VertexInputAttributeDescription(
            12, 1, vk::Format::eR32G32B32A32Sfloat,
            offsetof(InstanceVertex, params)),
    };
} // namespace VertexInfo
