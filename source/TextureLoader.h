#pragma once
#include <vulkan/vulkan.hpp>

//定义一个单例类，用于加载纹理
class TextureLoader {
public:
    static TextureLoader& GetInstance() {
        static TextureLoader instance;
        return instance;
    }

    //设置设备、物理设备和命令池
    void Init(vk::Device* device, vk::PhysicalDevice* physicalDevice, vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties, vk::CommandPool* commandPool, vk::Queue* graphicsQueue);

    //加载纹理
    std::pair<vk::Image, vk::DeviceMemory> LoadTexture(const std::string& filename);
    uint32_t GetMipLevels() const { return mipLevels; }

    vk::ImageView GetImageView(vk::Image& textureImage, vk::Format format, uint32_t mipLevels, const std::string& name = "");

    vk::Sampler GetSampler(const std::string& name = "");

private:
    TextureLoader() {} //私有构造函数，防止外部实例化
    TextureLoader(const TextureLoader&) = delete; //禁止拷贝构造
    TextureLoader& operator=(const TextureLoader&) = delete; //禁止赋值操作
    ~TextureLoader() {} //私有析构函数，防止外部销毁
    //其他私有成员函数和数据

    uint32_t mipLevels = 0; //纹理的mipmap级别s

    vk::Device* device;
    vk::PhysicalDevice* physicalDevice;
    vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties;
    vk::CommandPool* commandPool;
    vk::Queue* graphicsQueue;
};