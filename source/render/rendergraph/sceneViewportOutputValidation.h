#pragma once

namespace VL
{

struct CompiledRenderGraph;

// 消费已编译图，仅校验重定向到新建 Scene Viewport 图像的显示目标合同；
// 不修改通用图编译语义或创建 GPU 资源。返回是否存在 swapChain writer，
// 无输出时调用方不分配图像；违反首尾 writer 合同时抛出 std::runtime_error。
bool ValidateSceneViewportOutputs(const CompiledRenderGraph& graph);

} // namespace VL
