#include "render/offline/offlineColorImage.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include "resource/image/textureIO.h"

namespace VL
{
namespace
{
double SrgbToLinear(double value)
{
    return value <= 0.04045 ? value / 12.92 :
        std::pow((value + 0.055) / 1.055, 2.4);
}
}

std::shared_ptr<const Offline::ColorMap> DecodeOfflineColorImage(
    const OfflineWorldAssets::ColorAsset& asset)
{
    TextureIO::LoadOptions options;
    options.transfer = asset.srgb ?
        TextureIO::LoadOptions::Transfer::SRGB :
        TextureIO::LoadOptions::Transfer::Linear;
    options.forceChannels = 4;
    // 离线材质必须复用 Texture 文件材质的单次行翻转合同；网格 UV 保持导入原值。
    options.flipY = TextureIO::LoadOptions::FlipYMode::ForceOn;
    if (asset.extension == ".png") options.formatHint = TextureIO::FileFormat::Png;
    else if (asset.extension == ".tga") options.formatHint = TextureIO::FileFormat::Tga;
    else if (asset.extension == ".hdr") options.formatHint = TextureIO::FileFormat::Hdr;
    else if (asset.extension == ".exr") options.formatHint = TextureIO::FileFormat::Exr;
    else throw std::runtime_error("Unsupported offline color format: " + asset.extension);
    auto image = TextureIO::LoadFromMemory(asset.bytes->data(), asset.bytes->size(), options);
    if (!image || image->width == 0 || image->height == 0 ||
        image->width > 4096 || image->height > 4096)
        throw std::runtime_error("Cannot decode offline color image: " + asset.sourcePath.string());
    const bool floating = image->format == HostImage::PixelFormat::RGBA32_FLOAT;
    if (!floating && image->format != HostImage::PixelFormat::RGBA8_SRGB &&
        image->format != HostImage::PixelFormat::RGBA8_UNORM)
        throw std::runtime_error("Unsupported decoded offline pixel format");
    auto result = std::make_shared<Offline::ColorMap>();
    result->width = static_cast<int>(image->width);
    result->height = static_cast<int>(image->height);
    result->repeat = asset.repeat;
    result->linear = asset.linear;
    const size_t pixelCount = static_cast<size_t>(result->width) * result->height;
    result->pixels.reserve(pixelCount);
    for (size_t index = 0; index < pixelCount; ++index)
    {
        Offline::Vec3 color;
        double channels[3]{};
        if (floating)
        {
            const auto* source = reinterpret_cast<const float*>(
                image->data.data() + index * 4 * sizeof(float));
            for (int channel = 0; channel < 3; ++channel)
                channels[channel] = source[channel];
        }
        else
        {
            const auto* source = image->data.data() + index * 4;
            for (int channel = 0; channel < 3; ++channel)
            {
                channels[channel] = source[channel] / 255.0;
                if (asset.srgb) channels[channel] = SrgbToLinear(channels[channel]);
            }
        }
        if (!std::isfinite(channels[0]) || !std::isfinite(channels[1]) ||
            !std::isfinite(channels[2]))
            throw std::runtime_error("Non-finite offline color texel: " + asset.sourcePath.string());
        color = {channels[0], channels[1], channels[2]};
        result->pixels.push_back(color);
    }
    return result;
}
} // namespace VL
