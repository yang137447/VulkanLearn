#include "render/rendergraph/sceneViewportOutputValidation.h"

#include <stdexcept>

#include "render/rendergraph/renderGraphCompiler.h"

namespace VL
{

bool ValidateSceneViewportOutputs(const CompiledRenderGraph& graph)
{
    const CompiledRenderGraphPass* finalPresentPass = nullptr;
    for (const CompiledRenderGraphPass& pass : graph.passes)
    {
        if (!pass.outputsSwapchain) continue;
        if (finalPresentPass == nullptr)
        {
            for (const CompiledRenderGraphPassOutput& output : pass.outputResources)
            {
                // graph 候选拥有全新图像，首个 writer 不能 LOAD 未初始化内容；
                // 后续 writer 可 LOAD 同帧内容，布局和依赖由兼容 pass 保持。
                if (output.resource == "swapChain" && output.loadOp == "load")
                {
                    throw std::runtime_error(
                        "First scene viewport output pass cannot LOAD an uninitialized image: " +
                        pass.name);
                }
            }
        }
        finalPresentPass = &pass;
    }
    if (finalPresentPass == nullptr) return false;
    for (const CompiledRenderGraphPassOutput& output : finalPresentPass->outputResources)
    {
        // UI 在最后一个 writer 结束后采样；仅校验最终输出，不能把中间 pass
        // 合法的 DontCare 一并禁止。编译器仍保留通用图语义，限制归显示目标边界。
        if (output.resource == "swapChain" && output.storeOp != "store")
        {
            throw std::runtime_error(
                "Final scene viewport output pass requires storeOp=store: " +
                finalPresentPass->name);
        }
    }
    return true;
}

} // namespace VL
