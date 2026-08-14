#pragma once

// File responsibility: Defines the stable, reflection-derived Shader ABI used
// for cache metadata and hot-reload compatibility checks. It contains no
// Vulkan objects and can safely cross the compile-worker boundary.

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

namespace VL
{

struct ShaderAbiMember
{
    std::string name;
    uint32_t offset = 0;
    uint32_t size = 0;
    std::string type;

    bool operator==(const ShaderAbiMember& other) const;
};

struct ShaderAbiDescriptor
{
    uint32_t set = 0;
    uint32_t binding = 0;
    vk::DescriptorType type = vk::DescriptorType::eSampler;
    uint32_t count = 1;
    vk::ShaderStageFlags stageFlags;
    std::string name;
    uint32_t blockSize = 0;
    std::vector<ShaderAbiMember> members;

    bool operator==(const ShaderAbiDescriptor& other) const;
};

struct ShaderAbiPushConstant
{
    uint32_t offset = 0;
    uint32_t size = 0;
    vk::ShaderStageFlags stageFlags;
    std::vector<ShaderAbiMember> members;

    bool operator==(const ShaderAbiPushConstant& other) const;
};

struct ShaderAbiInterfaceVariable
{
    uint32_t location = 0;
    uint32_t component = 0;
    vk::Format format = vk::Format::eUndefined;
    std::string type;
    std::string name;

    bool operator==(const ShaderAbiInterfaceVariable& other) const;
};

struct ShaderAbiSpecializationConstant
{
    uint32_t constantId = 0;
    std::string type;
    std::string name;

    bool operator==(const ShaderAbiSpecializationConstant& other) const;
};

struct ShaderAbiWorkgroupSize
{
    uint32_t x = 1;
    uint32_t y = 1;
    uint32_t z = 1;
    bool present = false;

    bool operator==(const ShaderAbiWorkgroupSize& other) const
    {
        return present == other.present &&
            (!present ||
             (x == other.x && y == other.y && z == other.z));
    }
};

struct ShaderAbiSignature
{
    std::vector<ShaderAbiDescriptor> descriptors;
    std::vector<ShaderAbiPushConstant> pushConstants;
    std::vector<ShaderAbiInterfaceVariable> vertexInputs;
    std::vector<ShaderAbiInterfaceVariable> fragmentOutputs;
    std::vector<ShaderAbiSpecializationConstant> specializationConstants;
    ShaderAbiWorkgroupSize workgroupSize;

    void Normalize();
    std::string GetFingerprint() const;
    std::vector<std::string> DescribeDifferences(
        const ShaderAbiSignature& candidate) const;

    bool operator==(const ShaderAbiSignature& other) const;
    bool operator!=(const ShaderAbiSignature& other) const { return !(*this == other); }
};

} // namespace VL
