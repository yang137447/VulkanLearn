#pragma once
#include <vulkan/vulkan.hpp>
#include <string>

class Texture
{
public:
    Texture(const std::string& texturePath);
    ~Texture();
    inline vk::Image getImage() const { return image; }
    inline vk::DeviceMemory getImageMemory() const { return imageMemory; }
    inline vk::ImageView getImageView() const { return imageView; }
    inline vk::Sampler getSampler() const { return sampler; }
    inline uint32_t getMipLevels() const { return mipLevels; }
    inline vk::Format GetFormat() const { return format; }
private:
    Texture();
    
    vk::Image image;
    vk::DeviceMemory imageMemory;
    vk::ImageView imageView;
    vk::Sampler sampler;
    uint32_t mipLevels;
    vk::Format format;
};
