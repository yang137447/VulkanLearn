#pragma once

#include <cstddef>
#include <vector>

#include <Eigen/Geometry>

namespace VL::Audio
{

// 混音格式与 SDL 设备格式一一对应。这里的策略不依赖 SDL，方便 GoogleTest
// 直接验证 voice 预算、等功率 pan 和距离低通。
inline constexpr int SampleRate = 48000;
inline constexpr int MixChannels = 2;
inline constexpr int MixBlockFrames = 128;
inline constexpr int MaxVoices = 32;

// 全局 voice 预算的优先级：同帧多个破碎事件时先保证 shatter/burst，
// 其次漏气长音，最后才是碎片落地这种高频低信息量事件。
enum class VoicePriority : int
{
    Landing = 0,
    Leak = 1,
    Impact = 2,
};

struct ListenerState
{
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f forward = Eigen::Vector3f(0.0f, 0.0f, 1.0f);
    Eigen::Vector3f up = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
};

struct SpatialParams
{
    float distance = 0.0f;
    // 距离衰减，取值与旧版单声道路径一致：1/(1+d/200)，下限 0.04。
    float gain = 1.0f;
    // -1 = 完全左声道，+1 = 完全右声道。
    float pan = 0.0f;
    // 等功率 pan：leftGain² + rightGain² == 1。
    float leftGain = 0.70710678f;
    float rightGain = 0.70710678f;
    float lowpassCutoffHz = 20000.0f;
    float lowpassAlpha = 1.0f;
};

// 一阶低通的系数：a = 1 - exp(-2π fc / fs)。cutoff 会被夹到
// [20 Hz, 0.45 * sampleRate]，保证系数落在 (0, 1]。
float LowpassAlphaForCutoff(float cutoffHz, int sampleRate = SampleRate);

// 距离越远，截止频率从 LowpassNearHz 指数下降到 LowpassFarHz。
// 只做一阶低通，刻意不模拟空气吸收/HRTF。
SpatialParams ComputeSpatialParams(
    const ListenerState& listener, const Eigen::Vector3f& sourcePosition);

struct VoiceSubmitResult
{
    bool accepted = false;
    int slot = -1;
    // 被抢占的槽位；未发生抢占时为 -1。
    int stolenSlot = -1;
};

struct VoiceStatus
{
    bool active = false;
    VoicePriority priority = VoicePriority::Landing;
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    std::size_t remainingFrames = 0;
};

// 固定容量 voice 池 + 块混音。Submit 只登记，MixBlock 才真正叠加样本；
// 多个音效因此是同时发声，而不是在 SDL 流里排队。
class VoiceMixer
{
public:
    explicit VoiceMixer(int capacity = MaxVoices);

    void SetListener(const ListenerState& listener);
    const ListenerState& GetListener() const { return listener; }

    // 满了以后按 priority > 距离 > 剩余时长选择被抢占的 voice；
    // 如果新 voice 不比池里最不重要的 voice 更重要，则拒绝（accepted=false）。
    VoiceSubmitResult Submit(
        VoicePriority priority,
        const Eigen::Vector3f& position,
        std::vector<float> samples);

    // interleavedStereo 至少 frames * 2 个 float；frames <= MixBlockFrames。
    void MixBlock(float* interleavedStereo, int frames);

    void Reset();

    int Capacity() const { return static_cast<int>(voices.size()); }
    int ActiveVoiceCount() const;
    VoiceStatus GetVoiceStatus(int slot) const;

private:
    struct Voice
    {
        bool active = false;
        VoicePriority priority = VoicePriority::Landing;
        Eigen::Vector3f position = Eigen::Vector3f::Zero();
        std::vector<float> samples;
        std::size_t cursor = 0;
        // 低通状态跨 block 保留，避免每个 128 帧块重启滤波器产生咔哒声。
        float filterState = 0.0f;
    };

    float DistanceToListener(const Eigen::Vector3f& position) const;
    int SelectStealCandidate(
        VoicePriority incomingPriority,
        float incomingDistance,
        std::size_t incomingRemaining) const;

    ListenerState listener;
    std::vector<Voice> voices;
};

} // namespace VL::Audio
