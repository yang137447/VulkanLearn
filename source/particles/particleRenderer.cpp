#include "particles/particleRendererInternal.h"

namespace VL
{

ParticleRenderer::~ParticleRenderer()
{
    // 正常关闭走 Shutdown；若调用方绕过它，至少在同一 backend 仍有效时
    // 走到设备空闲并释放全局池/管线/查询池，避免析构泄漏。
    if (backend != nullptr)
    {
        try
        {
            backend->WaitIdle();
        }
        catch (...)
        {
            // 析构不能把关闭路径异常抛回引擎。
        }
        Shutdown(*backend);
    }
}

const ParticleSystemAsset&
ParticleRenderer::ResolveParticleSystemAsset(
    const ParticleSystemSnapshot& snapshot,
    const ParticleDataChannelMap& dataChannels,
    const std::unordered_map<std::string, float>& userParameters)
{
    const ParticleSystemAsset& base = snapshot.asset;
    // 只有“带参数的 v2 作者 JSON + 该资产实际引用的运行时值”才需要重解析。
    const ParticleDataChannelMap effectiveChannels =
        FilterParticleRuntimeParametersTyped(base, dataChannels);
    const std::unordered_map<std::string, float> effectiveParameters =
        FilterParticleRuntimeParameters(base, userParameters);
    if (!base.authoringJson ||
        (effectiveChannels.empty() && effectiveParameters.empty()))
    {
        return base;
    }

    const std::string cacheKey = BuildParticleSystemAssetKey(
        snapshot.name, base.sourcePath);
    const uint64_t signature =
        BuildParticleUserParameterSignature(
            effectiveParameters);
    const uint64_t channelSignature =
        BuildParticleDataChannelSignature(effectiveChannels);
    const nlohmann::json* authoring = base.authoringJson.get();

    auto entryIt = userParameterCache.find(cacheKey);
    if (entryIt != userParameterCache.end() &&
        entryIt->second.attempted &&
        entryIt->second.signature == signature &&
        entryIt->second.channelSignature == channelSignature &&
        entryIt->second.authoringJson == authoring)
    {
        return entryIt->second.parsed
            ? entryIt->second.asset
            : (entryIt->second.asset.authoringJson !=
                    base.authoringJson ||
                entryIt->second.asset.emitters.empty()
                ? base
                : entryIt->second.asset);
    }

    // 缓存有界：每个系统一条，超出上限时淘汰最早插入项。缓存只服务
    // 渲染线程，不跨系统共享资产。
    if (entryIt == userParameterCache.end())
    {
        if (userParameterCache.size() >= 32)
        {
            userParameterCache.erase(userParameterCache.begin());
        }
        ParticleUserParameterCacheEntry entry;
        entry.asset = base; // 解析失败时的 last-good 起点
        entryIt = userParameterCache.emplace(
            cacheKey, std::move(entry)).first;
    }

    ParticleUserParameterCacheEntry& entry = entryIt->second;
    entry.signature = signature;
    entry.channelSignature = channelSignature;
    entry.authoringJson = authoring;
    entry.attempted = true;
    try
    {
        // 合并 -> ParseJson -> 运行时-only emitter 对齐；缓存资产始终
        // 保留原始作者 JSON 指针，下一次覆盖仍在原始模块结构上合并。
        ParticleSystemAsset parsed =
            ParseParticleAssetWithRuntimeParametersTyped(
                base,
                effectiveChannels,
                effectiveParameters,
                std::filesystem::path(
                    CommonFunction::GetResourcePath()));
        entry.asset = std::move(parsed);
        entry.parsed = true;
    }
    catch (const std::exception& exception)
    {
        // 渲染线程不能抛异常：保留 last-good，并只告警一次。
        WarnUserParameterFailureOnce(
            cacheKey, signature, exception.what());
        entry.parsed = false;
    }
    return entry.parsed
        ? entry.asset
        : (entry.asset.authoringJson != base.authoringJson ||
            entry.asset.emitters.empty()
            ? base
            : entry.asset);
}

void ParticleRenderer::WarnUserParameterFailureOnce(
    const std::string& key,
    uint64_t signature,
    const std::string& error)
{
    const std::string warningKey =
        key + "|" + std::to_string(signature);
    if (!warnedUserParameterFailures.insert(warningKey).second)
    {
        return;
    }
    const std::string message =
        "Particle runtime user parameters ignored for '" + key +
        "': " + error + "; keeping the last good asset";
    ProcessLog().Append(
        LogSeverity::Warning, "Particles", message);
    std::cout << "[Particles][Warning] " << message << std::endl;
}

} // namespace VL
