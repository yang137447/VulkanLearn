#pragma once

#include <memory>
#include <string>

#include <Eigen/Core>

namespace VL
{

class DiagnosticsSubsystem;

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
    void PlayBalloonBurst(float intensity = 1.0f,
        const Eigen::Vector3f& position = Eigen::Vector3f::Zero());
    void PlayShardLanding(float intensity, const Eigen::Vector3f& position);
    // 气球漏气：持续嘶嘶声 + 低频塌陷，与爆裂音区分。
    void PlayBalloonLeak(float intensity, const Eigen::Vector3f& position);
    // 玻璃破碎：高频脆裂 transient + 多模态 tinkle 尾音。
    void PlayGlassShatter(float intensity, const Eigen::Vector3f& position);
    // 玻璃碎片落地：短促高频 ping；与气球落地音使用独立冷却。
    void PlayGlassShardLanding(float intensity, const Eigen::Vector3f& position);
    // 玻璃开裂（未碎）：短促高频 click + 2 个模态，区别于整块破碎。
    void PlayGlassCrack(float intensity, const Eigen::Vector3f& position);
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VL
