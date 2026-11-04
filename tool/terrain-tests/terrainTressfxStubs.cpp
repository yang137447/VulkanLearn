#include "renderableObject.h"

// terrain_tests 只覆盖 CPU 地形/页表逻辑，不实例化 RenderableObject。
// rendererResourceCache.cpp 仍会编译 TressFX 分支，因此这里提供最小链接桩，
// 避免把完整 Vulkan 渲染对象和 GPU hair pass 拖进地形模块测试。
bool RenderableObject::IsTressfx() const
{
    return false;
}

bool RenderableObject::UsesGpuGuideSimulation() const
{
    return false;
}

void RenderableObject::AdvanceTressfxSimulation(
    float, const Eigen::Vector3f&)
{
}
