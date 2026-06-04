#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.hpp>

namespace VL
{

// Opaque Vulkan resource lifecycle handles. Concrete backend files may
// translate them to native Vulkan resources, while higher runtime code passes
// handles instead of owning raw API objects directly.
struct RHIBufferHandle
{
    uint64_t id = 0;

    bool IsValid() const { return id != 0; }
};

struct RHIImageHandle
{
    uint64_t id = 0;

    bool IsValid() const { return id != 0; }
};

struct RHIImageViewHandle
{
    uint64_t id = 0;

    bool IsValid() const { return id != 0; }
};

struct RHISamplerHandle
{
    uint64_t id = 0;

    bool IsValid() const { return id != 0; }
};

struct RHIDescriptorSetLayoutHandle
{
    uint64_t id = 0;

    bool IsValid() const { return id != 0; }
};

struct RHIDescriptorPoolHandle
{
    uint64_t id = 0;

    bool IsValid() const { return id != 0; }
};

struct RHIDescriptorSetHandle
{
    uint64_t id = 0;

    bool IsValid() const { return id != 0; }
};

struct RHIRenderPassHandle
{
    uint64_t id = 0;

    bool IsValid() const { return id != 0; }
};

struct RHIFramebufferHandle
{
    uint64_t id = 0;

    bool IsValid() const { return id != 0; }
};

struct RHIDescriptorWrite
{
    RHIDescriptorSetHandle destinationSet;
    uint32_t destinationBinding = 0;
    uint32_t destinationArrayElement = 0;
    uint32_t descriptorCount = 0;
    vk::DescriptorType descriptorType = vk::DescriptorType::eSampler;
    std::vector<vk::DescriptorImageInfo> imageInfos;
    std::vector<vk::DescriptorBufferInfo> bufferInfos;
    std::vector<vk::BufferView> texelBufferViews;
};

} // namespace VL
