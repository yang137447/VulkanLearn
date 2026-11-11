#include "editor/terrain/terrainLayerPreview.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include "commonFunction.h"
#include "resource/image/textureIO.h"
#include "shader/build/atomicFile.h"
#include "shader/build/contentHash.h"

namespace VL::Editor
{
namespace
{

constexpr float Pi = 3.14159265358979323846f;
constexpr std::array<const char*, 3> TextureArrayFields = {
    "layerAlbedo", "layerNormal", "layerPbr"};
constexpr std::array<const char*, 3> PreviewChannelNames = {
    "albedo", "normal", "pbr"};
constexpr size_t PreviewTextureCount = 3;
constexpr size_t PreviewStampCount = 4;

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string PreviewKindName(TerrainLayerPreviewKind kind)
{
    return kind == TerrainLayerPreviewKind::Sphere ? "sphere" : "plane";
}

std::string PreviewChannelName(size_t channel)
{
    return channel < PreviewChannelNames.size() ?
        PreviewChannelNames[channel] : "unknown";
}

std::filesystem::path ResolveResourceFile(
    const std::filesystem::path& resourceRoot,
    const std::string& value)
{
    if (resourceRoot.empty() || value.empty() ||
        value.find('\0') != std::string::npos)
    {
        return {};
    }

    std::string normalized = value;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    // 资源字段只接受资源根相对路径；盘符/绝对路径允许绕过 root，直接拒绝。
    if (normalized.find(':') != std::string::npos)
        return {};
    const std::filesystem::path relative = std::filesystem::u8path(normalized);
    if (relative.empty() || relative.is_absolute() || relative.has_root_path())
        return {};
    for (const std::filesystem::path& part : relative)
        if (part == "..")
            return {};

    std::error_code error;
    const std::filesystem::path absolute =
        std::filesystem::absolute(resourceRoot / relative, error);
    if (error)
        return {};
    const std::filesystem::path physical =
        std::filesystem::weakly_canonical(absolute, error);
    if (error || !std::filesystem::is_regular_file(physical, error) || error)
        return {};

    const std::filesystem::path absoluteRoot =
        std::filesystem::weakly_canonical(
            std::filesystem::absolute(resourceRoot, error), error);
    if (error)
        return {};
    const std::filesystem::path fromRoot = physical.lexically_relative(absoluteRoot);
    if (fromRoot.empty() || *fromRoot.begin() == "..")
        return {};
    return physical;
}

bool ReadJsonFile(const std::filesystem::path& path, nlohmann::json& output)
{
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > 16u * 1024u * 1024u)
        return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
        return false;
    try
    {
        stream >> output;
    }
    catch (const std::exception&)
    {
        return false;
    }
    return output.is_object();
}

bool FileMetadata(
    const std::filesystem::path& path,
    uintmax_t& size,
    std::filesystem::file_time_type& writeTime)
{
    std::error_code error;
    size = std::filesystem::file_size(path, error);
    if (error)
        return false;
    writeTime = std::filesystem::last_write_time(path, error);
    return !error;
}

bool UsesPbrMaterial(const nlohmann::json& materialInstance)
{
    if (!materialInstance.contains("material") ||
        !materialInstance["material"].is_string())
    {
        return false;
    }
    const std::string filename = LowerAscii(
        std::filesystem::path(
            materialInstance["material"].get<std::string>()).filename().string());
    return filename == "m_pbr.json" || filename.rfind("m_pbr.", 0) == 0;
}

struct LoadedTextureArraySource
{
    std::filesystem::path path;
    std::filesystem::file_time_type writeTime{};
    uintmax_t size = 0;
    std::vector<std::filesystem::path> sources;
    bool displacementDeclared = false;
    std::vector<std::filesystem::path> displacementSources;
};

struct LoadedMaterialSourceSet
{
    std::filesystem::path materialPath;
    std::filesystem::file_time_type materialWriteTime{};
    uintmax_t materialSize = 0;
    std::array<LoadedTextureArraySource, 3> arrays;
};

bool LoadTextureArraySource(
    const std::filesystem::path& resourceRoot,
    const nlohmann::json& textures,
    const char* field,
    LoadedTextureArraySource& output)
{
    output = {};
    if (!textures.contains(field) || !textures[field].is_string())
        return false;

    const std::string textureAssetPath = textures[field].get<std::string>();
    output.path = ResolveResourceFile(resourceRoot, textureAssetPath);
    if (output.path.empty() ||
        !FileMetadata(output.path, output.size, output.writeTime))
    {
        return false;
    }

    nlohmann::json textureAsset;
    if (!ReadJsonFile(output.path, textureAsset) ||
        !textureAsset.contains("type") ||
        !textureAsset["type"].is_string() ||
        textureAsset["type"].get<std::string>() != "textureArray" ||
        !textureAsset.contains("sources") ||
        !textureAsset["sources"].is_array())
    {
        output = {};
        return false;
    }

    for (const nlohmann::json& source : textureAsset["sources"])
    {
        // 缺失的 slice 保留空 path，不能在数组里删除，否则后续层会错位到下一张图。
        if (!source.is_string())
            output.sources.emplace_back();
        else
            output.sources.push_back(
                ResolveResourceFile(resourceRoot, source.get<std::string>()));
    }

    // 16-bit 位移源是层 source-set 的编辑器侧扩展：与 sources 同序、同 slice。
    // 缺少字段只表示旧组仍走 pbr.a/亮度回退；字段存在但格式错误则整组拒绝。
    if (textureAsset.contains("displacementSources"))
    {
        if (!textureAsset["displacementSources"].is_array())
        {
            output = {};
            return false;
        }
        output.displacementDeclared = true;
        for (const nlohmann::json& source : textureAsset["displacementSources"])
        {
            if (!source.is_string())
                output.displacementSources.emplace_back();
            else
                output.displacementSources.push_back(
                    ResolveResourceFile(resourceRoot, source.get<std::string>()));
        }
    }
    return true;
}

LoadedMaterialSourceSet LoadMaterialSourceSet(
    const std::filesystem::path& materialPath,
    const std::filesystem::path& resourceRoot)
{
    LoadedMaterialSourceSet result;
    result.materialPath = materialPath;
    if (!FileMetadata(materialPath, result.materialSize, result.materialWriteTime))
        return {};

    nlohmann::json materialInstance;
    if (!ReadJsonFile(materialPath, materialInstance))
        return {};
    if (materialInstance.contains("type") &&
        (!materialInstance["type"].is_string() ||
         materialInstance["type"].get<std::string>() != "materialInstance"))
    {
        return {};
    }
    if (UsesPbrMaterial(materialInstance))
        return {};
    if (!materialInstance.contains("textures") ||
        !materialInstance["textures"].is_object())
    {
        return {};
    }

    const nlohmann::json& textures = materialInstance["textures"];
    // albedo 决定这个 MI 是否是层材质；normal/pbr 可以先缺失，调用方会退回
    // avgColor 色块，而不是用 albedo 单独伪造 PBR 预览。
    if (!LoadTextureArraySource(
            resourceRoot, textures, TextureArrayFields[0], result.arrays[0]))
    {
        return {};
    }
    LoadTextureArraySource(
        resourceRoot, textures, TextureArrayFields[1], result.arrays[1]);
    LoadTextureArraySource(
        resourceRoot, textures, TextureArrayFields[2], result.arrays[2]);
    return result;
}

bool IsSupportedTexture(const HostImage& image)
{
    if (image.width == 0 || image.height == 0)
        return false;
    if (image.format != HostImage::PixelFormat::RGBA8_UNORM &&
        image.format != HostImage::PixelFormat::RGBA8_SRGB)
    {
        return false;
    }
    const size_t rowStride = image.rowStrideBytes > 0 ?
        image.rowStrideBytes : static_cast<size_t>(image.width) * 4;
    return image.data.size() >= rowStride * image.height;
}

bool HasNonOpaqueAlpha(const HostImage& image)
{
    const size_t rowStride = image.rowStrideBytes > 0 ?
        image.rowStrideBytes : static_cast<size_t>(image.width) * 4;
    for (uint32_t y = 0; y < image.height; ++y)
    {
        const size_t rowOffset = static_cast<size_t>(y) * rowStride;
        for (uint32_t x = 0; x < image.width; ++x)
            if (image.data[rowOffset + static_cast<size_t>(x) * 4 + 3] < 255)
                return true;
    }
    return false;
}

float SrgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

uint8_t LinearToSrgbByte(float value)
{
    const float encoded = value <= 0.0031308f ? value * 12.92f
        : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(
        std::lround(std::clamp(encoded, 0.0f, 1.0f) * 255.0f));
}

float Luminance(const std::array<float, 3>& color)
{
    return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
}

struct PreviewVec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

PreviewVec3 Add(const PreviewVec3& left, const PreviewVec3& right)
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

PreviewVec3 Subtract(const PreviewVec3& left, const PreviewVec3& right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

PreviewVec3 Scale(const PreviewVec3& value, float scalar)
{
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

PreviewVec3 Multiply(const PreviewVec3& left, const PreviewVec3& right)
{
    return {left.x * right.x, left.y * right.y, left.z * right.z};
}

float Dot(const PreviewVec3& left, const PreviewVec3& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

PreviewVec3 Cross(const PreviewVec3& left, const PreviewVec3& right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x};
}

float Length(const PreviewVec3& value)
{
    return std::sqrt(Dot(value, value));
}

PreviewVec3 Normalize(
    const PreviewVec3& value,
    const PreviewVec3& fallback = {0.0f, 0.0f, 1.0f})
{
    const float length = Length(value);
    if (!std::isfinite(length) || length <= 1.0e-8f)
        return fallback;
    return Scale(value, 1.0f / length);
}

PreviewVec3 Mix(const PreviewVec3& low, const PreviewVec3& high, float amount)
{
    return Add(Scale(low, 1.0f - amount), Scale(high, amount));
}

struct PreviewTexel
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

class TextureSampler
{
public:
    TextureSampler(const HostImage& image, bool decodeSrgb)
        : width(image.width),
          height(image.height),
          stride(image.rowStrideBytes > 0 ?
              image.rowStrideBytes : static_cast<size_t>(image.width) * 4),
          pixels(image.data.data()),
          srgb(decodeSrgb)
    {
        for (size_t index = 0; index < 256; ++index)
            linearTable[index] = SrgbToLinear(static_cast<float>(index) / 255.0f);
    }

    PreviewTexel Sample(float u, float v) const
    {
        // repeat 采样：预览平面与运行时 layerAlbedo 使用同一 wrap 语义。
        u -= std::floor(u);
        v -= std::floor(v);
        const float x = u * static_cast<float>(width) - 0.5f;
        const float y = v * static_cast<float>(height) - 0.5f;
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const float tx = x - static_cast<float>(x0);
        const float ty = y - static_cast<float>(y0);
        const PreviewTexel c00 = Texel(x0, y0);
        const PreviewTexel c10 = Texel(x0 + 1, y0);
        const PreviewTexel c01 = Texel(x0, y0 + 1);
        const PreviewTexel c11 = Texel(x0 + 1, y0 + 1);
        const PreviewTexel top = Lerp(c00, c10, tx);
        const PreviewTexel bottom = Lerp(c01, c11, tx);
        return Lerp(top, bottom, ty);
    }

private:
    static PreviewTexel Lerp(
        const PreviewTexel& left,
        const PreviewTexel& right,
        float amount)
    {
        return {
            left.r + (right.r - left.r) * amount,
            left.g + (right.g - left.g) * amount,
            left.b + (right.b - left.b) * amount,
            left.a + (right.a - left.a) * amount};
    }

    float DecodeChannel(uint8_t value) const
    {
        return srgb ? linearTable[value] : static_cast<float>(value) / 255.0f;
    }

    PreviewTexel Texel(int x, int y) const
    {
        const int wrappedX = ((x % static_cast<int>(width)) +
            static_cast<int>(width)) % static_cast<int>(width);
        const int wrappedY = ((y % static_cast<int>(height)) +
            static_cast<int>(height)) % static_cast<int>(height);
        const size_t offset = static_cast<size_t>(wrappedY) * stride +
            static_cast<size_t>(wrappedX) * 4;
        return {
            DecodeChannel(pixels[offset + 0]),
            DecodeChannel(pixels[offset + 1]),
            DecodeChannel(pixels[offset + 2]),
            static_cast<float>(pixels[offset + 3]) / 255.0f};
    }

    uint32_t width = 0;
    uint32_t height = 0;
    size_t stride = 0;
    const uint8_t* pixels = nullptr;
    bool srgb = false;
    float linearTable[256] = {};
};

struct LayerPreviewSample
{
    PreviewVec3 baseColor;
    PreviewVec3 tangentNormal = {0.0f, 0.0f, 1.0f};
    float roughness = 1.0f;
    float metallic = 0.0f;
    float ambientOcclusion = 1.0f;
    float height = 0.5f;
};

class LayerMaterialSampler
{
public:
    LayerMaterialSampler(
        const HostImage& albedo,
        const HostImage& normal,
        const HostImage& pbr,
        const TerrainLayerHeightField* displacement,
        float textureRepeat)
        : albedoSampler(albedo, true),
          normalSampler(normal, false),
          pbrSampler(pbr, false),
          displacementField(displacement),
          repeat(std::clamp(textureRepeat, 0.05f, 32.0f)),
          pbrAlphaCarriesHeight(HasNonOpaqueAlpha(pbr)),
          heightWidth(displacement != nullptr && displacement->IsValid() ?
              displacement->width : albedo.width),
          heightHeight(displacement != nullptr && displacement->IsValid() ?
              displacement->height : albedo.height),
          texelStepU(1.0f / static_cast<float>(heightWidth * repeat)),
          texelStepV(1.0f / static_cast<float>(heightHeight * repeat))
    {
    }

    LayerPreviewSample Sample(float u, float v) const
    {
        const PreviewTexel albedo = albedoSampler.Sample(u * repeat, v * repeat);
        const PreviewTexel normal = normalSampler.Sample(u * repeat, v * repeat);
        const PreviewTexel pbr = pbrSampler.Sample(u * repeat, v * repeat);
        LayerPreviewSample result;
        result.baseColor = {albedo.r, albedo.g, albedo.b};
        result.tangentNormal = Normalize({
            normal.r * 2.0f - 1.0f,
            normal.g * 2.0f - 1.0f,
            normal.b * 2.0f - 1.0f});
        result.roughness = std::clamp(pbr.r, 0.04f, 1.0f);
        result.metallic = std::clamp(pbr.g, 0.0f, 1.0f);
        result.ambientOcclusion = std::clamp(pbr.b, 0.0f, 1.0f);
        result.height = HeightAt(u, v, albedo, pbr);
        return result;
    }

    float HeightAt(float u, float v) const
    {
        return HeightAt(
            u, v,
            albedoSampler.Sample(u * repeat, v * repeat),
            pbrSampler.Sample(u * repeat, v * repeat));
    }

    float TexelStepU() const { return texelStepU; }
    float TexelStepV() const { return texelStepV; }

private:
    float HeightAt(
        float u,
        float v,
        const PreviewTexel& albedo,
        const PreviewTexel& pbr) const
    {
        // 16-bit displacementSources 是主路径；只有旧组缺少该字段时才走
        // pbr.a 或 albedo 亮度回退。height 只参与预览轮廓/法线扰动。
        if (displacementField != nullptr && displacementField->IsValid())
            return displacementField->SampleNormalized(u * repeat, v * repeat);
        return HeightFrom(albedo, pbr);
    }

    float HeightFrom(
        const PreviewTexel& albedo,
        const PreviewTexel& pbr) const
    {
        // 旧组回退：pbr.a 只有非恒定 255 时才被视为可用高度；纯 RGB pbr
        // 退回 albedo 亮度。这不是运行地形高度，也不改变运行时数组。
        if (pbrAlphaCarriesHeight)
            return std::clamp(pbr.a, 0.0f, 1.0f);
        const std::array<float, 3> albedoColor = {albedo.r, albedo.g, albedo.b};
        return std::clamp(Luminance(albedoColor), 0.0f, 1.0f);
    }

    TextureSampler albedoSampler;
    TextureSampler normalSampler;
    TextureSampler pbrSampler;
    const TerrainLayerHeightField* displacementField = nullptr;
    float repeat = 1.0f;
    bool pbrAlphaCarriesHeight = false;
    uint32_t heightWidth = 0;
    uint32_t heightHeight = 0;
    float texelStepU = 1.0f;
    float texelStepV = 1.0f;
};

PreviewVec3 PerturbNormal(
    const LayerMaterialSampler& sampler,
    const LayerPreviewSample& sample,
    float u,
    float v,
    const PreviewVec3& meshNormal,
    const PreviewVec3& tangent,
    const PreviewVec3& bitangent,
    const TerrainLayerPreviewSettings& settings)
{
    const float stepU = sampler.TexelStepU();
    const float stepV = sampler.TexelStepV();
    const float heightU = sampler.HeightAt(u + stepU, v) -
        sampler.HeightAt(u - stepU, v);
    const float heightV = sampler.HeightAt(u, v + stepV) -
        sampler.HeightAt(u, v - stepV);

    PreviewVec3 tangentNormal = sample.tangentNormal;
    // 位移代理的高度梯度与 normal map 叠加；v 在纹理中向下增长，而
    // bitangent 取 N×T（向上），所以这里的 y 符号与 x 相反。
    tangentNormal.x -= heightU * settings.displacementNormalStrength;
    tangentNormal.y += heightV * settings.displacementNormalStrength;
    tangentNormal.z = std::max(tangentNormal.z, 0.05f);
    tangentNormal = Normalize(tangentNormal);

    const PreviewVec3 worldNormal = Add(
        Add(Scale(tangent, tangentNormal.x), Scale(bitangent, tangentNormal.y)),
        Scale(meshNormal, tangentNormal.z));
    return Normalize(worldNormal, meshNormal);
}

PreviewVec3 ToneMapAces(const PreviewVec3& color)
{
    // CPU 预览只求稳定观感；使用 Narkowicz ACES 拟合并在最后钳制到显示域。
    const float red = std::max(0.0f, color.x);
    const float green = std::max(0.0f, color.y);
    const float blue = std::max(0.0f, color.z);
    const float mappedRed = (red * (2.51f * red + 0.03f)) /
        (red * (2.43f * red + 0.59f) + 0.14f);
    const float mappedGreen = (green * (2.51f * green + 0.03f)) /
        (green * (2.43f * green + 0.59f) + 0.14f);
    const float mappedBlue = (blue * (2.51f * blue + 0.03f)) /
        (blue * (2.43f * blue + 0.59f) + 0.14f);
    return {
        std::clamp(mappedRed, 0.0f, 1.0f),
        std::clamp(mappedGreen, 0.0f, 1.0f),
        std::clamp(mappedBlue, 0.0f, 1.0f)};
}

PreviewVec3 ShadePbr(
    const LayerPreviewSample& sample,
    const PreviewVec3& normal,
    const PreviewVec3& view)
{
    const PreviewVec3 light = Normalize({-0.42f, 0.62f, 0.66f});
    const PreviewVec3 lightColor = {3.4f, 3.2f, 2.95f};
    const float nDotL = std::max(0.0f, Dot(normal, light));
    const float nDotV = std::max(0.001f, Dot(normal, view));
    const PreviewVec3 halfVector = Normalize(Add(light, view));
    const float nDotH = std::max(0.0f, Dot(normal, halfVector));
    const float vDotH = std::max(0.0f, Dot(view, halfVector));
    const float roughness = std::max(0.04f, sample.roughness);
    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float distributionDenominator =
        nDotH * nDotH * (alphaSquared - 1.0f) + 1.0f;
    const float distribution = alphaSquared / std::max(
        Pi * distributionDenominator * distributionDenominator, 1.0e-6f);

    const float geometryK = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
    const float geometryView = nDotV /
        (nDotV * (1.0f - geometryK) + geometryK);
    const float geometryLight = nDotL /
        (nDotL * (1.0f - geometryK) + geometryK);
    const float geometry = geometryView * geometryLight;

    const PreviewVec3 dielectricF0 = {0.04f, 0.04f, 0.04f};
    const PreviewVec3 f0 = Mix(dielectricF0, sample.baseColor, sample.metallic);
    const float fresnelScalar = std::pow(1.0f - vDotH, 5.0f);
    const PreviewVec3 fresnel = Add(
        f0, Scale(Subtract({1.0f, 1.0f, 1.0f}, f0), fresnelScalar));

    const float specularScale = distribution * geometry /
        std::max(4.0f * nDotV * nDotL, 1.0e-5f);
    const PreviewVec3 specular = Scale(
        Multiply(fresnel, lightColor), specularScale * nDotL);

    const float diffuseScale = nDotL / Pi;
    const PreviewVec3 diffuse = Scale(
        Multiply(
            Multiply(
                sample.baseColor,
                Subtract({1.0f, 1.0f, 1.0f}, fresnel)),
            lightColor),
        diffuseScale * (1.0f - sample.metallic));

    // 简单半球环境项：AO 直接压暗 ambient，金属保留少量 F0 高光，
    // 让 rough rock/sand 和 grass 纤维都有可读的粗糙度响应。
    const float skyAmount = 0.5f + 0.5f * std::max(0.0f, normal.y);
    const PreviewVec3 ambientDiffuse = Scale(
        sample.baseColor,
        sample.ambientOcclusion * (1.0f - sample.metallic) *
            (0.16f + 0.08f * skyAmount));
    const PreviewVec3 ambientSpecular = Scale(
        f0,
        sample.ambientOcclusion * (0.035f + 0.025f * skyAmount));
    return ToneMapAces(Add(
        Add(Add(diffuse, specular), ambientDiffuse),
        ambientSpecular));
}

HostImage MakePreviewImage(uint32_t width, uint32_t height)
{
    HostImage image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.format = HostImage::PixelFormat::RGBA8_SRGB;
    image.semantic = HostImage::TextureSemantic::Color;
    image.rowStrideBytes = width * 4;
    image.data.resize(static_cast<size_t>(image.rowStrideBytes) * height);
    return image;
}

void WritePixel(
    HostImage& image,
    uint32_t x,
    uint32_t y,
    const PreviewVec3& color,
    uint8_t alpha)
{
    const size_t offset = static_cast<size_t>(y) * image.rowStrideBytes +
        static_cast<size_t>(x) * 4;
    image.data[offset + 0] = LinearToSrgbByte(color.x);
    image.data[offset + 1] = LinearToSrgbByte(color.y);
    image.data[offset + 2] = LinearToSrgbByte(color.z);
    image.data[offset + 3] = alpha;
}

uint32_t FloatBits(float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void AddSourceStamp(
    CanonicalFieldHasher& hasher,
    const char* prefix,
    const TerrainLayerPreviewSourceStamp& stamp)
{
    hasher.AddString(std::string(prefix) + "Path", stamp.path.generic_string());
    hasher.AddUInt64(std::string(prefix) + "Size",
        static_cast<uint64_t>(stamp.size));
    hasher.AddUInt64(std::string(prefix) + "WriteTime",
        static_cast<uint64_t>(stamp.writeTime.time_since_epoch().count()));
}

float HeightTexel(const TerrainLayerHeightField& field, int x, int y)
{
    const int wrappedX = ((x % static_cast<int>(field.width)) +
        static_cast<int>(field.width)) % static_cast<int>(field.width);
    const int wrappedY = ((y % static_cast<int>(field.height)) +
        static_cast<int>(field.height)) % static_cast<int>(field.height);
    const size_t offset = static_cast<size_t>(wrappedY) * field.width +
        static_cast<size_t>(wrappedX);
    return static_cast<float>(field.samples[offset]) / 65535.0f;
}

} // namespace

float TerrainLayerHeightField::SampleNormalized(float u, float v) const
{
    if (!IsValid())
        return 0.5f;

    u -= std::floor(u);
    v -= std::floor(v);
    const float x = u * static_cast<float>(width) - 0.5f;
    const float y = v * static_cast<float>(height) - 0.5f;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const float top = HeightTexel(*this, x0, y0) * (1.0f - tx) +
        HeightTexel(*this, x0 + 1, y0) * tx;
    const float bottom = HeightTexel(*this, x0, y0 + 1) * (1.0f - tx) +
        HeightTexel(*this, x0 + 1, y0 + 1) * tx;
    return top * (1.0f - ty) + bottom * ty;
}

bool LoadTerrainLayerDisplacement(
    const std::filesystem::path& path,
    TerrainLayerHeightField& output)
{
    output = {};
    if (path.empty())
        return false;

    int width = 0;
    int height = 0;
    int channels = 0;
    const std::string pathString = path.string();
    // 明确走 16-bit 解码：stbi_load（8-bit）会在这里静默截断高度细节。
    stbi_us* pixels = stbi_load_16(
        pathString.c_str(), &width, &height, &channels, 1);
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        if (pixels != nullptr)
            stbi_image_free(pixels);
        return false;
    }

    output.width = static_cast<uint32_t>(width);
    output.height = static_cast<uint32_t>(height);
    const size_t sampleCount =
        static_cast<size_t>(output.width) * output.height;
    output.samples.assign(pixels, pixels + sampleCount);
    stbi_image_free(pixels);
    return true;
}

size_t TerrainLayerSourceSlice(const TerrainAssetDesc& desc, size_t layerIndex)
{
    if (layerIndex >= desc.layers.size())
        return std::numeric_limits<size_t>::max();
    const uint32_t sourceSet = desc.layers[layerIndex].sourceSet;
    size_t slice = 0;
    for (size_t index = 0; index < layerIndex; ++index)
        if (desc.layers[index].sourceSet == sourceSet)
            ++slice;
    return slice;
}

TerrainLayerPreviewSources ResolveTerrainLayerPreviewSources(
    const TerrainAssetDesc& desc,
    size_t layerIndex,
    const std::filesystem::path& resourceRoot)
{
    TerrainLayerPreviewSources result;
    if (layerIndex >= desc.layers.size() || desc.materialInstancePath.empty())
        return result;
    const std::filesystem::path materialPath =
        ResolveResourceFile(resourceRoot, desc.materialInstancePath);
    if (materialPath.empty())
        return result;
    const LoadedMaterialSourceSet sourceSet =
        LoadMaterialSourceSet(materialPath, resourceRoot);
    const size_t slice = TerrainLayerSourceSlice(desc, layerIndex);
    if (slice == std::numeric_limits<size_t>::max())
        return result;

    if (slice < sourceSet.arrays[0].sources.size())
        result.albedoPath = sourceSet.arrays[0].sources[slice];
    if (slice < sourceSet.arrays[1].sources.size())
        result.normalPath = sourceSet.arrays[1].sources[slice];
    if (slice < sourceSet.arrays[2].sources.size())
        result.pbrPath = sourceSet.arrays[2].sources[slice];
    result.displacementDeclared = sourceSet.arrays[0].displacementDeclared;
    if (slice < sourceSet.arrays[0].displacementSources.size())
        result.displacementPath = sourceSet.arrays[0].displacementSources[slice];
    return result;
}

std::filesystem::path ResolveTerrainLayerPreviewSource(
    const TerrainAssetDesc& desc,
    size_t layerIndex,
    const std::filesystem::path& resourceRoot)
{
    return ResolveTerrainLayerPreviewSources(desc, layerIndex, resourceRoot).albedoPath;
}

bool BuildTerrainLayerSpherePreview(
    const HostImage& albedo,
    const HostImage& normal,
    const HostImage& pbr,
    const TerrainLayerHeightField* displacement,
    const TerrainLayerPreviewSettings& settings,
    HostImage& output)
{
    if (!IsSupportedTexture(albedo) ||
        !IsSupportedTexture(normal) ||
        !IsSupportedTexture(pbr) ||
        (displacement != nullptr && !displacement->IsValid()) ||
        settings.sphereSize == 0)
    {
        return false;
    }

    output = MakePreviewImage(settings.sphereSize, settings.sphereSize);
    const LayerMaterialSampler sampler(
        albedo, normal, pbr, displacement, settings.textureRepeat);
    const float center = static_cast<float>(settings.sphereSize) * 0.5f;
    // 96² 输出中球体直径 84px；外圈留白同时容纳位移代理的轮廓起伏。
    const float radius = static_cast<float>(settings.sphereSize) * (42.0f / 96.0f);
    const PreviewVec3 view = {0.0f, 0.0f, 1.0f};

    for (uint32_t y = 0; y < settings.sphereSize; ++y)
    {
        for (uint32_t x = 0; x < settings.sphereSize; ++x)
        {
            const float nx = (static_cast<float>(x) + 0.5f - center) / radius;
            const float ny = (center - static_cast<float>(y) - 0.5f) / radius;
            const float radial = std::sqrt(nx * nx + ny * ny);
            const float radialPixels = radial * radius;
            if (radialPixels > radius + 3.0f)
            {
                WritePixel(output, x, y, {}, 0);
                continue;
            }

            // 轮廓外仍用最近的球面方向采样高度，让位移代理可以轻微改变边缘。
            const float clampedRadial = std::min(radial, 1.0f);
            const float sampleNx = radial > 1.0e-6f ?
                nx / radial * clampedRadial : 0.0f;
            const float sampleNy = radial > 1.0e-6f ?
                ny / radial * clampedRadial : 0.0f;
            const float sampleNz = std::sqrt(std::max(
                0.0f, 1.0f - clampedRadial * clampedRadial));
            const float u = std::atan2(sampleNx, sampleNz) / (2.0f * Pi) + 0.5f;
            const float v = 0.5f - std::asin(std::clamp(
                sampleNy, -1.0f, 1.0f)) / Pi;
            const float height = sampler.HeightAt(u, v);
            const float displacedRadius = radius *
                (1.0f + (height - 0.5f) * settings.displacementRadius);
            const float coverage = std::clamp(
                displacedRadius - radialPixels + 0.5f, 0.0f, 1.0f);
            if (coverage <= 0.0f)
            {
                WritePixel(output, x, y, {}, 0);
                continue;
            }

            const LayerPreviewSample sample = sampler.Sample(u, v);
            const PreviewVec3 meshNormal = {sampleNx, sampleNy, sampleNz};
            const float phi = (u - 0.5f) * 2.0f * Pi;
            const PreviewVec3 tangent = {std::cos(phi), 0.0f, -std::sin(phi)};
            const PreviewVec3 bitangent = Normalize(
                Cross(meshNormal, tangent), {0.0f, 1.0f, 0.0f});
            const PreviewVec3 shadedNormal = PerturbNormal(
                sampler, sample, u, v, meshNormal,
                tangent, bitangent, settings);
            const PreviewVec3 color = ShadePbr(sample, shadedNormal, view);
            WritePixel(output, x, y, color,
                static_cast<uint8_t>(std::lround(coverage * 255.0f)));
        }
    }
    return true;
}

bool BuildTerrainLayerPlanePreview(
    const HostImage& albedo,
    const HostImage& normal,
    const HostImage& pbr,
    const TerrainLayerHeightField* displacement,
    const TerrainLayerPreviewSettings& settings,
    HostImage& output)
{
    if (!IsSupportedTexture(albedo) ||
        !IsSupportedTexture(normal) ||
        !IsSupportedTexture(pbr) ||
        (displacement != nullptr && !displacement->IsValid()) ||
        settings.planeWidth == 0 ||
        settings.planeHeight == 0)
    {
        return false;
    }

    output = MakePreviewImage(settings.planeWidth, settings.planeHeight);
    const LayerMaterialSampler sampler(
        albedo, normal, pbr, displacement, 1.0f);
    constexpr float PlanePreviewTiles = 3.0f;
    const float cellWidth =
        static_cast<float>(settings.planeWidth) / PlanePreviewTiles;
    const float cellHeight =
        static_cast<float>(settings.planeHeight) / PlanePreviewTiles;
    constexpr float GridHalfWidth = 0.65f;
    const PreviewVec3 meshNormal = {0.0f, 0.0f, 1.0f};
    const PreviewVec3 tangent = {1.0f, 0.0f, 0.0f};
    const PreviewVec3 bitangent = {0.0f, 1.0f, 0.0f};
    const PreviewVec3 view = {0.0f, 0.0f, 1.0f};

    for (uint32_t y = 0; y < settings.planeHeight; ++y)
    {
        for (uint32_t x = 0; x < settings.planeWidth; ++x)
        {
            const float tileU = (static_cast<float>(x) + 0.5f) / cellWidth;
            const float tileV = (static_cast<float>(y) + 0.5f) / cellHeight;
            const float u = tileU / PlanePreviewTiles;
            const float v = tileV / PlanePreviewTiles;
            LayerPreviewSample sample = sampler.Sample(u, v);

            // 3×3 tile 的极细暗线只提示重复尺度；悬停层的 RmlUi 网格线仍叠加在上层。
            const float localU = (tileU - std::floor(tileU)) * cellWidth;
            const float localV = (tileV - std::floor(tileV)) * cellHeight;
            const bool grid =
                std::min(localU, cellWidth - localU) <= GridHalfWidth ||
                std::min(localV, cellHeight - localV) <= GridHalfWidth;
            if (grid)
                sample.baseColor = Scale(sample.baseColor, 0.72f);

            const PreviewVec3 shadedNormal = PerturbNormal(
                sampler, sample, u, v, meshNormal,
                tangent, bitangent, settings);
            const PreviewVec3 color = ShadePbr(sample, shadedNormal, view);
            WritePixel(output, x, y, color, 255);
        }
    }
    return true;
}

std::string TerrainLayerPreviewCacheKey(
    const TerrainLayerPreviewSourceStamp& albedo,
    const TerrainLayerPreviewSourceStamp& normal,
    const TerrainLayerPreviewSourceStamp& pbr,
    const TerrainLayerPreviewSourceStamp& displacement,
    const TerrainLayerPreviewSettings& settings,
    TerrainLayerPreviewKind kind)
{
    CanonicalFieldHasher hasher("vl-terrain-layer-preview-v3");
    AddSourceStamp(hasher, "albedo", albedo);
    AddSourceStamp(hasher, "normal", normal);
    AddSourceStamp(hasher, "pbr", pbr);
    AddSourceStamp(hasher, "displacement", displacement);
    hasher.AddString("kind", PreviewKindName(kind));
    hasher.AddUInt32("shadingVersion", settings.shadingVersion);
    hasher.AddUInt32("sphereSize", settings.sphereSize);
    hasher.AddUInt32("planeWidth", settings.planeWidth);
    hasher.AddUInt32("planeHeight", settings.planeHeight);
    hasher.AddUInt32("textureRepeat", FloatBits(settings.textureRepeat));
    hasher.AddUInt32("displacementNormalStrength",
        FloatBits(settings.displacementNormalStrength));
    hasher.AddUInt32("displacementRadius",
        FloatBits(settings.displacementRadius));
    return hasher.Finalize().ToHex();
}

TerrainLayerPreviewCache::TerrainLayerPreviewCache()
{
    try
    {
        SetResourceRoot(CommonFunction::GetResourcePath());
    }
    catch (const std::exception&)
    {
        resourceRoot.clear();
    }
}

TerrainLayerPreviewCache::TerrainLayerPreviewCache(
    std::filesystem::path root)
{
    SetResourceRoot(std::move(root));
}

void TerrainLayerPreviewCache::SetResourceRoot(std::filesystem::path root)
{
    if (root.empty())
    {
        resourceRoot.clear();
        Reset();
        return;
    }
    std::error_code error;
    const std::filesystem::path absolute =
        std::filesystem::absolute(root, error);
    resourceRoot = error ? std::move(root) : absolute.lexically_normal();
    Reset();
}

void TerrainLayerPreviewCache::Reset()
{
    materialSourceCache.clear();
    decodedSourceCache.clear();
    generatedPathCache.clear();
    failedPreviewKeys.clear();
    reportedFailures.clear();
}

void TerrainLayerPreviewCache::ReportFailureOnce(
    const std::string& key, const std::string& message)
{
    if (!reportedFailures.insert(key).second)
        return;
    std::fprintf(stderr, "[TerrainLayerPreview] %s\n", message.c_str());
}

const TerrainLayerPreviewCache::MaterialSourceSet*
TerrainLayerPreviewCache::FindMaterialSources(
    const std::string& materialInstancePath)
{
    if (materialInstancePath.empty())
        return nullptr;
    const std::filesystem::path materialPath =
        ResolveResourceFile(resourceRoot, materialInstancePath);
    if (materialPath.empty())
    {
        ReportFailureOnce("material|" + materialInstancePath,
            "Material instance is missing or outside the resource root: " +
                materialInstancePath);
        return nullptr;
    }

    uintmax_t materialSize = 0;
    std::filesystem::file_time_type materialWriteTime{};
    if (!FileMetadata(materialPath, materialSize, materialWriteTime))
        return nullptr;

    const auto found = materialSourceCache.find(materialInstancePath);
    if (found != materialSourceCache.end() &&
        found->second.materialPath == materialPath &&
        found->second.materialSize == materialSize &&
        found->second.materialWriteTime == materialWriteTime)
    {
        bool textureArraysUnchanged = true;
        for (const TextureArraySource& array : found->second.arrays)
        {
            if (array.path.empty())
                continue;
            uintmax_t textureSize = 0;
            std::filesystem::file_time_type textureWriteTime{};
            if (!FileMetadata(array.path, textureSize, textureWriteTime) ||
                array.size != textureSize ||
                array.writeTime != textureWriteTime)
            {
                textureArraysUnchanged = false;
                break;
            }
        }
        if (textureArraysUnchanged)
            return &found->second;
    }

    const LoadedMaterialSourceSet loaded =
        LoadMaterialSourceSet(materialPath, resourceRoot);
    if (loaded.materialPath.empty())
    {
        // 缓存一次失败结果，避免每帧重复打开同一个错误 MI；文件变化会走上面的
        // size/time 校验重新解析。
        MaterialSourceSet failed;
        failed.materialPath = materialPath;
        failed.materialSize = materialSize;
        failed.materialWriteTime = materialWriteTime;
        materialSourceCache[materialInstancePath] = std::move(failed);
        ReportFailureOnce("material|" + materialInstancePath,
            "Material instance does not expose layerAlbedo texture-array sources: " +
                materialInstancePath);
        return nullptr;
    }

    auto& stored = materialSourceCache[materialInstancePath];
    stored.materialPath = loaded.materialPath;
    stored.materialWriteTime = loaded.materialWriteTime;
    stored.materialSize = loaded.materialSize;
    for (size_t channel = 0; channel < stored.arrays.size(); ++channel)
    {
        stored.arrays[channel].path = loaded.arrays[channel].path;
        stored.arrays[channel].writeTime = loaded.arrays[channel].writeTime;
        stored.arrays[channel].size = loaded.arrays[channel].size;
        stored.arrays[channel].sources = loaded.arrays[channel].sources;
        stored.arrays[channel].displacementDeclared =
            loaded.arrays[channel].displacementDeclared;
        stored.arrays[channel].displacementSources =
            loaded.arrays[channel].displacementSources;
    }
    if (stored.arrays[1].path.empty() ||
        stored.arrays[2].path.empty() ||
        stored.arrays[1].sources.empty() ||
        stored.arrays[2].sources.empty())
    {
        ReportFailureOnce("material-pbr|" + materialInstancePath,
            "Layer preview requires complete layerAlbedo/layerNormal/layerPbr "
            "texture arrays; falling back to avgColor: " +
                materialInstancePath);
    }
    return &stored;
}

const HostImage* TerrainLayerPreviewCache::DecodeSource(
    const TerrainLayerPreviewSourceStamp& stamp,
    size_t channel)
{
    if (!stamp.IsValid() || channel >= PreviewChannelNames.size())
        return nullptr;
    CanonicalFieldHasher hasher("vl-terrain-layer-preview-source-v2");
    hasher.AddString("source", stamp.path.generic_string());
    hasher.AddUInt64("size", static_cast<uint64_t>(stamp.size));
    hasher.AddUInt64("writeTime", static_cast<uint64_t>(
        stamp.writeTime.time_since_epoch().count()));
    hasher.AddString("channel", PreviewChannelName(channel));
    const std::string key = hasher.Finalize().ToHex();
    const auto found = decodedSourceCache.find(key);
    if (found != decodedSourceCache.end())
        return &found->second;

    TextureIO::LoadOptions options;
    options.semantic = channel == 0 ?
        HostImage::TextureSemantic::Color :
        (channel == 1 ?
            HostImage::TextureSemantic::Normal :
            HostImage::TextureSemantic::Data);
    options.flipY = TextureIO::LoadOptions::FlipYMode::ForceOff;
    options.transfer = channel == 0 ?
        TextureIO::LoadOptions::Transfer::SRGB :
        TextureIO::LoadOptions::Transfer::Linear;
    options.forceChannels = 4;
    std::optional<HostImage> image;
    try
    {
        image = TextureIO::Load(stamp.path, options);
    }
    catch (const std::exception&)
    {
        image.reset();
    }
    if (!image || !IsSupportedTexture(*image))
    {
        ReportFailureOnce("decode|" + key,
            "Layer " + PreviewChannelName(channel) +
                " could not be decoded as RGBA8: " +
                stamp.path.generic_string());
        return nullptr;
    }
    decodedSourceCache[key] = std::move(*image);
    return &decodedSourceCache[key];
}

const TerrainLayerHeightField* TerrainLayerPreviewCache::DecodeDisplacement(
    const TerrainLayerPreviewSourceStamp& stamp)
{
    if (!stamp.IsValid())
        return nullptr;
    CanonicalFieldHasher hasher("vl-terrain-layer-preview-displacement-v1");
    hasher.AddString("source", stamp.path.generic_string());
    hasher.AddUInt64("size", static_cast<uint64_t>(stamp.size));
    hasher.AddUInt64("writeTime", static_cast<uint64_t>(
        stamp.writeTime.time_since_epoch().count()));
    const std::string key = hasher.Finalize().ToHex();
    const auto found = decodedDisplacementCache.find(key);
    if (found != decodedDisplacementCache.end())
        return &found->second;

    TerrainLayerHeightField field;
    if (!LoadTerrainLayerDisplacement(stamp.path, field))
    {
        ReportFailureOnce("displacement|" + key,
            "Layer 16-bit displacement could not be decoded: " +
                stamp.path.generic_string());
        return nullptr;
    }
    decodedDisplacementCache[key] = std::move(field);
    return &decodedDisplacementCache[key];
}

std::string TerrainLayerPreviewCache::EnsurePreview(
    const TerrainLayerPreviewSources& sources,
    const std::array<TerrainLayerPreviewSourceStamp, 4>& stamps,
    std::array<const HostImage*, 3>& decodedSources,
    const TerrainLayerHeightField*& decodedDisplacement,
    TerrainLayerPreviewKind kind)
{
    if (!sources.IsComplete() ||
        !stamps[0].IsValid() ||
        !stamps[1].IsValid() ||
        !stamps[2].IsValid())
    {
        return {};
    }

    const TerrainLayerPreviewSettings settings =
        DefaultTerrainLayerPreviewSettings;
    const std::string key = TerrainLayerPreviewCacheKey(
        stamps[0], stamps[1], stamps[2], stamps[3], settings, kind);
    if (failedPreviewKeys.count(key) != 0)
        return {};

    const auto generated = generatedPathCache.find(key);
    if (generated != generatedPathCache.end() &&
        std::filesystem::is_regular_file(generated->second))
    {
        return generated->second;
    }

    const std::filesystem::path cacheDirectory =
        resourceRoot / "Generated" / "TerrainPreview" / "PreviewCache";
    const std::filesystem::path target = cacheDirectory / (key + ".png");
    std::error_code error;
    const uintmax_t targetSize = std::filesystem::file_size(target, error);
    if (!error && targetSize > 0 &&
        std::filesystem::is_regular_file(target, error) && !error)
    {
        const std::string path = target.generic_string();
        generatedPathCache[key] = path;
        return path;
    }

    for (size_t channel = 0; channel < decodedSources.size(); ++channel)
    {
        if (decodedSources[channel] == nullptr)
            decodedSources[channel] = DecodeSource(stamps[channel], channel);
        if (decodedSources[channel] == nullptr)
        {
            failedPreviewKeys.insert(key);
            return {};
        }
    }

    if (stamps[3].IsValid() && decodedDisplacement == nullptr)
        decodedDisplacement = DecodeDisplacement(stamps[3]);
    if (stamps[3].IsValid() && decodedDisplacement == nullptr)
    {
        failedPreviewKeys.insert(key);
        return {};
    }

    HostImage preview;
    const bool built = kind == TerrainLayerPreviewKind::Sphere
        ? BuildTerrainLayerSpherePreview(
            *decodedSources[0], *decodedSources[1], *decodedSources[2],
            decodedDisplacement, settings, preview)
        : BuildTerrainLayerPlanePreview(
            *decodedSources[0], *decodedSources[1], *decodedSources[2],
            decodedDisplacement, settings, preview);
    if (!built)
    {
        failedPreviewKeys.insert(key);
        ReportFailureOnce("build|" + key,
            "Layer preview generation failed for: " +
                sources.albedoPath.generic_string());
        return {};
    }

    TextureIO::SaveOptions saveOptions;
    saveOptions.format = TextureIO::FileFormat::Png;
    const std::vector<uint8_t> bytes = TextureIO::SaveToMemory(preview, saveOptions);
    if (bytes.empty())
    {
        failedPreviewKeys.insert(key);
        ReportFailureOnce("encode|" + key,
            "Layer preview PNG encoding failed for: " +
                sources.albedoPath.generic_string());
        return {};
    }

    try
    {
        WriteBinaryFileAtomically(target, bytes);
    }
    catch (const std::exception& exception)
    {
        failedPreviewKeys.insert(key);
        ReportFailureOnce("write|" + key,
            "Layer preview cache write failed: " + std::string(exception.what()));
        return {};
    }

    const std::string path = target.generic_string();
    generatedPathCache[key] = path;
    return path;
}

TerrainLayerPreviewPaths TerrainLayerPreviewCache::Get(
    const TerrainAssetDesc& desc, size_t layerIndex)
{
    TerrainLayerPreviewPaths paths;
    if (layerIndex >= desc.layers.size() || resourceRoot.empty())
        return paths;

    try
    {
        const MaterialSourceSet* sources =
            FindMaterialSources(desc.materialInstancePath);
        const size_t slice = TerrainLayerSourceSlice(desc, layerIndex);
        if (sources == nullptr ||
            slice == std::numeric_limits<size_t>::max() ||
            slice >= sources->arrays[0].sources.size() ||
            slice >= sources->arrays[1].sources.size() ||
            slice >= sources->arrays[2].sources.size())
        {
            return paths;
        }

        TerrainLayerPreviewSources layerSources;
        layerSources.albedoPath = sources->arrays[0].sources[slice];
        layerSources.normalPath = sources->arrays[1].sources[slice];
        layerSources.pbrPath = sources->arrays[2].sources[slice];
        if (!layerSources.IsComplete())
        {
            ReportFailureOnce(
                "sources|" + desc.materialInstancePath + "|" +
                    std::to_string(slice),
                "Layer slice " + std::to_string(slice) +
                    " is missing albedo/normal/pbr; falling back to avgColor");
            return paths;
        }

        // 组声明了 displacementSources 就必须给出当前 slice 的可用 16-bit 源；
        // 声明但缺失/损坏时不静默退回 pbr.a，避免把 8-bit 近似伪装成同一路径。
        if (sources->arrays[0].displacementDeclared)
        {
            layerSources.displacementDeclared = true;
            if (slice >= sources->arrays[0].displacementSources.size() ||
                sources->arrays[0].displacementSources[slice].empty())
            {
                ReportFailureOnce(
                    "displacement|" + desc.materialInstancePath + "|" +
                        std::to_string(slice),
                    "Layer slice " + std::to_string(slice) +
                        " is missing its 16-bit displacement source");
                return paths;
            }
            layerSources.displacementPath =
                sources->arrays[0].displacementSources[slice];
        }

        std::array<TerrainLayerPreviewSourceStamp, PreviewStampCount> stamps;
        stamps[0].path = layerSources.albedoPath;
        stamps[1].path = layerSources.normalPath;
        stamps[2].path = layerSources.pbrPath;
        stamps[3].path = layerSources.displacementPath;
        for (size_t stampIndex = 0; stampIndex < PreviewTextureCount; ++stampIndex)
        {
            if (!FileMetadata(
                    stamps[stampIndex].path,
                    stamps[stampIndex].size,
                    stamps[stampIndex].writeTime))
                return paths;
        }
        if (stamps[3].IsValid() &&
            !FileMetadata(stamps[3].path, stamps[3].size, stamps[3].writeTime))
        {
            return paths;
        }

        std::array<const HostImage*, 3> decodedSources = {
            nullptr, nullptr, nullptr};
        const TerrainLayerHeightField* decodedDisplacement = nullptr;
        paths.spherePath = EnsurePreview(
            layerSources, stamps, decodedSources, decodedDisplacement,
            TerrainLayerPreviewKind::Sphere);
        if (!paths.spherePath.empty())
        {
            paths.planePath = EnsurePreview(
                layerSources, stamps, decodedSources, decodedDisplacement,
                TerrainLayerPreviewKind::Plane);
        }
    }
    catch (const std::exception& exception)
    {
        ReportFailureOnce("unexpected|" + desc.materialInstancePath,
            "Layer preview failed: " + std::string(exception.what()));
    }
    return paths;
}

} // namespace VL::Editor
