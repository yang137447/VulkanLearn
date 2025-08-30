#pragma once
#include <vulkan/vulkan.hpp>

//定义一个单例类，用于加载纹理
class TextureLoader {
public:
    static TextureLoader& getInstance() {
        static TextureLoader instance;
        return instance;
    }

    //设置设备、物理设备和命令池
    void Init(vk::Device* device, vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties, vk::CommandPool* commandPool, vk::Queue* graphicsQueue);

    //加载纹理
    std::pair<vk::Image, vk::DeviceMemory> loadTexture(const std::string& filename);

private:
    TextureLoader() {} //私有构造函数，防止外部实例化
    TextureLoader(const TextureLoader&) = delete; //禁止拷贝构造
    TextureLoader& operator=(const TextureLoader&) = delete; //禁止赋值操作
    ~TextureLoader() {} //私有析构函数，防止外部销毁
    //其他私有成员函数和数据

    vk::Device* device;
    vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties;
    vk::CommandPool* commandPool;
    vk::Queue* graphicsQueue;
};