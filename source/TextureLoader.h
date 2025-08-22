#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <vulkan/vulkan.hpp>

//定义一个单例类，用于加载纹理
class TextureLoader {
public:
    static TextureLoader& getInstance() {
        static TextureLoader instance;
        return instance;
    }

    //加载纹理
    vk::Image loadTexture(const char* filename, vk::Device device, vk::PhysicalDevice physicalDevice, vk::CommandPool commandPool, vk::Queue graphicsQueue);

private:
    TextureLoader() {} //私有构造函数，防止外部实例化
    TextureLoader(const TextureLoader&) = delete; //禁止拷贝构造
    TextureLoader& operator=(const TextureLoader&) = delete; //禁止赋值操作
    ~TextureLoader() {} //私有析构函数，防止外部销毁
    //其他私有成员函数和数据
}