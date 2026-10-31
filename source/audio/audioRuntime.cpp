#include "audio/audioRuntime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "SDL3/SDL.h"

#include "audio/audioClip.h"
#include "audio/audioMixer.h"
#include "engine/diagnosticsSubsystem.h"

namespace VL
{
namespace
{

constexpr double DurationBalloonBurst = 0.55;
constexpr double DurationBalloonLeak = 0.90;
constexpr double DurationGlassShatter = 0.48;
constexpr double DurationShardLanding = 0.10;
constexpr double DurationGlassShardLanding = 0.12;
constexpr double DurationGlassCrack = 0.14;

// 没有音频回调线程时，排队深度至少要覆盖一帧 60 FPS；1024 帧约 21 ms，
// 比原先 2048 帧（约 43 ms）明显降低命中拖尾，同时仍留出一帧余量。
constexpr int TargetQueuedFrames = 1024;
// 掉帧后一次最多补 3072 帧（64 ms），防止长时间 hitch 后一次性灌满整段音频。
constexpr int MaxMixBlocksPerPump = 24;

struct SplitMix64
{
    uint64_t state = 0;

    uint64_t Next()
    {
        uint64_t value = (state += 0x9E3779B97F4A7C15ull);
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31);
    }

    float Signed()
    {
        return static_cast<float>(Next() >> 40) * (2.0f / 16777215.0f) - 1.0f;
    }
};

void AddLayer(
    std::vector<float>& target, const std::vector<float>& source, float gain)
{
    const std::size_t count = std::min(target.size(), source.size());
    for (std::size_t index = 0; index < count; ++index)
        target[index] += source[index] * gain;
}

void ClampSamples(std::vector<float>& samples)
{
    for (float& sample : samples)
    {
        sample = std::clamp(
            std::isfinite(sample) ? sample : 0.0f, -1.0f, 1.0f);
    }
}

} // namespace

struct AudioRuntime::Impl
{
    DiagnosticsSubsystem& diagnostics;
    std::string resourceRoot;
    SDL_AudioStream* stream = nullptr;
    bool audioSubsystemReady = false;
    bool warnedUnavailable = false;
    Audio::VoiceMixer mixer;
    std::vector<float> mixScratch =
        std::vector<float>(Audio::MixBlockFrames * Audio::MixChannels, 0.0f);
    std::chrono::steady_clock::time_point lastLanding;
    bool hasLastLanding = false;
    std::chrono::steady_clock::time_point lastGlassLanding;
    bool hasLastGlassLanding = false;
    // 录音素材与它们的播放速率变体。变体在加载时一次性生成，
    // 播放时只复制样本，不在 GT 上做重采样。
    std::vector<std::vector<float>> balloonBurstVariants;
    std::vector<std::vector<float>> glassShatterVariants;
    std::vector<std::vector<float>> glassLandingVariants;
    std::vector<float> balloonLeakClip;
    std::vector<float> glassCrackClip;
    std::uint64_t clipEventSerial = 0;

