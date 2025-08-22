#include "TextureLoader.h"
#include <vulkan/vulkan.hpp>

vk::Image TextureLoader::loadTexture(const char* filename, vk::Device device, vk::PhysicalDevice physicalDevice, vk::CommandPool commandPool, vk::Queue graphicsQueue)
{
    int textureWidth, textureHeight, textureChannels;
    stbi_uc* pixels = stbi_load(filename, &textureWidth, &textureHeight, &textureChannels, STBI_rgb_alpha);
    vk::DeviceSize imageSize = textureWidth * textureHeight * 4; // Assuming 4 bytes per pixel (RGBA)
    if (!pixels) {
        throw std::runtime_error("Failed to load texture image!");
    }
}