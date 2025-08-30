#include "TextureLoader.h"
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_enums.hpp>
#include "CommonFunction.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

void TextureLoader::Init(vk::Device* device, vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties, vk::CommandPool* commandPool, vk::Queue* graphicsQueue) {
    this->device = device;
    this->physicalDeviceMemoryProperties = physicalDeviceMemoryProperties;
    this->commandPool = commandPool;
    this->graphicsQueue = graphicsQueue;
}

std::pair<vk::Image, vk::DeviceMemory> TextureLoader::loadTexture(const std::string& filename)
{
    int textureWidth, textureHeight, textureChannels;
    // Load image using stb_image
    stbi_uc* pixels = stbi_load(filename.c_str(), &textureWidth, &textureHeight, &textureChannels, STBI_rgb_alpha);
    vk::DeviceSize imageSize = textureWidth * textureHeight * 4; // Assuming 4 bytes per pixel (RGBA)
    if (!pixels) {
        throw std::runtime_error("Failed to load texture image!");
    }

    // Create staging buffer
    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingBufferMemory;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eTransferSrc;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    std::tie(stagingBuffer, stagingBufferMemory) = CommonFunction::CreateBuffer(*device, imageSize,
        usage,
        *physicalDeviceMemoryProperties,
        memoryPropertyFlags);

    // Copy image data to staging buffer
    void *data = device->mapMemory(stagingBufferMemory, 0, imageSize);
    memcpy(data, pixels, static_cast<size_t>(imageSize));
    device->unmapMemory(stagingBufferMemory);
    stbi_image_free(pixels);

    // Create texture image
    vk::Image Image;
    vk::DeviceMemory ImageMemory;
    vk::Format format = vk::Format::eR8G8B8A8Srgb;
    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
    vk::ImageUsageFlags usageFlags = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    memoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    std::tie(Image, ImageMemory) = CommonFunction::CreateImage(
        *device, 
        textureWidth, textureHeight, 
        format, tiling, 
        usageFlags, 
        *physicalDeviceMemoryProperties, 
        memoryPropertyFlags);
    
    // copy staging buffer to texture image
    CommonFunction::TransitionImageLayout(Image, *device, *commandPool, *graphicsQueue, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
    CommonFunction::CopyBufferToImage(*device, *graphicsQueue, *commandPool, stagingBuffer, Image, textureWidth, textureHeight);
    CommonFunction::TransitionImageLayout(Image, *device, *commandPool, *graphicsQueue, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);

    // Clean up staging buffer
    device->destroyBuffer(stagingBuffer);
    device->freeMemory(stagingBufferMemory);

    return { Image, ImageMemory };
}