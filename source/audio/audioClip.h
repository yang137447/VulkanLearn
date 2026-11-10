#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace VL::Audio
{

// WAV 解码后的单声道浮点样本。资产可以有不同的采样率与声道数，
// 运行时只把这里的结果重采样到 Mixer 的 48 kHz。
struct AudioClip
{
    int sampleRate = 0;
    std::vector<float> samples;
};

// 支持 RIFF/WAVE 的 PCM 8/16/24/32-bit 与 IEEE float 32/64-bit，
// 包括 WAVE_FORMAT_EXTENSIBLE。压缩 WAV、无数据块或截断文件返回 false。
bool LoadWavFile(
    const std::filesystem::path& path, AudioClip& clip, std::string& error);

// 线性插值重采样。对 44.1 kHz -> 48 kHz 这类小幅变换足够，
// 不做 sinc 滤波；长采样会按目标长度精确取整，避免尾部累积误差。
std::vector<float> ResampleLinear(
    const std::vector<float>& samples, int sourceRate, int targetRate);

// 从 48 kHz 样本中截取一段并施加线性淡入/淡出，用于去掉录音首尾
// 的静音和环境残响。起止时间以秒为单位，超出范围时自动裁剪。
std::vector<float> MakeSegment(
    const std::vector<float>& samples, int sampleRate,
    double startSeconds, double durationSeconds,
    double fadeInSeconds, double fadeOutSeconds);

float PeakAmplitude(const std::vector<float>& samples);
void ApplyGain(std::vector<float>& samples, float gain);

} // namespace VL::Audio
