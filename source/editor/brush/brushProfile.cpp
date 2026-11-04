#include "editor/brush/brushProfile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace VL
{
namespace Editor
{
namespace
{

bool Fail(std::string& error, const char* field, const char* reason)
{
    error = std::string(field) + " " + reason;
    return false;
}

bool ReadFloat(
    const nlohmann::json& object,
    const char* field,
    float& target,
    std::string& error)
{
    const auto iterator = object.find(field);
    if (iterator == object.end())
        return true;
    if (!iterator->is_number())
        return Fail(error, field, "must be a number");
    const double value = iterator->get<double>();
    const float converted = static_cast<float>(value);
    if (!std::isfinite(value) || !std::isfinite(converted))
        return Fail(error, field, "must be finite and representable as float");
    target = converted;
    return true;
}

bool ReadString(
    const nlohmann::json& object,
    const char* field,
    std::string& target,
    std::string& error)
{
    const auto iterator = object.find(field);
    if (iterator == object.end())
        return true;
    if (!iterator->is_string())
        return Fail(error, field, "must be a string");
    target = iterator->get<std::string>();
    return true;
}

bool ReadUint32(
    const nlohmann::json& object,
    const char* field,
    uint32_t& target,
    std::string& error)
{
    const auto iterator = object.find(field);
    if (iterator == object.end())
        return true;

    uint64_t converted = 0;
    if (iterator->is_number_unsigned())
    {
        converted = iterator->get<uint64_t>();
    }
    else if (iterator->is_number_integer())
    {
        const int64_t signedValue = iterator->get<int64_t>();
        if (signedValue < 0)
            return Fail(error, field, "must be an unsigned integer");
        converted = static_cast<uint64_t>(signedValue);
    }
    else
    {
        return Fail(error, field, "must be an unsigned integer");
    }

    if (converted > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
        return Fail(error, field, "is out of range");
    target = static_cast<uint32_t>(converted);
    return true;
}

bool ReadShape(
    const nlohmann::json& object,
    BrushShape& target,
    std::string& error)
{
    const auto iterator = object.find("shape");
    if (iterator == object.end())
        return true;
    if (!iterator->is_string())
        return Fail(error, "shape", "must be a string");

    const std::string value = iterator->get<std::string>();
    if (value == "circle")
        target = BrushShape::Circle;
    else if (value == "square")
        target = BrushShape::Square;
    else if (value == "stamp")
        target = BrushShape::Stamp;
    else
        return Fail(error, "shape", "must be circle, square or stamp");
    return true;
}

bool ReadFalloff(
    const nlohmann::json& object,
    BrushFalloff& target,
    std::string& error)
{
    const auto iterator = object.find("falloff");
    if (iterator == object.end())
        return true;
    if (!iterator->is_string())
        return Fail(error, "falloff", "must be a string");

    const std::string value = iterator->get<std::string>();
    if (value == "smooth")
        target = BrushFalloff::Smooth;
    else if (value == "linear")
        target = BrushFalloff::Linear;
    else if (value == "constant")
        target = BrushFalloff::Constant;
    else if (value == "lut")
        target = BrushFalloff::Lut;
    else
        return Fail(error, "falloff", "must be smooth, linear, constant or lut");
    return true;
}

bool ReadFalloffLut(
    const nlohmann::json& object,
    std::vector<float>& target,
    std::string& error)
{
    const auto iterator = object.find("falloffLut");
    if (iterator == object.end())
        return true;
    if (!iterator->is_array())
        return Fail(error, "falloffLut", "must be an array of numbers");

    std::vector<float> samples;
    samples.reserve(iterator->size());
    for (const nlohmann::json& item : *iterator)
    {
        if (!item.is_number())
            return Fail(error, "falloffLut", "must contain only numbers");
        const double value = item.get<double>();
        const float converted = static_cast<float>(value);
        if (!std::isfinite(value) || !std::isfinite(converted))
            return Fail(error, "falloffLut", "must contain finite numbers");
        samples.push_back(converted);
    }
    target = std::move(samples);
    return true;
}

const char* ShapeName(BrushShape shape)
{
    switch (shape)
    {
    case BrushShape::Circle:
        return "circle";
    case BrushShape::Square:
        return "square";
    case BrushShape::Stamp:
        return "stamp";
    }
    return "circle";
}

const char* FalloffName(BrushFalloff falloff)
{
    switch (falloff)
    {
    case BrushFalloff::Smooth:
        return "smooth";
    case BrushFalloff::Linear:
        return "linear";
    case BrushFalloff::Constant:
        return "constant";
    case BrushFalloff::Lut:
        return "lut";
    }
    return "smooth";
}

} // namespace

bool Validate(const BrushProfile& profile, std::string& error)
{
    error.clear();
    if (!std::isfinite(profile.radiusCm))
        return Fail(error, "radiusCm", "must be finite");
    if (profile.radiusCm <= 0.0f)
        return Fail(error, "radiusCm", "must be greater than zero");

    if (!std::isfinite(profile.strength))
        return Fail(error, "strength", "must be finite");
    if (profile.strength < 0.0f || profile.strength > 1.0f)
        return Fail(error, "strength", "must be in [0,1]");

    if (!std::isfinite(profile.spacing))
        return Fail(error, "spacing", "must be finite");
    if (profile.spacing <= 0.0f || profile.spacing > 1.0f)
        return Fail(error, "spacing", "must be in (0,1]");

    if (!std::isfinite(profile.jitter))
        return Fail(error, "jitter", "must be finite");
    if (profile.jitter < 0.0f || profile.jitter > 1.0f)
        return Fail(error, "jitter", "must be in [0,1]");

    if (!std::isfinite(profile.rotationDeg))
        return Fail(error, "rotationDeg", "must be finite");

    if (!std::isfinite(profile.noise))
        return Fail(error, "noise", "must be finite");
    if (profile.noise < 0.0f || profile.noise > 1.0f)
        return Fail(error, "noise", "must be in [0,1]");

    if (!std::isfinite(profile.pressureSizeInfluence))
        return Fail(error, "pressureSizeInfluence", "must be finite");
    if (profile.pressureSizeInfluence < 0.0f ||
        profile.pressureSizeInfluence > 1.0f)
        return Fail(error, "pressureSizeInfluence", "must be in [0,1]");

    if (!std::isfinite(profile.pressureStrengthInfluence))
        return Fail(error, "pressureStrengthInfluence", "must be finite");
    if (profile.pressureStrengthInfluence < 0.0f ||
        profile.pressureStrengthInfluence > 1.0f)
        return Fail(error, "pressureStrengthInfluence", "must be in [0,1]");

    if (profile.shape != BrushShape::Circle &&
        profile.shape != BrushShape::Square &&
        profile.shape != BrushShape::Stamp)
        return Fail(error, "shape", "must be circle, square or stamp");
    if (profile.falloff != BrushFalloff::Smooth &&
        profile.falloff != BrushFalloff::Linear &&
        profile.falloff != BrushFalloff::Constant &&
        profile.falloff != BrushFalloff::Lut)
        return Fail(error, "falloff", "must be smooth, linear, constant or lut");

    if (profile.falloff == BrushFalloff::Lut)
    {
        if (profile.falloffLut.size() < 2)
            return Fail(error, "falloffLut", "must contain at least two samples");
        for (float sample : profile.falloffLut)
        {
            if (!std::isfinite(sample) || sample < 0.0f || sample > 1.0f)
                return Fail(
                    error,
                    "falloffLut",
                    "samples must be finite and in [0,1]");
        }
    }

    if (profile.shape == BrushShape::Stamp && profile.stampPath.empty())
        return Fail(error, "stampPath", "must not be empty for stamp brushes");
    return true;
}

nlohmann::json ToJson(const BrushProfile& profile)
{
    nlohmann::json value;
    value["name"] = profile.name;
    value["category"] = profile.category;
    value["shape"] = ShapeName(profile.shape);
    value["falloff"] = FalloffName(profile.falloff);
    value["radiusCm"] = profile.radiusCm;
    value["strength"] = profile.strength;
    value["spacing"] = profile.spacing;
    value["jitter"] = profile.jitter;
    value["rotationDeg"] = profile.rotationDeg;
    value["noise"] = profile.noise;
    value["noiseSeed"] = profile.noiseSeed;
    value["pressureSizeInfluence"] = profile.pressureSizeInfluence;
    value["pressureStrengthInfluence"] = profile.pressureStrengthInfluence;
    value["falloffLut"] = profile.falloffLut;
    value["stampPath"] = profile.stampPath;
    return value;
}

bool FromJson(
    const nlohmann::json& value,
    BrushProfile& profile,
    std::string& error)
{
    error.clear();
    if (!value.is_object())
    {
        error = "brush profile must be a JSON object";
        return false;
    }

    BrushProfile parsed;
    if (!ReadString(value, "name", parsed.name, error) ||
        !ReadString(value, "category", parsed.category, error) ||
        !ReadShape(value, parsed.shape, error) ||
        !ReadFalloff(value, parsed.falloff, error) ||
        !ReadFloat(value, "radiusCm", parsed.radiusCm, error) ||
        !ReadFloat(value, "strength", parsed.strength, error) ||
        !ReadFloat(value, "spacing", parsed.spacing, error) ||
        !ReadFloat(value, "jitter", parsed.jitter, error) ||
        !ReadFloat(value, "rotationDeg", parsed.rotationDeg, error) ||
        !ReadFloat(value, "noise", parsed.noise, error) ||
        !ReadUint32(value, "noiseSeed", parsed.noiseSeed, error) ||
        !ReadFloat(
            value,
            "pressureSizeInfluence",
            parsed.pressureSizeInfluence,
            error) ||
        !ReadFloat(
            value,
            "pressureStrengthInfluence",
            parsed.pressureStrengthInfluence,
            error) ||
        !ReadFalloffLut(value, parsed.falloffLut, error) ||
        !ReadString(value, "stampPath", parsed.stampPath, error))
    {
        return false;
    }

    if (!Validate(parsed, error))
        return false;

    profile = std::move(parsed);
    return true;
}

std::string BrushProfileId(const BrushProfile& profile)
{
    std::string id;
    bool pendingSeparator = false;
    for (const unsigned char character : profile.name)
    {
        if (std::isalnum(character))
        {
            if (pendingSeparator && !id.empty())
                id += '-';
            id += static_cast<char>(std::tolower(character));
            pendingSeparator = false;
        }
        else if (!id.empty())
        {
            pendingSeparator = true;
        }
    }
    return id;
}

std::vector<BrushProfile> SculptBrushProfiles()
{
    BrushProfile softRound;
    softRound.name = "Soft Round";
    softRound.category = "Sculpt";
    softRound.shape = BrushShape::Circle;
    softRound.falloff = BrushFalloff::Smooth;
    softRound.radiusCm = 500.0f;
    softRound.strength = 0.1f;

    BrushProfile hardRound;
    hardRound.name = "Hard Round";
    hardRound.category = "Sculpt";
    hardRound.shape = BrushShape::Circle;
    hardRound.falloff = BrushFalloff::Constant;
    hardRound.radiusCm = 300.0f;
    hardRound.strength = 0.2f;

    BrushProfile wideSoft;
    wideSoft.name = "Wide Soft";
    wideSoft.category = "Sculpt";
    wideSoft.shape = BrushShape::Circle;
    wideSoft.falloff = BrushFalloff::Smooth;
    wideSoft.radiusCm = 1000.0f;
    wideSoft.strength = 0.08f;

    BrushProfile square;
    square.name = "Square";
    square.category = "Sculpt";
    square.shape = BrushShape::Square;
    square.falloff = BrushFalloff::Linear;
    square.radiusCm = 400.0f;
    square.strength = 0.15f;

    return {softRound, hardRound, wideSoft, square};
}

bool Validate(const PaintBrushPreset& preset, std::string& error)
{
    if (!Validate(preset.profile, error))
        return false;
    if (!std::isfinite(preset.hardness) ||
        preset.hardness < 0.0f || preset.hardness > 1.0f)
        return Fail(error, "hardness", "must be in [0,1]");
    if (!std::isfinite(preset.opacity) ||
        preset.opacity < 0.0f || preset.opacity > 1.0f)
        return Fail(error, "opacity", "must be in [0,1]");
    if (!std::isfinite(preset.flow) ||
        preset.flow < 0.0f || preset.flow > 1.0f)
        return Fail(error, "flow", "must be in [0,1]");
    return true;
}

std::vector<PaintBrushPreset> PaintBrushProfiles()
{
    PaintBrushPreset softMask;
    softMask.profile.name = "Soft Mask";
    softMask.profile.category = "Paint";
    softMask.profile.shape = BrushShape::Circle;
    softMask.profile.falloff = BrushFalloff::Smooth;
    softMask.profile.radiusCm = 500.0f;
    softMask.profile.strength = 1.0f;
    softMask.hardness = 0.5f;
    softMask.opacity = 0.8f;
    softMask.flow = 0.8f;

    PaintBrushPreset hardMask;
    hardMask.profile.name = "Hard Mask";
    hardMask.profile.category = "Paint";
    hardMask.profile.shape = BrushShape::Circle;
    hardMask.profile.falloff = BrushFalloff::Constant;
    hardMask.profile.radiusCm = 300.0f;
    hardMask.profile.strength = 1.0f;
    hardMask.hardness = 1.0f;
    hardMask.opacity = 1.0f;
    hardMask.flow = 1.0f;

    PaintBrushPreset spray;
    spray.profile.name = "Spray";
    spray.profile.category = "Paint";
    spray.profile.shape = BrushShape::Circle;
    spray.profile.falloff = BrushFalloff::Smooth;
    spray.profile.radiusCm = 600.0f;
    spray.profile.strength = 1.0f;
    // Paint 运行时按 texel 用稳定噪声调制 flow；预设只保存噪声幅度与 seed。
    spray.profile.noise = 0.35f;
    spray.profile.spacing = 0.15f;
    spray.hardness = 0.4f;
    spray.opacity = 0.6f;
    spray.flow = 0.7f;

    PaintBrushPreset squareMask;
    squareMask.profile.name = "Square Mask";
    squareMask.profile.category = "Paint";
    squareMask.profile.shape = BrushShape::Square;
    squareMask.profile.falloff = BrushFalloff::Linear;
    squareMask.profile.radiusCm = 400.0f;
    squareMask.profile.strength = 1.0f;
    squareMask.hardness = 0.8f;
    squareMask.opacity = 1.0f;
    squareMask.flow = 0.9f;

    return {softMask, hardMask, spray, squareMask};
}

} // namespace Editor
} // namespace VL