    explicit Impl(
        DiagnosticsSubsystem& diagnostics, std::string resourceRoot)
        : diagnostics(diagnostics)
        , resourceRoot(std::move(resourceRoot))
    {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            diagnostics.ReportWarning(
                std::string("Audio unavailable: ") + SDL_GetError());
            return;
        }
        audioSubsystemReady = true;
        // 输出必须与 VoiceMixer 一致：48 kHz、F32、双声道交错。
        SDL_AudioSpec spec{SDL_AUDIO_F32, Audio::MixChannels, Audio::SampleRate};
        stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (stream == nullptr)
        {
            diagnostics.ReportWarning(
                std::string("Audio device unavailable: ") + SDL_GetError());
            return;
        }
        SDL_ResumeAudioStreamDevice(stream);
        diagnostics.ReportInfo(
            "Audio stream ready: 48000 Hz stereo float, 32 voices");
        LoadAudioAssets();
    }

    ~Impl()
    {
        if (stream) SDL_DestroyAudioStream(stream);
        if (audioSubsystemReady) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    std::filesystem::path ClipPath(const char* fileName) const
    {
        return std::filesystem::path(resourceRoot) /
            "Common" / "Audio" / fileName;
    }

    bool LoadClip48k(const char* fileName, std::vector<float>& samples)
    {
        const std::filesystem::path path = ClipPath(fileName);
        Audio::AudioClip clip;
        std::string error;
        if (!Audio::LoadWavFile(path, clip, error))
        {
            diagnostics.ReportWarning(
                std::string("Audio clip unavailable: ") + path.string() +
                " (" + error + ")");
            return false;
        }
        samples = Audio::ResampleLinear(
            clip.samples, clip.sampleRate, Audio::SampleRate);
        if (samples.empty())
        {
            diagnostics.ReportWarning(
                std::string("Audio clip is empty: ") + path.string());
            return false;
        }
        diagnostics.ReportInfo(
            std::string("Audio clip loaded: ") + fileName +
            " " + std::to_string(clip.sampleRate) + " Hz -> " +
            std::to_string(Audio::SampleRate) + " Hz, " +
            std::to_string(
                static_cast<double>(clip.samples.size()) /
                static_cast<double>(clip.sampleRate)) + " s");
        return true;
    }

    void BuildRateVariants(
        const std::vector<float>& base,
        const std::vector<float>& rates,
        std::vector<std::vector<float>>& variants)
    {
        variants.clear();
        for (float rate : rates)
        {
            if (rate <= 0.0f) continue;
            const int targetRate = static_cast<int>(std::lround(
                static_cast<double>(Audio::SampleRate) / rate));
            // 以目标采样率改变播放速度：rate > 1 时声音更短、音高更高。
            variants.push_back(Audio::ResampleLinear(
                base, Audio::SampleRate, std::max(1, targetRate)));
        }
    }

    void LoadAudioAssets()
    {
        if (resourceRoot.empty())
        {
            diagnostics.ReportWarning(
                "Audio clips skipped: resource root is empty");
            return;
        }

        std::vector<float> balloonPop;
        if (LoadClip48k("balloon_pop.wav", balloonPop))
        {
            balloonPop = Audio::MakeSegment(
                balloonPop, Audio::SampleRate, 0.0, 0.56, 0.001, 0.02);
            // 原始录音峰值约 -6 dB；抬到与玻璃破碎可比的响度。
            Audio::ApplyGain(balloonPop, 1.10f);
            BuildRateVariants(
                balloonPop, {0.94f, 1.0f, 1.06f}, balloonBurstVariants);
        }

        std::vector<float> glass;
        if (LoadClip48k("glass_break.wav", glass))
        {
            // 原始录音前 50 ms 是低电平预兆，主冲击约在 70 ms；截掉前段
            // 但保留 15 ms 上升沿，并用额外的 click 对齐命中帧。
            std::vector<float> shatter = Audio::MakeSegment(
                glass, Audio::SampleRate, 0.052, 0.58, 0.002, 0.035);
            Audio::ApplyGain(shatter, 0.72f);
            BuildRateVariants(
                shatter, {0.95f, 1.0f, 1.05f}, glassShatterVariants);

            glassCrackClip = Audio::MakeSegment(
                glass, Audio::SampleRate, 0.050, 0.16, 0.001, 0.09);
            Audio::ApplyGain(glassCrackClip, 0.78f);

            // 落地音从破碎尾音的若干位置切片，避免每片都是同一个 ping。
            const double landingOffsets[] = {0.20, 0.27, 0.35, 0.42};
            const std::vector<float> landingRates = {0.94f, 1.0f, 1.06f};
            for (double offset : landingOffsets)
            {
                std::vector<float> landing = Audio::MakeSegment(
                    glass, Audio::SampleRate, offset, 0.22, 0.001, 0.16);
                Audio::ApplyGain(landing, 3.20f);
                for (float rate : landingRates)
                {
                    const int targetRate = static_cast<int>(std::lround(
                        static_cast<double>(Audio::SampleRate) / rate));
                    glassLandingVariants.push_back(Audio::ResampleLinear(
                        landing, Audio::SampleRate, std::max(1, targetRate)));
                }
            }
        }

        std::vector<float> gasLeak;
        const bool hasGasLeak = LoadClip48k("gas_leak.wav", gasLeak);
        if (hasGasLeak)
        {
            gasLeak = Audio::MakeSegment(
                gasLeak, Audio::SampleRate, 0.0, 1.15, 0.03, 0.18);
            Audio::ApplyGain(gasLeak, 1.60f);
        }

        std::vector<float> inflate;
        const bool hasInflate = LoadClip48k("balloon_inflate.wav", inflate);
        if (hasInflate)
        {
            // 泄气时气流/橡胶褶皱方向与充气相反；把录音末段反向播放，
            // 作为漏气的“膜体”层，而不是再加一段纯噪声。
            inflate = Audio::MakeSegment(
                inflate, Audio::SampleRate, 1.0, 1.15, 0.02, 0.20);
            std::reverse(inflate.begin(), inflate.end());
            Audio::ApplyGain(inflate, 0.75f);
        }

        if (hasGasLeak || hasInflate)
        {
            const std::size_t length =
                std::max(gasLeak.size(), inflate.size());
            balloonLeakClip.assign(length, 0.0f);
            if (hasGasLeak) AddLayer(balloonLeakClip, gasLeak, 1.0f);
            if (hasInflate) AddLayer(balloonLeakClip, inflate, 0.55f);
            ClampSamples(balloonLeakClip);
        }
    }

    void SetListenerPosition(const Eigen::Vector3f& position)
    {
        mixer.SetListener(MakeListener(position, currentForward, currentUp));
    }

    void SetListenerOrientation(
        const Eigen::Vector3f& forward, const Eigen::Vector3f& up)
    {
        currentForward = forward;
        currentUp = up;
        const Audio::ListenerState listener = mixer.GetListener();
        mixer.SetListener(MakeListener(listener.position, forward, up));
    }

    Audio::ListenerState MakeListener(
        const Eigen::Vector3f& position,
        const Eigen::Vector3f& forward,
        const Eigen::Vector3f& up) const
    {
        Audio::ListenerState listener;
        listener.position = position;
        listener.forward = forward;
        listener.up = up;
        return listener;
    }

    void WarnUnavailable()
    {
        if (warnedUnavailable) return;
        warnedUnavailable = true;
        diagnostics.ReportWarning("Audio playback skipped: audio stream unavailable");
    }

    // 把当前活跃 voice 混成若干 128 帧的 stereo block，补到目标排队深度。
    // 没有 voice 时仍然写静音，保持设备流连续。
    void Pump()
    {
        if (stream == nullptr) return;
        const int bytesPerFrame =
            static_cast<int>(sizeof(float)) * Audio::MixChannels;
        int queuedFrames = SDL_GetAudioStreamQueued(stream) / bytesPerFrame;
        for (int block = 0;
             block < MaxMixBlocksPerPump &&
                 queuedFrames + Audio::MixBlockFrames <= TargetQueuedFrames;
             ++block)
        {
            std::fill(mixScratch.begin(), mixScratch.end(), 0.0f);
            mixer.MixBlock(mixScratch.data(), Audio::MixBlockFrames);
            const int bytes = static_cast<int>(
                mixScratch.size() * sizeof(float));
            if (!SDL_PutAudioStreamData(stream, mixScratch.data(), bytes))
            {
                diagnostics.ReportWarning(
                    std::string("Audio mix queue failed: ") + SDL_GetError());
                return;
            }
            queuedFrames += Audio::MixBlockFrames;
        }
    }

    bool Submit(
        Audio::VoicePriority priority,
        const Eigen::Vector3f& position,
        std::vector<float> samples)
    {
        if (stream == nullptr)
        {
            WarnUnavailable();
            return false;
        }
        const Audio::VoiceSubmitResult result =
            mixer.Submit(priority, position, std::move(samples));
        if (!result.accepted) return false;
        Pump();
        return true;
    }

    void PlayBalloonBurst(float intensity, const Eigen::Vector3f& position)
    {
        intensity = std::clamp(intensity, 0.0f, 2.0f);
        if (!balloonBurstVariants.empty())
        {
            // 录音给出真实的乳胶爆裂宽带与房间尾音；再叠极短的压力脉冲和
            // 橡胶回弹，让“箭尖刺破”的命中感落在同一帧，而不是只听到录音。
            const float assetIntensity = std::min(intensity, 1.25f);
            std::vector<float> samples = balloonBurstVariants[
                static_cast<std::size_t>(
                    clipEventSerial++ % balloonBurstVariants.size())];
            const int sampleCount = static_cast<int>(samples.size());
            SplitMix64 random{
                static_cast<std::uint64_t>(sampleCount) * 0x9E3779B9ull +
                static_cast<std::uint64_t>(assetIntensity * 1000.0f)};
            for (int index = 0; index < sampleCount; ++index)
            {
                const float time = static_cast<float>(index) /
                    static_cast<float>(Audio::SampleRate);
                const float normalized = time / 0.56f;
                const float pressure =
                    std::exp(-time * 22.0f) *
                    std::sin(6.2831853f *
                        (78.0f + 24.0f * (1.0f - normalized)) * time);
                const float rubberTime = std::max(0.0f, time - 0.022f);
                const float rubber = time >= 0.022f
                    ? std::sin(6.2831853f *
                        (540.0f + 320.0f * (1.0f - normalized)) * time) *
                        std::exp(-rubberTime * 17.0f)
                    : 0.0f;
                const float flutterTime = std::max(0.0f, time - 0.12f);
                const float flutter = time >= 0.12f
                    ? random.Signed() * std::exp(-flutterTime * 7.0f)
                    : 0.0f;
                samples[static_cast<std::size_t>(index)] = std::clamp(
                    assetIntensity * (samples[static_cast<std::size_t>(index)] +
                        0.14f * pressure + 0.10f * rubber + 0.10f * flutter),
                    -1.0f, 1.0f);
            }
            if (Submit(
                    Audio::VoicePriority::Impact, position, std::move(samples)))
            {
                diagnostics.ReportInfo("Balloon burst audio queued (asset)");
            }
            return;
        }
        // 气球爆裂是宽带瞬态：高频脆裂先到，压力脉冲和橡胶回弹紧随其后，
        // 最后保留一小段空气/碎片 flutter，而不是单一低频 boom。
        const int sampleCount =
            static_cast<int>(Audio::SampleRate * DurationBalloonBurst);
        std::vector<float> samples(static_cast<size_t>(sampleCount));
        SplitMix64 random{static_cast<uint64_t>(sampleCount) * 0x9E3779B9ull +
            static_cast<uint64_t>(intensity * 1000.0f)};
        float previousNoise = 0.0f;
        for (int index = 0; index < sampleCount; ++index)
        {
            const float time = static_cast<float>(index) /
                static_cast<float>(Audio::SampleRate);
            const float normalized = time /
                static_cast<float>(DurationBalloonBurst);
            const float noise = random.Signed();
            const float highPass = noise - previousNoise;
            previousNoise = noise;
            const float pressure = std::exp(-time * 18.0f) *
                std::sin(6.2831853f * (82.0f + 38.0f * (1.0f - normalized)) * time);
            const float crackle = highPass * std::exp(-time * 110.0f);
            const float rubberTime = std::max(0.0f, time - 0.028f);
            const float rubber = time >= 0.028f
                ? std::sin(6.2831853f * (520.0f + 340.0f * (1.0f - normalized)) * time) *
                    std::exp(-rubberTime * 15.0f)
                : 0.0f;
            const float flutterTime = std::max(0.0f, time - 0.14f);
            const float flutter = time >= 0.14f
                ? noise * std::exp(-flutterTime * 4.0f) * 0.20f
                : 0.0f;
            const float sample = intensity *
                (0.52f * crackle + 0.42f * pressure + 0.30f * rubber + flutter);
            samples[static_cast<size_t>(index)] = std::clamp(sample, -1.0f, 1.0f);
        }
        if (Submit(Audio::VoicePriority::Impact, position, std::move(samples)))
            diagnostics.ReportInfo("Balloon burst audio queued");
    }

    void PlayShardLanding(float intensity, const Eigen::Vector3f& position)
    {
        if (stream == nullptr)
        {
            WarnUnavailable();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (hasLastLanding && now - lastLanding < std::chrono::milliseconds(24))
            return;
        hasLastLanding = true;
        lastLanding = now;

        intensity = std::clamp(intensity, 0.02f, 1.0f);
        const int sampleCount =
            static_cast<int>(Audio::SampleRate * DurationShardLanding);
        std::vector<float> samples(static_cast<size_t>(sampleCount));
        SplitMix64 random{static_cast<uint64_t>(sampleCount) * 0x85EBCA6Bull};
        const float frequency = 900.0f + 900.0f * intensity;
        for (int index = 0; index < sampleCount; ++index)
        {
            const float time = static_cast<float>(index) /
                static_cast<float>(Audio::SampleRate);
            const float ring = std::sin(6.2831853f * frequency * time) *
                std::exp(-time * 55.0f);
            const float click = random.Signed() * std::exp(-time * 110.0f);
            samples[static_cast<size_t>(index)] = std::clamp(
                (0.40f * ring + 0.28f * click) * intensity, -1.0f, 1.0f);
        }
        if (Submit(Audio::VoicePriority::Landing, position, std::move(samples)))
            diagnostics.ReportInfo(
                "Shard landing audio queued intensity=" + std::to_string(intensity));
    }

    void PlayBalloonLeak(float intensity, const Eigen::Vector3f& position)
    {
        intensity = std::clamp(intensity, 0.0f, 1.0f);
        if (!balloonLeakClip.empty())
        {
            // 漏气由两层构成：真实气体泄漏噪声 + 反向充气录音的橡胶褶皱。
            // 再叠一个很轻的 150 Hz 抖动，避免长音听起来像静态白噪声。
            std::vector<float> samples = balloonLeakClip;
            const int sampleCount = static_cast<int>(samples.size());
            for (int index = 0; index < sampleCount; ++index)
            {
                const float time = static_cast<float>(index) /
                    static_cast<float>(Audio::SampleRate);
                const float normalized = time / 1.15f;
                const float wobble = std::sin(6.2831853f *
                    (150.0f + 30.0f * (1.0f - normalized)) * time) *
                    std::exp(-time * 1.5f);
                samples[static_cast<std::size_t>(index)] = std::clamp(
                    intensity * (samples[static_cast<std::size_t>(index)] +
                        0.035f * wobble),
                    -1.0f, 1.0f);
            }
            if (Submit(
                    Audio::VoicePriority::Leak, position, std::move(samples)))
            {
                diagnostics.ReportInfo("Balloon leak audio queued (asset)");
            }
            return;
        }
        // 漏气是长尾嘶声：高通噪声缓慢衰减，叠一点低频塌陷，没有爆裂 attack。
        const int sampleCount =
            static_cast<int>(Audio::SampleRate * DurationBalloonLeak);
        std::vector<float> samples(static_cast<size_t>(sampleCount));
        SplitMix64 random{static_cast<uint64_t>(sampleCount) * 0x9E3779B1ull};
        float previousNoise = 0.0f;
        for (int index = 0; index < sampleCount; ++index)
        {
            const float time = static_cast<float>(index) /
                static_cast<float>(Audio::SampleRate);
            const float noise = random.Signed();
            const float hiss = (noise - previousNoise) * std::exp(-time * 3.2f);
            previousNoise = noise;
            const float flutter = std::sin(6.2831853f * 170.0f * time) *
                std::exp(-time * 6.0f);
            samples[static_cast<size_t>(index)] = std::clamp(
                intensity * (0.42f * hiss + 0.10f * flutter), -1.0f, 1.0f);
        }
        if (Submit(Audio::VoicePriority::Leak, position, std::move(samples)))
            diagnostics.ReportInfo("Balloon leak audio queued");
    }

    void PlayGlassShatter(float intensity, const Eigen::Vector3f& position)
    {
        intensity = std::clamp(intensity, 0.0f, 2.0f);
        if (!glassShatterVariants.empty())
        {
            // 录音包含真实玻璃碎裂的多模态尾音；额外 click 和低频冲击只负责
            // 把声音起始钉在命中帧，不替代录音本身的材质信息。
            const float assetIntensity = std::min(intensity, 1.25f);
            std::vector<float> samples = glassShatterVariants[
                static_cast<std::size_t>(
                    clipEventSerial++ % glassShatterVariants.size())];
            const int sampleCount = static_cast<int>(samples.size());
            SplitMix64 random{
                static_cast<std::uint64_t>(sampleCount) * 0xC2B2AE35ull +
                static_cast<std::uint64_t>(assetIntensity * 1000.0f)};
            float previousNoise = 0.0f;
            for (int index = 0; index < sampleCount; ++index)
            {
                const float time = static_cast<float>(index) /
                    static_cast<float>(Audio::SampleRate);
                const float noise = random.Signed();
                const float highPass = noise - previousNoise;
                previousNoise = noise;
                const float click = highPass * std::exp(-time * 150.0f);
                const float thump = std::sin(
                    6.2831853f * 118.0f * time) * std::exp(-time * 20.0f);
                samples[static_cast<std::size_t>(index)] = std::clamp(
                    assetIntensity *
                        (samples[static_cast<std::size_t>(index)] +
                            0.12f * click + 0.07f * thump),
                    -1.0f, 1.0f);
            }
            if (Submit(
                    Audio::VoicePriority::Impact, position, std::move(samples)))
            {
                diagnostics.ReportInfo("Glass shatter audio queued (asset)");
            }
            return;
        }
        // 玻璃是高频、短衰减材料：先用高通噪声给出脆裂 attack，再叠 4 个
        // 快速衰减的模态谐振作为碎片 tinkle，最后留一点低频板体冲击。
        const int sampleCount =
            static_cast<int>(Audio::SampleRate * DurationGlassShatter);
        std::vector<float> samples(static_cast<size_t>(sampleCount));
        SplitMix64 random{static_cast<uint64_t>(sampleCount) * 0xC2B2AE35ull +
            static_cast<uint64_t>(intensity * 1000.0f)};
        const float modes[4] = {2380.0f, 3470.0f, 5210.0f, 7180.0f};
        const float decay[4] = {19.0f, 24.0f, 31.0f, 44.0f};
        const float modeGain[4] = {0.22f, 0.20f, 0.16f, 0.12f};
        float previousNoise = 0.0f;
        for (int index = 0; index < sampleCount; ++index)
        {
            const float time = static_cast<float>(index) /
                static_cast<float>(Audio::SampleRate);
            const float noise = random.Signed();
            const float highPass = noise - previousNoise;
            previousNoise = noise;
            const float crackle = highPass * std::exp(-time * 95.0f);
            float tinkle = 0.0f;
            for (int mode = 0; mode < 4; ++mode)
            {
                tinkle += modeGain[mode] *
                    std::sin(6.2831853f * modes[mode] * time) *
                    std::exp(-time * decay[mode]);
            }
            const float thump = std::sin(6.2831853f * 118.0f * time) *
                std::exp(-time * 13.0f);
            const float sample = intensity *
                (0.55f * crackle + tinkle + 0.16f * thump);
            samples[static_cast<size_t>(index)] = std::clamp(sample, -1.0f, 1.0f);
        }
        if (Submit(Audio::VoicePriority::Impact, position, std::move(samples)))
            diagnostics.ReportInfo("Glass shatter audio queued");
    }

    void PlayGlassCrack(float intensity, const Eigen::Vector3f& position)
    {
        intensity = std::clamp(intensity, 0.02f, 1.0f);
        if (!glassCrackClip.empty())
        {
            // 从真实破碎录音的 attack 段切片，保留玻璃材质音色；用较短的
            // 包络和低增益把它压成“开裂”而不是一次完整破碎。
            std::vector<float> samples = glassCrackClip;
            const int sampleCount = static_cast<int>(samples.size());
            SplitMix64 random{
                static_cast<std::uint64_t>(sampleCount) * 0x85EBCA77ull +
                static_cast<std::uint64_t>(intensity * 1000.0f)};
            for (int index = 0; index < sampleCount; ++index)
            {
                const float time = static_cast<float>(index) /
                    static_cast<float>(Audio::SampleRate);
                const float click =
                    random.Signed() * std::exp(-time * 180.0f);
                const float ring = std::sin(
                    6.2831853f * 5200.0f * time) * std::exp(-time * 55.0f);
                const float ringHigh = std::sin(
                    6.2831853f * 7800.0f * time) * std::exp(-time * 80.0f);
                samples[static_cast<std::size_t>(index)] = std::clamp(
                    intensity *
                        (samples[static_cast<std::size_t>(index)] +
                            0.45f * click + 0.25f * ring + 0.15f * ringHigh),
                    -1.0f, 1.0f);
            }
            if (Submit(
                    Audio::VoicePriority::Impact, position, std::move(samples)))
            {
                diagnostics.ReportInfo("Glass crack audio queued (asset)");
            }
            return;
        }
        const int sampleCount =
            static_cast<int>(Audio::SampleRate * DurationGlassCrack);
        std::vector<float> samples(static_cast<size_t>(sampleCount));
        SplitMix64 random{static_cast<uint64_t>(sampleCount) * 0x85EBCA77ull +
            static_cast<uint64_t>(intensity * 1000.0f)};
        for (int index = 0; index < sampleCount; ++index)
        {
            const float time = static_cast<float>(index) /
                static_cast<float>(Audio::SampleRate);
            // 开裂是玻璃破碎的“前半段”：只有高频 click 和两个短模态，没有低频冲击。
            const float click = random.Signed() * std::exp(-time * 160.0f);
            const float ring = std::sin(6.2831853f * 5200.0f * time) *
                std::exp(-time * 55.0f);
            const float ringHigh = std::sin(6.2831853f * 7800.0f * time) *
                std::exp(-time * 80.0f);
            samples[static_cast<size_t>(index)] = std::clamp(
                intensity * (0.45f * click + 0.30f * ring + 0.18f * ringHigh),
                -1.0f, 1.0f);
        }
        if (Submit(Audio::VoicePriority::Impact, position, std::move(samples)))
            diagnostics.ReportInfo("Glass crack audio queued");
    }

    void PlayGlassShardLanding(float intensity, const Eigen::Vector3f& position)
    {
        if (stream == nullptr)
        {
            WarnUnavailable();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (hasLastGlassLanding &&
            now - lastGlassLanding < std::chrono::milliseconds(30))
            return;
        hasLastGlassLanding = true;
        lastGlassLanding = now;

        intensity = std::clamp(intensity, 0.02f, 1.0f);
        if (!glassLandingVariants.empty())
        {
            // 切片本身是真实玻璃尾音的随机位置；再加同步 click，
            // 使每次落地仍有明确接触点。
            std::vector<float> samples = glassLandingVariants[
                static_cast<std::size_t>(
                    clipEventSerial++ % glassLandingVariants.size())];
            const int sampleCount = static_cast<int>(samples.size());
            SplitMix64 random{
                static_cast<std::uint64_t>(sampleCount) * 0x27D4EB2Full +
                static_cast<std::uint64_t>(intensity * 1000.0f)};
            for (int index = 0; index < sampleCount; ++index)
            {
                const float time = static_cast<float>(index) /
                    static_cast<float>(Audio::SampleRate);
                const float click =
                    random.Signed() * std::exp(-time * 180.0f);
                samples[static_cast<std::size_t>(index)] = std::clamp(
                    intensity *
                        (samples[static_cast<std::size_t>(index)] +
                            0.20f * click),
                    -1.0f, 1.0f);
            }
            if (Submit(
                    Audio::VoicePriority::Landing, position, std::move(samples)))
            {
                diagnostics.ReportInfo(
                    "Glass shard landing audio queued (asset) intensity=" +
                    std::to_string(intensity));
            }
            return;
        }
        const int sampleCount =
            static_cast<int>(Audio::SampleRate * DurationGlassShardLanding);
        std::vector<float> samples(static_cast<size_t>(sampleCount));
        SplitMix64 random{static_cast<uint64_t>(sampleCount) * 0x27D4EB2Full +
            static_cast<uint64_t>(intensity * 1000.0f)};
        // 用强度微调音高：轻碰更尖、重落更沉，避免每片都是同一个音。
        const float frequency = 4200.0f - 1400.0f * intensity;
        for (int index = 0; index < sampleCount; ++index)
        {
            const float time = static_cast<float>(index) /
                static_cast<float>(Audio::SampleRate);
            const float ring = std::sin(6.2831853f * frequency * time) *
                std::exp(-time * 42.0f);
            const float click = random.Signed() * std::exp(-time * 120.0f);
            samples[static_cast<size_t>(index)] = std::clamp(
                (0.34f * ring + 0.22f * click) * intensity, -1.0f, 1.0f);
        }
        if (Submit(Audio::VoicePriority::Landing, position, std::move(samples)))
            diagnostics.ReportInfo(
                "Glass shard landing audio queued intensity=" + std::to_string(intensity));
    }

    Eigen::Vector3f currentForward = Eigen::Vector3f(0.0f, 0.0f, 1.0f);
    Eigen::Vector3f currentUp = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
};

AudioRuntime::AudioRuntime(
    DiagnosticsSubsystem& diagnostics, std::string resourceRoot)
    : impl(std::make_unique<Impl>(diagnostics, std::move(resourceRoot)))
{
}

AudioRuntime::~AudioRuntime() = default;

void AudioRuntime::SetListenerPosition(const Eigen::Vector3f& position)
{
    if (impl) impl->SetListenerPosition(position);
}

void AudioRuntime::SetListenerOrientation(
    const Eigen::Vector3f& forward, const Eigen::Vector3f& up)
{
    if (impl) impl->SetListenerOrientation(forward, up);
}

void AudioRuntime::Update()
{
    if (impl) impl->Pump();
}

void AudioRuntime::PlayImpact(const ImpactAudioEvent& event)
{
    if (!impl) return;
    if (event.audioProfile == "glass")
    {
        switch (event.kind)
        {
        case ImpactAudioKind::Crack:
            impl->PlayGlassCrack(event.intensity, event.position);
            return;
        case ImpactAudioKind::Shatter:
            impl->PlayGlassShatter(event.intensity, event.position);
            return;
        case ImpactAudioKind::Subfracture:
            impl->PlayGlassCrack(
                event.intensity * 0.75f, event.position);
            return;
        case ImpactAudioKind::Landing:
            impl->PlayGlassShardLanding(event.intensity, event.position);
            return;
        case ImpactAudioKind::Burst:
        case ImpactAudioKind::Leak:
            return;
        }
    }
    if (event.audioProfile == "balloon")
    {
        switch (event.kind)
        {
        case ImpactAudioKind::Burst:
            impl->PlayBalloonBurst(event.intensity, event.position);
            return;
        case ImpactAudioKind::Leak:
            impl->PlayBalloonLeak(event.intensity, event.position);
            return;
        case ImpactAudioKind::Landing:
        case ImpactAudioKind::Subfracture:
            impl->PlayShardLanding(event.intensity, event.position);
            return;
        case ImpactAudioKind::Crack:
        case ImpactAudioKind::Shatter:
            return;
        }
    }
}

void AudioRuntime::Shutdown()
{
    impl.reset();
}

} // namespace VL
