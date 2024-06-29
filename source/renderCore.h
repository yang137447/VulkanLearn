#include "vulkan/vulkan.hpp"

class RenderCore
{
public:
    void Init();
    void Quit();

    void CreateVkInstace();
private:
    vk::Instance instace;
};