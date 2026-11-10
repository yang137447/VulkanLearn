#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace VL
{
namespace Editor
{

enum class BrushShape
{
    Circle,
    Square,
    Stamp
};

enum class BrushFalloff
{
    Smooth,
    Linear,
    Constant,
    Lut
};

struct BrushProfile
{
    std::string name;
    std::string category;
    BrushShape shape = BrushShape::Circle;
    BrushFalloff falloff = BrushFalloff::Smooth;
    float radiusCm = 500.0f;
    float strength = 0.1f;
    // 相邻两次落点圆心距占当前直径的比例。
    float spacing = 0.25f;
    // 落点位置抖动，单位为当前 brush 半径的比例。
    float jitter = 0.0f;
    float rotationDeg = 0.0f;
    float noise = 0.0f;
    uint32_t noiseSeed = 0;
    float pressureSizeInfluence = 0.0f;
    float pressureStrengthInfluence = 0.0f;
    // falloff == Lut 时使用；t 从 0（圆心）线性映射到 1（边缘）。
    std::vector<float> falloffLut;
    // shape == Stamp 时使用。
    std::string stampPath;
};

bool Validate(const BrushProfile& profile, std::string& error);

nlohmann::json ToJson(const BrushProfile& profile);
bool FromJson(
    const nlohmann::json& value,
    BrushProfile& profile,
    std::string& error);

// 预设身份由名称稳定派生；两套领域目录各自保证名称唯一，因此 id 也唯一。
std::string BrushProfileId(const BrushProfile& profile);

// Sculpt 与 Paint 是两套独立的笔刷语义：Sculpt 关心高度位移与 falloff，
// Paint 关心蒙版硬度/不透明度/流量。底层仍共用 BrushProfile/BrushSampler。
std::vector<BrushProfile> SculptBrushProfiles();

struct PaintBrushPreset
{
    BrushProfile profile;
    float hardness = 0.5f;
    float opacity = 0.8f;
    float flow = 0.8f;
};

bool Validate(const PaintBrushPreset& preset, std::string& error);

std::vector<PaintBrushPreset> PaintBrushProfiles();

} // namespace Editor
} // namespace VL
