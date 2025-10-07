#include "texture.h"
#include "textureLoader.h"
#include "commonFunction.h"
#include "vulkanManager.h"

Texture::Texture(const std::string& texturePath)
{
    TextureLoader& loader = TextureLoader::GetInstance();
    std::tie(image, imageMemory) = loader.LoadTexture(CommonFunction::Path(texturePath));
    mipLevels = loader.GetMipLevels();
    imageView = loader.GetImageView(image, vk::Format::eR8G8B8A8Srgb, mipLevels);
    sampler = loader.GetSampler();
}
Texture::~Texture()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    device.destroyImageView(imageView);
    device.destroyImage(image);
    device.freeMemory(imageMemory);
    device.destroySampler(sampler);
}