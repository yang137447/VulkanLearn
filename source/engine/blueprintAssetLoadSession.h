#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

class PipelineFactory;
class RenderGraph;
namespace VL
{
class RendererBackendVulkan;
class World;

// Engine GT 稳定边界推进；CPU worker 不持有 World、Vulkan 或托管上下文。
class BlueprintAssetLoadSession
{
public:
    BlueprintAssetLoadSession(RendererBackendVulkan& backend, PipelineFactory& factory, RenderGraph& graph);
    ~BlueprintAssetLoadSession();
    void Begin(const World& world, uint64_t requestId, const std::string& path,
        const nlohmann::json& classCatalog);
    bool Step(World& world);
    void Cancel();
    // Engine shutdown 在配置/全局路径服务析构前收口；普通取消和 World 切换不等待。
    static void WaitForWorkers();
    bool IsDone() const;
    const std::string& Error() const;
    const std::string& Path() const;
    uint64_t RequestId() const;
    const nlohmann::json& Template() const;
    // 调用方已收口该路径全部 lease 与 Spawn；旧 GPU package 按最后使用 epoch 退休。
    void Release(World& world);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
