#pragma once

#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"

class Controller;
class PipelineFactory;
class ShaderCompiler;

namespace VL
{

class DiagnosticsSubsystem;
class RendererBackendVulkan;
class RuntimeConfig;
class RenderThread;
class WorldManager;
class WorldTransitionCoordinator;
struct MaterialDefinitionReloadBatch;
struct WorldHandle;

struct WorldGraphTransactionTestFaultInjection
{
    size_t failGraphResourceCreationAt = 0;
    size_t failRenderPassCreationAt = 0;
    size_t failFramebufferCreationAt = 0;
    size_t failDescriptorCreationAt = 0;
    bool failPassMaterialContract = false;
    bool failAfterCandidateWorldBuilt = false;
    bool failViewTargetPrecheck = false;
    bool failAfterRuntimeBindingPrepared = false;
    bool failBeforeCommit = false;
    bool failResizeAfterSwapchainRecreate = false;
};

// Coordinates the isolated World/RenderGraph transaction. It prepares the
// candidate graph, World-local resource package, runtime binding, pipeline
// cache state, and formal shader artifacts; EngineLoop invokes it at a safe
// point and remains responsible for loop and shutdown policy. This type does
// not own the main loop or the shader compile worker.
class WorldGraphTransactionCoordinator
{
public:
    WorldGraphTransactionCoordinator(
        RendererBackendVulkan& rendererBackend,
        ShaderCompiler& shaderCompiler,
        PipelineFactory& pipelineFactory,
        WorldTransitionCoordinator& worldTransitionCoordinator,
        WorldManager& worldManager,
        Controller& controller,
        const RuntimeConfig& runtimeConfig,
        const DiagnosticsSubsystem& diagnostics,
        RenderThread* renderThread);

    RuntimeResult<WorldHandle> Execute(
        const std::string& scenePath,
        const MaterialDefinitionReloadBatch* materialDefinitionReload = nullptr,
        // 本次加载附加的场景对象（JSON 对象数组，可为空）。模型编辑器靠它把打开的模型
        // 放进中性预览环境，而不必生成临时场景文件。
        const nlohmann::json& extraSceneObjects = nlohmann::json::array(),
        const nlohmann::json& sceneDocument = nullptr);

    void SetFaultInjection(
        WorldGraphTransactionTestFaultInjection injection) noexcept;
    void SetRenderThread(RenderThread* renderThread) noexcept
    {
        this->renderThread = renderThread;
    }

private:
    RendererBackendVulkan& rendererBackend;
    ShaderCompiler& shaderCompiler;
    PipelineFactory& pipelineFactory;
    WorldTransitionCoordinator& worldTransitionCoordinator;
    WorldManager& worldManager;
    Controller& controller;
    const RuntimeConfig& runtimeConfig;
    const DiagnosticsSubsystem& diagnostics;
    RenderThread* renderThread = nullptr;
    WorldGraphTransactionTestFaultInjection faultInjection;
};

} // namespace VL
