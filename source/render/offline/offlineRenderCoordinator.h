#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "render/offline/offlineSceneAdapter.h"

namespace VL
{
struct OfflinePublishedResult
{
    uint64_t taskGeneration = 0;
    std::shared_ptr<const Offline::FilmSnapshot> film;
    nlohmann::json manifest;
};

// GT 冻结输入，prepare worker 建 CPU BVH，core worker 积分；RT 只读取发布结果。
// 两个 worker 均由此 owner 关闭/join，pending 永远只有最新请求。
class OfflineRenderCoordinator
{
public:
    OfflineRenderCoordinator();
    ~OfflineRenderCoordinator();
    void Observe(WorldSnapshotPtr snapshot,
                 std::shared_ptr<const OfflineWorldAssets> assets,
                 OfflineRenderSettings settings);
    void Pause();
    void Resume();
    void Stop();
    void Restart();
    Offline::SessionState State() const;
    std::string Error() const;
    std::shared_ptr<const OfflinePublishedResult> Capture() const;
    std::shared_ptr<const Offline::PreviewSnapshot> CapturePreview() const;
    static nlohmann::json DescribeInput(const WorldSnapshot& snapshot,
        const OfflineWorldAssets& assets, const OfflineRenderSettings& settings);

private:
    struct Request
    {
        uint64_t generation = 0;
        WorldSnapshotPtr snapshot;
        std::shared_ptr<const OfflineWorldAssets> assets;
        OfflineRenderSettings settings;
        std::chrono::steady_clock::time_point acceptedAt;
        nlohmann::json manifest;
    };
    void Run();
    mutable std::mutex mutex;
    std::condition_variable wake;
    Offline::RenderSession session;
    bool exiting = false;
    bool paused = false;
    bool stopped = false;
    uint64_t generation = 0;
    std::atomic<uint64_t> latestGeneration{0};
    uint64_t sessionGeneration = 0;
    int targetSamples = 0;
    WorldSnapshotPtr observedSnapshot;
    std::shared_ptr<const OfflineWorldAssets> observedAssets;
    OfflineRenderSettings observedSettings;
    std::string error;
    std::shared_ptr<const Request> pending;
    std::shared_ptr<const Request> active;
    // 只由 prepare worker 读写；World 换代清空，运行中的 session 另持有共享 mesh。
    uint64_t preparedMeshWorldGeneration = 0;
    std::unordered_map<std::string, std::shared_ptr<const Offline::Mesh>> preparedMeshes;
    std::unordered_map<std::string, OfflineWorldAssets::Section> preparedMeshSections;
    // 仅 worker 使用的临时记忆；持久摘要仍由顶点/索引内容计算。
    std::shared_ptr<const OfflineWorldAssets> preparedMeshDigestAssets;
    nlohmann::json preparedMeshDigests;
    Offline::SessionState state = Offline::SessionState::Idle;
    std::thread prepareWorker;
};

// 一次导出只消费一次 Capture；清单为最后发布标记，引用资产仍需原版本。
void ExportOfflineResult(const OfflinePublishedResult& result,
    const std::filesystem::path& outputDirectory, float displayExposure);
} // namespace VL
