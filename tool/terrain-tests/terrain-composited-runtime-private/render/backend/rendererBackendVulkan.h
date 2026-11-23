#pragma once

#include <vulkan/vulkan.hpp>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace VL
{
class RendererBackendVulkan
{
public:
    struct Image
    {
        vk::ImageCreateInfo description;
        std::map<std::array<uint32_t, 3>, std::array<float, 4>> entries;
        uint64_t copies = 0;
    };

    vk::MemoryRequirements QueryBufferMemoryRequirements(uint64_t bytes, vk::BufferUsageFlags)
    {
        return {bytes, 16, 1};
    }

    vk::MemoryRequirements QueryImageMemoryRequirements(const vk::ImageCreateInfo& info)
    {
        const uint64_t texelBytes = info.format == vk::Format::eR32G32B32A32Sfloat ? 16 : 4;
        return {uint64_t(info.extent.width) * info.extent.height * info.arrayLayers * texelBytes, 16, 1};
    }

    std::pair<vk::Buffer, vk::DeviceMemory> CreateBuffer(uint64_t bytes,
        vk::BufferUsageFlags, vk::MemoryPropertyFlags, const std::string&)
    {
        const vk::Buffer buffer(reinterpret_cast<VkBuffer>(++sequence));
        const vk::DeviceMemory memory(reinterpret_cast<VkDeviceMemory>(++sequence));
        memoryBytes[memory].resize(bytes);
        buffers[buffer] = memory;
        ++bufferAllocations;
        return {buffer, memory};
    }

    void* MapMemory(vk::DeviceMemory memory, uint64_t) { return memoryBytes.at(memory).data(); }
    void UnmapMemory(vk::DeviceMemory) {}
    void DestroyBuffer(vk::Buffer buffer, vk::DeviceMemory memory)
    {
        buffers.erase(buffer);
        memoryBytes.erase(memory);
    }

    std::pair<vk::Image, vk::DeviceMemory> CreateImage(const vk::ImageCreateInfo& info,
        vk::MemoryPropertyFlags, const std::string&)
    {
        if (failImageAllocation)
            throw std::runtime_error("Injected image allocation failure");
        const vk::Image image(reinterpret_cast<VkImage>(++sequence));
        const vk::DeviceMemory memory(reinterpret_cast<VkDeviceMemory>(++sequence));
        images.emplace(image, Image{info, {}, 0});
        ++imageAllocations;
        return {image, memory};
    }

    template<typename... Arguments> void TransitionImageLayout(Arguments&&...) {}
    template<typename... Arguments> vk::ImageView CreateImageView(Arguments&&...)
    {
        return vk::ImageView(reinterpret_cast<VkImageView>(++sequence));
    }
    template<typename... Arguments> vk::Sampler CreateSampler(Arguments&&...)
    {
        return vk::Sampler(reinterpret_cast<VkSampler>(++sequence));
    }
    void DestroyImageResource(vk::Image image, vk::DeviceMemory, vk::ImageView, vk::Sampler)
    {
        images.erase(image);
    }

    void RecordCopyBufferToImageRegions(vk::CommandBuffer, vk::Buffer buffer,
        vk::Image image, const std::vector<vk::BufferImageCopy>& regions)
    {
        auto& target = images.at(image);
        for (const auto& region : regions)
        {
            ++target.copies;
            if (target.description.format == vk::Format::eR32G32B32A32Sfloat)
            {
                std::array<float, 4> entry;
                std::memcpy(entry.data(), memoryBytes.at(buffers.at(buffer)).data() +
                    region.bufferOffset, sizeof(entry));
                target.entries[{region.imageSubresource.baseArrayLayer,
                    uint32_t(region.imageOffset.x), uint32_t(region.imageOffset.y)}] = entry;
            }
        }
    }

    void RecordClearColorImage(vk::CommandBuffer, vk::Image image,
        vk::ImageLayout, vk::ClearColorValue, uint32_t, uint32_t)
    {
        images.at(image).entries.clear();
    }

    std::map<vk::Image, Image> images;
    uint64_t imageAllocations = 0;
    uint64_t bufferAllocations = 0;
    bool failImageAllocation = false;

private:
    uintptr_t sequence = 0;
    std::map<vk::Buffer, vk::DeviceMemory> buffers;
    std::map<vk::DeviceMemory, std::vector<uint8_t>> memoryBytes;
};
}
