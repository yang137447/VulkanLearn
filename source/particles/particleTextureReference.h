#pragma once

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace VL
{

inline std::string LowerParticleTextureExtension(const std::string& path)
{
    std::string extension =
        std::filesystem::path(path).extension().string();
    for (char& character : extension)
    {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return extension;
}

inline bool IsSupportedParticleTextureExtension(
    const std::string& path)
{
    const std::string extension =
        LowerParticleTextureExtension(path);
    return extension == ".png" || extension == ".jpg" ||
        extension == ".jpeg" || extension == ".tga" ||
        extension == ".bmp";
}

// 纹理热切换身份：路径 + 文件大小 + 修改时间 + mipmap 配置。同一路径内容变化、空->非空、
// A->B 都会得到不同的 key，ParticleRenderer 据此 retire 旧发射器并重新解码。
// 文件不可读时返回稳定的 "missing" 键，避免渲染线程抛异常；文件重新出现后
// 真实 size/time 会再次改变 key。
inline std::string BuildParticleTextureSourceKey(
    const std::string& reference,
    const std::filesystem::path& resolvedPath,
    bool generateMipmaps = true)
{
    if (reference.empty())
    {
        return {};
    }

    std::error_code sizeError;
    const std::uintmax_t size =
        std::filesystem::file_size(resolvedPath, sizeError);
    std::error_code timeError;
    const std::filesystem::file_time_type writeTime =
        std::filesystem::last_write_time(resolvedPath, timeError);

    std::string key = reference;
    key += '|';
    key += sizeError
        ? std::string("missing")
        : std::to_string(size);
    key += '|';
    key += timeError
        ? std::string("missing")
        : std::to_string(static_cast<long long>(
            writeTime.time_since_epoch().count()));
    // 图集与单帧纹理使用不同的 mipmap 采样配置，不能共享同一 GPU 纹理。
    key += generateMipmaps ? "|mipmap" : "|no-mipmap";
    return key;
}

// 作者侧纹理引用必须留在 service 的 resourceRoot 内。渲染线程仍会做最终
// 解码与失败回退，但保存路径在这里先拒绝明显无效的引用，避免坏资产落盘。
inline std::filesystem::path ValidateParticleTextureReference(
    const std::filesystem::path& resourceRoot,
    const std::string& reference,
    std::string_view fieldName)
{
    std::string portable = reference;
    for (char& character : portable)
    {
        if (character == '\\')
        {
            character = '/';
        }
    }
    const std::filesystem::path parsed(portable);
    if (reference.empty() || reference.find('\0') != std::string::npos ||
        parsed.is_absolute() || parsed.has_root_name() ||
        parsed.has_root_directory())
    {
        throw std::invalid_argument(
            "Particle texture field '" + std::string(fieldName) +
            "' must be resource-root-relative: " + reference);
    }
    for (const auto& part : parsed)
    {
        if (part == "..")
        {
            throw std::invalid_argument(
                "Particle texture field '" + std::string(fieldName) +
                "' must not contain traversal: " + reference);
        }
    }

    const std::filesystem::path absolute =
        std::filesystem::weakly_canonical(resourceRoot / parsed);
    const std::filesystem::path relative =
        absolute.lexically_relative(
            std::filesystem::weakly_canonical(resourceRoot));
    if (relative.empty() || relative.is_absolute() ||
        *relative.begin() == "..")
    {
        throw std::invalid_argument(
            "Particle texture field '" + std::string(fieldName) +
            "' escapes resource root: " + reference);
    }
    if (!std::filesystem::is_regular_file(absolute))
    {
        throw std::invalid_argument(
            "Particle texture field '" + std::string(fieldName) +
            "' references a missing file: " + reference);
    }
    if (!IsSupportedParticleTextureExtension(reference))
    {
        throw std::invalid_argument(
            "Particle texture field '" + std::string(fieldName) +
            "' must use png/jpg/jpeg/tga/bmp: " + reference);
    }
    return absolute;
}

} // namespace VL
