#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/renderer.h"

namespace VL::Offline
{

struct RenderInput
{
    Scene scene;
    std::vector<std::shared_ptr<const Mesh>> instanceMeshes;
    std::vector<std::shared_ptr<const FabricMaterial>> instanceFabrics;
    std::vector<std::shared_ptr<const VelvetMaterial>> instanceVelvets;
    std::vector<std::shared_ptr<const SatinMaterial>> instanceSatins;
    std::vector<std::shared_ptr<const GroomMap>> instanceGrooms;
    std::vector<std::shared_ptr<const ColorMap>> instanceColorMaps;
    int width = 0;
    int height = 0;
    int targetSamples = 0;
    int bounces = 0;
    int lightSamples = 1;
    bool interactivePreview = false;
    // GT 接受请求时确定的绝对截止时间，包含 coordinator 的准备阶段。
    std::chrono::steady_clock::time_point previewDeadline{};
    // VL 透视相机使用世界空间正交基；core 不再构造独立 CLI 正交相机。
    double horizontalFovDegrees = 60.0;
    // 仅透视主射线使用；沿 cameraForward 的视空间深度，不限制反弹和阴影射线。
    double cameraClipNear = 0.1;
    double cameraClipFar = 1000.0;
    Vec3 cameraRight{1, 0, 0};
    Vec3 cameraUp{0, 1, 0};
    Vec3 cameraForward{0, 0, -1};
};

struct FilmSnapshot
{
    std::uint64_t generation = 0;
    int width = 0;
    int height = 0;
    int completedSamples = 0;
    int targetSamples = 0;
    std::vector<Vec3> direct;
    std::vector<Vec3> indirect;
};

struct PreviewSnapshot
{
    std::uint64_t generation = 0;
    int width = 0;
    int height = 0;
    // 未计算像素的 RGB 为零；仅 valid != 0 的像素可供视口显示，预览不可导出。
    std::vector<std::uint8_t> valid;
    std::vector<Vec3> direct;
    std::vector<Vec3> indirect;
    std::size_t validCount = 0;
};

enum class SessionState
{
    Idle,
    Preparing,
    Rendering,
    Paused,
    Completed,
    Canceled,
    Failed
};

// 协调 worker 持有累积与发布权；行 worker 只写互不相交的像素，GT/RT 只取得完整 sample 副本。
class RenderSession
{
public:
    // 0 按硬件线程数自动选择，并为 GT/RT 留出核心；显式值用于可重复的测试。
    explicit RenderSession(unsigned parallelism = 0);
    ~RenderSession();
    RenderSession(const RenderSession&) = delete;
    RenderSession& operator=(const RenderSession&) = delete;

    std::uint64_t Start(RenderInput input);
    bool ExtendTargetSamples(int targetSamples);
    void Pause();
    void Resume();
    void Stop();
    SessionState State() const;
    std::string Error() const;
    std::shared_ptr<const FilmSnapshot> Latest() const;
    std::shared_ptr<const PreviewSnapshot> LatestPreview() const;

private:
    struct SampleWork;
    struct PreviewWork;
    void Run();
    void Render(std::uint64_t generation, RenderInput input);
    void RenderPreview(std::uint64_t generation, const RenderInput& input);
    void RunPreviewWorker(PreviewWork& work);
    void RunSampleWorker(SampleWork& work);
    void RenderRows(SampleWork& work, int sample);

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<std::uint64_t> activeGeneration{0};
    std::atomic<bool> pauseRequested{false};
    unsigned parallelism;
    bool exiting = false;
    bool paused = false;
    bool previewMode = false;
    std::uint64_t generation = 0;
    int targetSamples = 0;
    std::shared_ptr<const RenderInput> pending;
    std::shared_ptr<const FilmSnapshot> latest;
    std::shared_ptr<const PreviewSnapshot> latestPreview;
    SessionState state = SessionState::Idle;
    std::string error;
    // 最后构造线程，保证其入口读取的锁、状态和 mailbox 已全部初始化。
    std::thread worker;
};

} // namespace VL::Offline
