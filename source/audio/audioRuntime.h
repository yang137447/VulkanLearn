#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <Eigen/Core>

namespace VL
{

class DiagnosticsSubsystem;

enum class ImpactAudioKind
{
    Crack,
    Shatter,
    Leak,
    Burst,
    Landing,
    Subfracture
};

struct ImpactAudioEvent
{
    std::string audioProfile = "glass";
    ImpactAudioKind kind = ImpactAudioKind::Landing;
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    float intensity = 1.0f;
    float sizeScale = 1.0f;
    uint64_t seed = 0;
};

// 轻量音频运行时。优先加载资源目录中的 WAV 录音（气球、玻璃、漏气），
// 缺失时回退到程序化合成。使用 SDL3 双声道音频流；所有 Play* 只登记
// voice，由固定容量混音器叠加播放。
// 它不读取 World、不持有 Vulkan 对象，也不阻塞 GT。
class AudioRuntime
{
public:
    explicit AudioRuntime(
        DiagnosticsSubsystem& diagnostics, std::string resourceRoot = {});
    ~AudioRuntime();

    void SetListenerPosition(const Eigen::Vector3f& position);
    // 相机朝向用于等功率 pan；不传时默认 +Z 前向、+Y 向上。
    void SetListenerOrientation(
        const Eigen::Vector3f& forward, const Eigen::Vector3f& up);
    // 每帧补足 SDL 流队列；音效事件本身也会立即 Pump 一次，降低延迟。
    void Update();
    // 统一冲击音频入口；具体采样/合成由 audioProfile + kind 决定。
    void PlayImpact(const ImpactAudioEvent& event);
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VL
