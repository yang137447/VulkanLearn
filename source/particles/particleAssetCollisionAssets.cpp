#include "particles/particleAssetInternal.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "commonFunction.h"
#include "particles/particleMesh.h"
#include "particles/particleMeshCollision.h"
#include "particles/particleMeshReference.h"

namespace VL
{
namespace ParticleAssetDetail
{
std::string BuildParticleCollisionSourceIdentity(
    const std::string& reference,
    const std::filesystem::path& resolvedPath)
{
    std::error_code sizeError;
    const std::uintmax_t size =
        std::filesystem::file_size(resolvedPath, sizeError);
    std::error_code timeError;
    const std::filesystem::file_time_type writeTime =
        std::filesystem::last_write_time(resolvedPath, timeError);
    std::string key = reference;
    key += '|';
    key += sizeError ? std::string("missing") : std::to_string(size);
    key += '|';
    key += timeError
        ? std::string("missing")
        : std::to_string(static_cast<long long>(
            writeTime.time_since_epoch().count()));
    // 文件大小/时间粒度不足以保证失效；碰撞资产只在加载时计算一次内容
    // 摘要，避免运行时重解析再次读盘。
    std::ifstream file(resolvedPath, std::ios::binary);
    std::uint64_t contentHash = 1469598103934665603ull;
    if (file.is_open())
    {
        char buffer[8192];
        while (file.good())
        {
            file.read(buffer, sizeof(buffer));
            const std::streamsize count = file.gcount();
            for (std::streamsize index = 0; index < count; ++index)
            {
                contentHash ^= static_cast<std::uint8_t>(
                    buffer[static_cast<size_t>(index)]);
                contentHash *= 1099511628211ull;
            }
        }
    }
    key += '|';
    key += std::to_string(contentHash);
    return key;
}

std::shared_ptr<const ParticleSdfAsset> LoadParticleSdfAsset(
    const std::string& path,
    const std::filesystem::path& resourceRoot)
{
    if (path.empty())
    {
        return nullptr;
    }
    RequireValidParticleMeshPath(path, "collision.sdf.path");
    std::filesystem::path resolvedPath;
    try
    {
        resolvedPath = resourceRoot.empty()
            ? std::filesystem::path(CommonFunction::Path(path))
            : ValidateParticleMeshReference(
                resourceRoot, path, "collision.sdf.path");
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to open particle SDF asset: " + path +
            ": " + exception.what());
    }
    std::ifstream file(resolvedPath);
    if (!file.is_open())
    {
        throw std::runtime_error(
            "Failed to open particle SDF asset: " + path);
    }
    nlohmann::json json;
    try
    {
        file >> json;
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to parse particle SDF asset: " + path +
            ": " + exception.what());
    }
    ParticleSdfAsset sdf = ParseParticleSdfJson(json);
    sdf.sourceIdentity =
        BuildParticleCollisionSourceIdentity(path, resolvedPath);
    return std::make_shared<const ParticleSdfAsset>(std::move(sdf));
}

std::shared_ptr<const ParticleSdfAsset> LoadParticleCollisionMeshAsset(
    const std::string& path,
    const std::filesystem::path& resourceRoot,
    float minimumPadding)
{
    if (path.empty())
    {
        return nullptr;
    }
    RequireValidParticleMeshPath(path, "collision.mesh.path");
    std::filesystem::path resolvedPath;
    try
    {
        resolvedPath = resourceRoot.empty()
            ? std::filesystem::path(CommonFunction::Path(path))
            : ValidateParticleMeshReference(
                resourceRoot, path, "collision.mesh.path");
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to open particle collision mesh asset: " + path +
            ": " + exception.what());
    }
    const ParticleCollisionMeshAsset mesh =
        ReadParticleCollisionMeshFile(resolvedPath);
    // SDF 在 bounds 外返回“无碰撞”。如果 padding 小于粒子半径，
    // 粒子球可能已经接触网格但中心仍在 bounds 外，形成漏碰撞。
    if (std::isfinite(minimumPadding) &&
        minimumPadding > 0.0f &&
        mesh.padding + 1.0e-5f < minimumPadding)
    {
        throw std::invalid_argument(
            "Particle collision mesh padding must be >= "
            "collision.radius");
    }
    ParticleSdfAsset sdf = BuildParticleSdfFromCollisionMesh(mesh);
    sdf.sourceIdentity =
        BuildParticleCollisionSourceIdentity(path, resolvedPath);
    return std::make_shared<const ParticleSdfAsset>(std::move(sdf));
}

} // namespace ParticleAssetDetail
} // namespace VL
