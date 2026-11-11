#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"

namespace VL
{

class IEditorHost;
class RenderThread;
class RendererBackendVulkan;
class SubsystemCollection;
enum class RenderGraphReleaseMode;
struct WorldGraphTransactionTestFaultInjection;

// EngineLoop 的窗口/渲染图重建状态视图。只保存指针，不复制 swapchain、
// render graph 或渲染线程状态。
struct EngineWindowLifecycleContext
{
    SubsystemCollection* subsystems = nullptr;
    RendererBackendVulkan* rendererBackend = nullptr;
    RenderThread* renderThread = nullptr;
    IEditorHost* editorHost = nullptr;
    nlohmann::json* runtimeRenderGraphJson = nullptr;
    const WorldGraphTransactionTestFaultInjection* testFaultInjection = nullptr;
    const bool* rendererBackendInitialized = nullptr;
    bool* shouldClose = nullptr;
    int* exitCode = nullptr;
};

// 结构变更前的安全点：先等渲染线程归还帧槽/空闲，并把 RT 致命错误升级成退出。
void WaitForEngineRenderThreadIdle(const EngineWindowLifecycleContext& context);
void WaitForEngineRenderFrameSlot(const EngineWindowLifecycleContext& context);
void PollEngineRenderThreadFatalError(
    const EngineWindowLifecycleContext& context);

// 交换链尺寸变化：Shutdown graph -> Release swapchain 资源 -> Recreate ->
// Load graph（保留 pass material/owner generation）。
RuntimeResult<void> RecreateEngineRendererForWindowResize(
    const EngineWindowLifecycleContext& context,
    uint32_t width,
    uint32_t height);

// Render graph 资源重载：候选 graph 载入后经 retire queue 原子换装。
RuntimeResult<void> ReloadEngineRenderGraphResources(
    const EngineWindowLifecycleContext& context,
    RenderGraphReleaseMode releaseMode);
RuntimeResult<void> ReloadEngineRenderGraphResources(
    const EngineWindowLifecycleContext& context);

} // namespace VL
