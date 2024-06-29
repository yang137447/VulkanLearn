#include "renderCore.h"

void RenderCore::CreateVkInstace()
{
    vk::ApplicationInfo applicationInfo;
    applicationInfo.setApiVersion(VK_API_VERSION_1_3);
    vk::InstanceCreateInfo instanceCreateInfo;
    instanceCreateInfo.setPApplicationInfo(&applicationInfo);
    instace = vk::createInstance(instanceCreateInfo);
}