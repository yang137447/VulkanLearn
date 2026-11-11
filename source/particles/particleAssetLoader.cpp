#include "particles/particleAssetInternal.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

#include "commonFunction.h"
#include "particles/particleTextureReference.h"

namespace VL
{

using namespace ParticleAssetDetail;
ParticleSystemAsset ParticleAssetLoader::Prepare(const std::string& path)
{
    std::string resolvedPath;
    try
    {
        resolvedPath = CommonFunction::Path(path);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to open particle system asset: " + path +
            ": " + exception.what());
    }

    std::ifstream file(resolvedPath);
    if (!file.is_open())
    {
        throw std::runtime_error(
            "Failed to open particle system asset: " + path);
    }

    const std::string source{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()};
    if (file.bad())
    {
        throw std::runtime_error(
            "Failed to read particle system asset: " + path);
    }

    nlohmann::json json;
    try
    {
        json = nlohmann::json::parse(source);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to parse particle system asset: " + path +
            ": " + exception.what());
    }
    return ParseJson(
        json,
        path,
        std::filesystem::path(CommonFunction::GetResourcePath()));
}

ParticleSystemAsset ParticleAssetLoader::ParseJson(
    const nlohmann::json& json,
    std::string_view sourcePath,
    const std::filesystem::path& resourceRoot,
    const ParticleCollisionResourceCache* collisionCache)
{
    const std::string path(sourcePath);
    if (!json.is_object())
    {
        throw std::invalid_argument(
            "Particle system asset must be a JSON object: " + path);
    }
    if (json.value("type", std::string()) != "particleSystem")
    {
        throw std::invalid_argument(
            "Particle system asset requires type=particleSystem: " + path);
    }

    ParticleSystemAsset asset;
    asset.sourcePath =
        std::filesystem::path(path).lexically_normal().generic_string();
    asset.name = json.value("name", asset.sourcePath);
    if (json.contains("version"))
    {
        const nlohmann::json& version = json.at("version");
        if (!version.is_number_integer() &&
            !version.is_number_unsigned())
        {
            throw std::invalid_argument(
                "Particle version must be an integer: " + path);
        }
        const int value = version.get<int>();
        if (value != 1 && value != 2)
        {
            throw std::invalid_argument(
                "Particle version must be 1 or 2: " + path);
        }
        asset.version = static_cast<uint32_t>(value);
    }
    if (asset.version == 2)
        asset.userParameters = ParseParticleUserParameters(json);
    std::unordered_set<std::string> userParameterReferences;
    if (json.contains("emitters"))
    {
        const nlohmann::json& emitters = json.at("emitters");
        if (!emitters.is_array() || emitters.empty())
        {
            throw std::invalid_argument(
                "Particle emitters requires a non-empty array");
        }
        if (emitters.size() > kMaxParticleEmitters)
        {
            throw std::invalid_argument(
                "Particle emitters supports at most 16 entries");
        }
        asset.emitters.reserve(emitters.size());
        for (size_t index = 0; index < emitters.size(); ++index)
        {
            nlohmann::json flattened = asset.version == 2
                ? FlattenParticleEmitterV2(emitters.at(index), index)
                : emitters.at(index);
            if (asset.version == 2)
            {
                SubstituteParticleUserParameters(
                    flattened,
                    asset.userParameters,
                    "emitters[" + std::to_string(index) + "]",
                    &userParameterReferences);
            }
            asset.emitters.push_back(
                ParseEmitter(
                    flattened,
                    index,
                    false,
                    resourceRoot,
                    collisionCache));
        }
    }
    else if (asset.version == 2 && json.contains("modules"))
    {
        // v2 也允许根对象直接携带一个隐式 emitter 的 modules。
        nlohmann::json flattened = FlattenParticleEmitterV2(json, 0);
        SubstituteParticleUserParameters(
            flattened,
            asset.userParameters,
            "modules",
            &userParameterReferences);
        asset.emitters.push_back(
            ParseEmitter(
                flattened,
                0,
                true,
                resourceRoot,
                collisionCache));
    }
    else
    {
        // 旧 PS 资产没有 emitters：根对象就是一个隐式发射器。
        asset.emitters.push_back(
            ParseEmitter(
                json,
                0,
                true,
                resourceRoot,
                collisionCache));
    }

    Validate(asset);
    asset.userParameterReferences =
        std::move(userParameterReferences);
    for (const ParticleEmitterAsset& emitter : asset.emitters)
    {
        if (emitter.texturePath.empty()) continue;
        // 在场景候选提交前解析一次资源根，避免渲染线程首次创建发射器时
        // 才发现 flipbook 纹理缺失。GPU 纹理本身仍由 ParticleRenderer 创建。
        if (resourceRoot.empty())
        {
            (void)CommonFunction::Path(emitter.texturePath);
        }
        else
        {
            (void)ValidateParticleTextureReference(
                resourceRoot, emitter.texturePath, "texture.path");
        }
    }
    // 只有 v2 + 用户参数才需要运行时覆盖重解析；冻结原始 JSON
    // （而不是展平后的 emitter），保证模块结构/字段归属不丢失。
    if (asset.version == 2 && !asset.userParameters.empty())
    {
        asset.authoringJson =
            std::make_shared<const nlohmann::json>(json);
    }
    return asset;
}

void ParticleAssetLoader::Validate(const ParticleSystemAsset& asset)
{
    if (asset.name.empty())
    {
        throw std::invalid_argument("Particle system requires a name");
    }
    if (asset.emitters.empty() ||
        asset.emitters.size() > kMaxParticleEmitters)
    {
        throw std::invalid_argument(
            "Particle system requires 1..16 emitters");
    }
    std::unordered_set<std::string> names;
    names.reserve(asset.emitters.size());
    for (const ParticleEmitterAsset& emitter : asset.emitters)
    {
        if (emitter.name.empty())
        {
            throw std::invalid_argument(
                "Particle emitter requires a non-empty name");
        }
        if (!names.insert(emitter.name).second)
        {
            throw std::invalid_argument(
                "Particle emitter names must be unique: " +
                emitter.name);
        }
        ValidateEmitter(emitter);
    }
    for (const ParticleEmitterAsset& emitter : asset.emitters)
    {
        if (emitter.spawnSource.mode != "parentEmitter")
        {
            continue;
        }
        if (emitter.spawnSource.parentEmitter == emitter.name)
        {
            throw std::invalid_argument(
                "Particle spawnSource.parentEmitter cannot reference itself");
        }
        if (names.find(emitter.spawnSource.parentEmitter) == names.end())
        {
            throw std::invalid_argument(
                "Particle spawnSource.parentEmitter is unknown: " +
                emitter.spawnSource.parentEmitter);
        }
    }
}

} // namespace VL
