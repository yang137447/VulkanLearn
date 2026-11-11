#include "particles/particleRendererInternal.h"

namespace VL
{

void ParticleRenderer::Synchronize(const RenderScene& renderScene)
{
    if (!initialized)
    {
        return;
    }

    resolvedSystemAssets.clear();
    std::unordered_set<std::string> desired;
    size_t emitterCount = 0;
    for (const ParticleSystemSnapshot& snapshot :
         renderScene.particleSystems)
        emitterCount += snapshot.asset.emitters.size();
    desired.reserve(emitterCount);
    const uint32_t swapchainImageCount =
        backend->GetSwapchainImageCount();
    for (const ParticleSystemSnapshot& snapshot :
         renderScene.particleSystems)
    {
        // 运行时 userParameters 覆盖可能改变展平后的数值字段；只有签名/
        // 作者指针变化时才重新 ParseJson，失败保留 last-good。
        const ParticleSystemAsset* resolvedAsset = &snapshot.asset;
        try
        {
            resolvedAsset = &ResolveParticleSystemAsset(
                snapshot,
                renderScene.particleDataChannels,
                snapshot.userParameters);
        }
        catch (const std::exception& exception)
        {
            // 缓存插入/JSON 复制也可能分配失败；渲染线程必须保持无异常。
            WarnUserParameterFailureOnce(
                snapshot.name + "|" + snapshot.assetPath,
                0u,
                exception.what());
        }
        catch (...)
        {
            WarnUserParameterFailureOnce(
                snapshot.name + "|" + snapshot.assetPath,
                0u,
                "unknown runtime user parameter failure");
        }
        const ParticleSystemAsset& systemAsset = *resolvedAsset;
        resolvedSystemAssets[BuildParticleSystemAssetKey(
            snapshot.name, snapshot.assetPath)] = systemAsset;
        std::unordered_map<std::string,
            const ParticleEmitterTelemetry*> telemetryByName;
        telemetryByName.reserve(snapshot.telemetry.emitters.size());
        for (const ParticleEmitterTelemetry& telemetry :
             snapshot.telemetry.emitters)
        {
            if (!telemetry.name.empty())
            {
                telemetryByName.emplace(
                    telemetry.name, &telemetry);
            }
        }
        // 快照通常带 World 生成的逐发射器预算；若 telemetry 缺失/名称
        // 不匹配，则用本地镜像按 snapshot.budget 计算，避免总上限失效。
        bool telemetryBudgetsUsable =
            !snapshot.telemetry.emitters.empty();
        if (telemetryBudgetsUsable)
        {
            for (const ParticleEmitterAsset& emitter :
                 systemAsset.emitters)
            {
                if (telemetryByName.find(emitter.name) ==
                    telemetryByName.end())
                {
                    telemetryBudgetsUsable = false;
                    break;
                }
            }
        }
        std::vector<uint32_t> fallbackBudgets;
        if (!telemetryBudgetsUsable)
        {
            std::vector<uint32_t> authored;
            authored.reserve(systemAsset.emitters.size());
            for (const ParticleEmitterAsset& emitter :
                 systemAsset.emitters)
            {
                // beam/light 不分配普通模拟粒子池；decal 现在与 billboard
                // 一样复用 GPU 模拟池，只有 beam 仍为零预算纯渲染模式。
                authored.push_back(
                    (emitter.renderMode == "beam" ||
                     emitter.renderMode == "light")
                        ? 0u
                        : emitter.maxParticles);
            }
            fallbackBudgets =
                ComputeParticleEmitterBudgetCapacities(
                    authored,
                    snapshot.budget.maxTotalParticles,
                    snapshot.budget.emitterBudgetScale);
        }

        for (size_t emitterIndex = 0;
             emitterIndex < systemAsset.emitters.size();
             ++emitterIndex)
        {
            const ParticleEmitterAsset& authoredEmitter =
                systemAsset.emitters[emitterIndex];
            // decal 发射器进入普通 GPU 模拟池，再由 particleDecalGather.comp
            // 压成紧凑记录回读给 DecalRenderer；light 发射器仍走独立
            // light emit compute 输出到场景光照缓冲。
            const auto telemetryIt = telemetryByName.find(
                authoredEmitter.name);
            const ParticleEmitterTelemetry* telemetry =
                telemetryIt != telemetryByName.end()
                ? telemetryIt->second
                : nullptr;
            // 预算 0 表示本帧不分配池；不加入 desired，已有池走正常
            // retire 清理路径。预算变化会让下面的 maxParticles 比较
            // 触发池重建。
            const uint32_t budgetedMaxParticles =
                telemetryBudgetsUsable && telemetry != nullptr
                ? telemetry->budgetedMaxParticles
                : fallbackBudgets.at(emitterIndex);
            // beam 是纯渲染模式：即使 emitterBudgetScale=0 或前面的
            // emitter 耗尽总预算，也不能把电弧整个裁掉。
            const uint32_t effectiveMaxParticles =
                authoredEmitter.renderMode == "beam"
                    ? authoredEmitter.maxParticles
                    : (authoredEmitter.renderMode == "light"
                        ? std::min(
                            authoredEmitter.maxParticles,
                            kMaxParticleLights)
                        : ResolveParticleEmitterCapacity(
                            authoredEmitter.maxParticles,
                            budgetedMaxParticles,
                            telemetryBudgetsUsable));
            if (!ShouldAllocateParticlePool(
                    effectiveMaxParticles))
            {
                continue;
            }
            ParticleEmitterAsset emitter = authoredEmitter;
            emitter.maxParticles = effectiveMaxParticles;
            const bool enabled =
                emitterIndex < kMaxParticleEmitters &&
                ((snapshot.runtime.enabledEmitterMask >> emitterIndex) & 1u) != 0;
            const std::string key =
                BuildEmitterKey(snapshot, emitter);
            desired.insert(key);
            const Eigen::Matrix4f model =
                BuildEmitterModel(snapshot, emitter);
            const std::string textureSourceKey =
                BuildEmitterTextureSourceKey(emitter);
            const std::string meshSourceKey =
                BuildEmitterMeshSourceKey(emitter);
            auto emitterIt = emitters.find(key);
            const ParticleIndirectDrawShape indirectShape =
                BuildParticleIndirectDrawShape(
                    emitter.renderMode,
                    emitter.maxParticles,
                    emitter.ribbonSegments,
                    emitter.renderMode == "mesh",
                    emitterIt == emitters.end()
                        ? 0u
                        : emitterIt->second->meshIndexCount,
                    emitter.beam.segments,
                    emitter.beam.count,
                    emitter.beam.branchCount);
            // 可见性/启用状态与实例覆盖写入所有分支：新建池直接继承，
            // 已有池每帧跟随快照；开关切换不走资源重建。
            if (emitterIt == emitters.end() ||
                emitterIt->second->asset.maxParticles !=
                    emitter.maxParticles ||
                emitterIt->second->asset.sortParticles !=
                    emitter.sortParticles ||
                emitterIt->second->asset.materialInstancePath !=
                    emitter.materialInstancePath ||
                ParticleCollisionResourceIdentityChanged(
                    emitterIt->second->asset,
                    emitter) ||
                // GPU light 输出 buffer 在本帧才可用时，需要重建描述符包；
                // 不能留下一个只有 simulate set、没有 emit set 的发射器。
                (emitter.renderMode == "light" &&
                 particleLightBuffers.size() >= swapchainImageCount &&
                 (emitterIt->second->lightEmitSets.size() <
                      swapchainImageCount ||
                  emitterIt->second->lightBufferGeneration !=
                      particleLightBufferGeneration)) ||
                // 场景灯/IBL descriptor 换代后所有图形发射器都要重建 set 0，
                // 否则会继续引用已退休的 buffer/cubemap。
                emitterIt->second->sceneLightingGeneration !=
                    sceneLightingGeneration ||
                emitterIt->second->textureSourceKey !=
                    textureSourceKey ||
                emitterIt->second->meshSourceKey !=
                    meshSourceKey ||
                emitterIt->second->indirectShape != indirectShape)
            {
                if (emitterIt != emitters.end())
                {
                    RetireEmitterResources(
                        std::move(emitterIt->second));
                    emitters.erase(emitterIt);
                }
                CreateEmitter(
                    emitter, model, key, snapshot.visible, enabled,
                    snapshot.runtime.spawnRateScale,
                    snapshot.runtime.colorTint);
                const auto created = emitters.find(key);
                if (created != emitters.end() && telemetry != nullptr)
                {
                    created->second->restartEpoch =
                        telemetry->restartEpoch;
                }
                continue;
            }

            // 资产参数和摆放矩阵每帧都可变化；GPU 缓冲只随 maxParticles 重建。
            emitterIt->second->asset = emitter;
            emitterIt->second->model = model;
            emitterIt->second->visible = snapshot.visible;
            emitterIt->second->enabled = enabled;
            emitterIt->second->spawnRateScale =
                snapshot.runtime.spawnRateScale;
            emitterIt->second->colorTint = snapshot.runtime.colorTint;
            emitterIt->second->meshScale = emitter.meshScale;
            emitterIt->second->meshAlignment =
                emitter.meshAlignment;
            // 逐帧时长/轴心是 pack 进 UBO 的派生表，必须在原地更新，
            // 否则编辑器改动或热重载后会继续用创建时的旧表。
            emitterIt->second->flipbookTable =
                BuildParticleFlipbookGpuTable(emitter);
            // World stable slot 的 restartEpoch 变化只重置该发射器的模拟
            // 状态；池/纹理/描述符保持原样，不重建 GPU 资源。
            if (telemetry != nullptr &&
                emitterIt->second->restartEpoch !=
                    telemetry->restartEpoch)
            {
                emitterIt->second->elapsedSeconds = 0.0f;
                emitterIt->second->fixedStepAccumulator = 0.0f;
                emitterIt->second->fixedStepIndex = 0;
                emitterIt->second->prewarm = true;
                emitterIt->second->needsClear = true;
                emitterIt->second->culled = false;
                emitterIt->second->restartEpoch =
                    telemetry->restartEpoch;
            }
            ComputeEmitterBounds(
                emitter,
                model,
                emitterIt->second->worldCenter,
                emitterIt->second->boundsRadius,
                emitterIt->second->meshLocalBoundsRadius);
        }
    }

    for (auto emitterIt = emitters.begin();
         emitterIt != emitters.end();)
    {
        if (desired.find(emitterIt->first) == desired.end())
        {
            RetireEmitterResources(
                std::move(emitterIt->second));
            emitterIt = emitters.erase(emitterIt);
        }
        else
        {
            ++emitterIt;
        }
    }
    // decal 回读缓存与活跃 decal 池集合同步；已有记录在池重建期间保留，
    // 新池先以空 vector 出现，确保 CPU fallback 不会因为 0 存活粒子误接管。
    for (auto cacheIt = decalGpuInstances.begin();
         cacheIt != decalGpuInstances.end();)
    {
        const auto emitterIt = emitters.find(cacheIt->first);
        if (emitterIt == emitters.end() ||
            emitterIt->second->asset.renderMode != "decal")
        {
            cacheIt = decalGpuInstances.erase(cacheIt);
        }
        else
        {
            ++cacheIt;
        }
    }
    for (const auto& [key, emitterPtr] : emitters)
    {
        if (emitterPtr->asset.renderMode == "decal")
        {
            decalGpuInstances.try_emplace(key);
        }
    }
    RebuildIesLut();
    BindParentSpawnSources();
    AssignParticleLightSlots();
    // 发射器集合稳定后再决定全局透明排序池的成员/纹理槽位；签名不变时
    // 直接复用，不在每帧创建 GPU 资源。
    SynchronizeGlobalSort();
}

void ParticleRenderer::RestartParticleSystems(
    const std::string& sceneObjectName)
{
    for (auto& entry : emitters)
    {
        if (!ParticleEmitterKeyMatchesScene(
                entry.first,
                sceneObjectName))
        {
            continue;
        }
        EmitterResources& emitter = *entry.second;
        // elapsedSeconds 决定 burst 周期与 flipbook 系统时间；归零后从
        // 第一帧重新开始。prewarm/needsClear 让下一帧重建整池初始状态。
        emitter.elapsedSeconds = 0.0f;
        emitter.fixedStepAccumulator = 0.0f;
        emitter.fixedStepIndex = 0;
        emitter.prewarm = true;
        emitter.needsClear = true;
        emitter.culled = false;
    }
}

ParticleRenderer::EmitterResources*
ParticleRenderer::FindEmitterBySceneAndName(
    const std::string& sceneObjectName,
    const std::string& emitterName)
{
    for (const auto& [key, emitter] : emitters)
    {
        if (!ParticleEmitterKeyMatchesScene(key, sceneObjectName))
        {
            continue;
        }
        if (emitter->asset.name == emitterName)
        {
            return emitter.get();
        }
    }
    return nullptr;
}

bool ParticleRenderer::RequestParticleKill(
    const std::string& sceneObjectName,
    const std::string& emitterName,
    uint32_t particleIndex,
    std::string& outError)
{
    outError.clear();
    if (!initialized)
    {
        outError = "Particle renderer is not initialized.";
        return false;
    }
    EmitterResources* emitter =
        FindEmitterBySceneAndName(sceneObjectName, emitterName);
    if (emitter == nullptr)
    {
        outError =
            "Particle emitter was not found: " + sceneObjectName +
            "." + emitterName;
        return false;
    }
    if (emitter->asset.renderMode == "beam")
    {
        outError =
            "Particle emitter has no simulated particle pool: " +
            emitterName;
        return false;
    }
    if (!emitter->visible || !emitter->enabled)
    {
        outError =
            "Particle emitter is hidden or disabled: " + emitterName;
        return false;
    }
    if (particleIndex >= emitter->asset.maxParticles)
    {
        outError =
            "Particle index " + std::to_string(particleIndex) +
            " is outside [0, " +
            std::to_string(emitter->asset.maxParticles) + ").";
        return false;
    }
    uint32_t pendingForKey = 0;
    for (const PendingParticleRequest& pending :
         pendingParticleRequests)
    {
        if (pending.emitterKey == emitter->key)
        {
            ++pendingForKey;
        }
    }
    if (pendingForKey >= 16u)
    {
        outError =
            "Particle emitter has too many pending runtime requests.";
        return false;
    }
    PendingParticleRequest request;
    request.emitterKey = emitter->key;
    request.kill = true;
    request.particleIndex = particleIndex;
    pendingParticleRequests.push_back(std::move(request));
    return true;
}

bool ParticleRenderer::RequestParticleSpawnAt(
    const std::string& sceneObjectName,
    const std::string& emitterName,
    const Eigen::Vector3f& worldPosition,
    float lifetimeSeconds,
    std::string& outError)
{
    outError.clear();
    if (!initialized)
    {
        outError = "Particle renderer is not initialized.";
        return false;
    }
    EmitterResources* emitter =
        FindEmitterBySceneAndName(sceneObjectName, emitterName);
    if (emitter == nullptr)
    {
        outError =
            "Particle emitter was not found: " + sceneObjectName +
            "." + emitterName;
        return false;
    }
    if (emitter->asset.renderMode == "beam")
    {
        outError =
            "Particle emitter has no simulated particle pool: " +
            emitterName;
        return false;
    }
    if (!emitter->visible || !emitter->enabled)
    {
        outError =
            "Particle emitter is hidden or disabled: " + emitterName;
        return false;
    }
    if (emitter->asset.maxParticles == 0)
    {
        outError =
            "Particle emitter has a zero-capacity pool: " +
            emitterName;
        return false;
    }
    if (!worldPosition.allFinite() ||
        !std::isfinite(lifetimeSeconds))
    {
        outError = "Particle spawn position/lifetime is not finite.";
        return false;
    }
    float lifetime = lifetimeSeconds;
    if (lifetime <= 0.0f)
    {
        lifetime = emitter->asset.lifetimeMax;
    }
    if (!(lifetime > 0.0f))
    {
        outError =
            "Particle spawn requires a positive lifetime or an "
            "emitter lifetime range.";
        return false;
    }
    uint32_t pendingForKey = 0;
    for (const PendingParticleRequest& pending :
         pendingParticleRequests)
    {
        if (pending.emitterKey == emitter->key)
        {
            ++pendingForKey;
        }
    }
    if (pendingForKey >= 16u)
    {
        outError =
            "Particle emitter has too many pending runtime requests.";
        return false;
    }
    PendingParticleRequest request;
    request.emitterKey = emitter->key;
    request.kill = false;
    request.particleIndex =
        emitter->nextRuntimeSpawnSlot++ %
        emitter->asset.maxParticles;
    request.position = worldPosition;
    request.lifetime = lifetime;
    pendingParticleRequests.push_back(std::move(request));
    return true;
}

std::vector<ParticleLightShadowCandidate>
ParticleRenderer::GetParticleLightShadowCandidates() const
{
    std::vector<ParticleLightShadowCandidate> candidates;
    for (const auto& [key, emitter] : emitters)
    {
        if (emitter->asset.renderMode != "light" ||
            !emitter->asset.light.castShadows ||
            emitter->culled || !emitter->visible || !emitter->enabled)
        {
            continue;
        }
        candidates.push_back({
            key,
            emitter->worldCenter,
            std::max(emitter->asset.light.radius, emitter->boundsRadius),
            true});
    }
    return candidates;
}

void ParticleRenderer::SetParticleLightShadowBinding(
    const std::string& emitterKey,
    uint32_t slice,
    float farCm,
    float biasCm,
    bool castShadows)
{
    const auto found = emitters.find(emitterKey);
    if (found == emitters.end())
    {
        return;
    }
    EmitterResources& emitter = *found->second;
    emitter.particleLightShadowSlice = slice;
    emitter.particleLightShadowFar = farCm;
    emitter.particleLightShadowBias = biasCm;
    emitter.particleLightCastShadows = castShadows;
}

void ParticleRenderer::ClearParticleLightShadowBindings()
{
    for (auto& [key, emitter] : emitters)
    {
        emitter->particleLightShadowSlice = 0xFFFFFFFFu;
        emitter->particleLightShadowFar = 0.0f;
        emitter->particleLightShadowBias =
            emitter->asset.light.shadowBias;
        emitter->particleLightCastShadows = false;
    }
}

void ParticleRenderer::SetShadowCascadeData(
    const ParticleShadowCascadeData& data)
{
    shadowCascadeData = data;
}

bool ParticleRenderer::DrainParticleEvents(
    RendererBackendVulkan& readbackBackend,
    uint32_t maxPerEmitter,
    std::vector<ParticleEventBatch>& outBatches,
    std::string& outError)
{
    outBatches.clear();
    if (!initialized || backend != &readbackBackend)
    {
        outError =
            "Particle renderer is not initialized for this backend.";
        return false;
    }
    if (emitters.empty())
    {
        outError = "No particle emitters are available for event drain.";
        return false;
    }

    maxPerEmitter = std::min(
        maxPerEmitter, kParticleEventCapacity);
    const vk::DeviceSize counterBytes = sizeof(ParticleEventCounters);
    const vk::DeviceSize recordBytes =
        maxPerEmitter > 0
        ? sizeof(ParticleEventRecord) *
            static_cast<vk::DeviceSize>(maxPerEmitter)
        : sizeof(ParticleEventRecord);

    for (const auto& [key, emitterPtr] : emitters)
    {
        const EmitterResources& emitter = *emitterPtr;
        // 冻结 emitter 的计数在 RecordSimulation 中已清零；这里直接跳过，
        // 避免 cull 边界上的旧记录被重复派发。
        if (emitter.culled)
        {
            continue;
        }
        if (!emitter.counters.buffer || !emitter.events.buffer)
        {
            outError = "Particle emitter has no event buffers: " + key;
            return false;
        }

        vk::Buffer counterReadback;
        vk::DeviceMemory counterMemory;
        vk::Buffer eventReadback;
        vk::DeviceMemory eventMemory;
        try
        {
            std::tie(counterReadback, counterMemory) =
                readbackBackend.CreateBuffer(
                    counterBytes,
                    vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "ParticleEventCountersReadback:" + key);
            std::tie(eventReadback, eventMemory) =
                readbackBackend.CreateBuffer(
                    recordBytes,
                    vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "ParticleEventsReadback:" + key);

            vk::CommandBuffer commandBuffer =
                readbackBackend.BeginSingleTimeCommands();
            vk::BufferMemoryBarrier toTransfer;
            toTransfer
                .setSrcAccessMask(
                    vk::AccessFlagBits::eShaderRead |
                    vk::AccessFlagBits::eShaderWrite)
                .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED);
            toTransfer.setBuffer(emitter.counters.buffer);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                emitter.counters.buffer,
                counterReadback,
                vk::BufferCopy(0, 0, counterBytes));

            toTransfer.setBuffer(emitter.events.buffer);
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {},
                nullptr,
                toTransfer,
                nullptr);
            commandBuffer.copyBuffer(
                emitter.events.buffer,
                eventReadback,
                vk::BufferCopy(0, 0, recordBytes));
            readbackBackend.EndSingleTimeCommands(commandBuffer);

            const ParticleEventCounters* counters =
                static_cast<const ParticleEventCounters*>(
                    readbackBackend.MapMemory(counterMemory, counterBytes));
            const ParticleEventRecord* records =
                static_cast<const ParticleEventRecord*>(
                    readbackBackend.MapMemory(eventMemory, recordBytes));
            if (counters == nullptr || records == nullptr)
            {
                readbackBackend.UnmapMemory(counterMemory);
                readbackBackend.UnmapMemory(eventMemory);
                readbackBackend.DestroyBuffer(
                    counterReadback, counterMemory);
                readbackBackend.DestroyBuffer(
                    eventReadback, eventMemory);
                outError = "Particle event readback map failed: " + key;
                return false;
            }

            ParticleEventBatch batch;
            batch.emitterKey = key;
            batch.counters = *counters;
            const uint32_t recordCount = std::min(
                std::min(maxPerEmitter, kParticleEventCapacity),
                counters->events);
            batch.records.assign(records, records + recordCount);
            outBatches.push_back(std::move(batch));

            readbackBackend.UnmapMemory(counterMemory);
            readbackBackend.UnmapMemory(eventMemory);
            readbackBackend.DestroyBuffer(
                counterReadback, counterMemory);
            readbackBackend.DestroyBuffer(
                eventReadback, eventMemory);
        }
        catch (const std::exception& exception)
        {
            outError =
                "Particle event drain failed for '" + key +
                "': " + exception.what();
            return false;
        }
    }
    return true;
}

void ParticleRenderer::EnsureSpawnSourceBindings(
    EmitterResources& emitter)
{
    // 只在 CreateDescriptors 重新分配 computeSets（资源重建）或父发射器
    // 选择变化时写入；每帧重复写会让 descriptor 校验失去意义。
    if (!emitter.spawnSourceBindingsDirty)
    {
        return;
    }
    const uint32_t imageCount = backend->GetSwapchainImageCount();
    for (uint32_t imageIndex = 0;
         imageIndex < imageCount &&
         imageIndex < emitter.computeSets.size();
         ++imageIndex)
    {
        const vk::DescriptorBufferInfo spawnSourceInfo(
            emitter.spawnSource.buffer,
            0,
            VK_WHOLE_SIZE);
        const vk::DescriptorBufferInfo parentSpawnEventInfo(
            emitter.spawnEventSource
                ? emitter.spawnEventSource
                : emitter.spawnEvents.buffer,
            0,
            VK_WHOLE_SIZE);
        const vk::DescriptorBufferInfo spawnEventOutInfo(
            emitter.spawnEvents.buffer,
            0,
            VK_WHOLE_SIZE);
        vk::WriteDescriptorSet spawnSourceWrite;
        spawnSourceWrite
            .setDstSet(emitter.computeSets.at(imageIndex))
            .setDstBinding(6)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setBufferInfo(spawnSourceInfo);
        vk::WriteDescriptorSet parentSpawnWrite;
        parentSpawnWrite
            .setDstSet(emitter.computeSets.at(imageIndex))
            .setDstBinding(7)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setBufferInfo(parentSpawnEventInfo);
        vk::WriteDescriptorSet spawnEventOutWrite;
        spawnEventOutWrite
            .setDstSet(emitter.computeSets.at(imageIndex))
            .setDstBinding(8)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setBufferInfo(spawnEventOutInfo);
        std::vector<vk::WriteDescriptorSet> writes{
            spawnSourceWrite,
            parentSpawnWrite,
            spawnEventOutWrite};
        backend->UpdateDescriptorSets(writes);
    }
    emitter.spawnSourceBindingsDirty = false;
}

void ParticleRenderer::BindParentSpawnSources()
{
    // 先算每个 emitter 的 set 0 binding 7 目标：默认读自己的当前帧事件
    // 缓冲，子 emitter 改读父 emitter 的缓冲。只有目标真的变化时才置
    // dirty，避免每帧重写仍在上一帧 command buffer 里的 descriptor set
    // （VUID-vkUpdateDescriptorSets-None-03047）。
    std::unordered_map<std::string, vk::Buffer> desiredSources;
    desiredSources.reserve(emitters.size());
    for (auto& [key, emitter] : emitters)
    {
        emitter->emitSpawnEvents = false;
        desiredSources.emplace(key, emitter->spawnEvents.buffer);
    }
    for (auto& [childKey, child] : emitters)
    {
        if (child->asset.spawnSource.mode != "parentEmitter")
        {
            continue;
        }
        const size_t separator = childKey.find_last_of('|');
        if (separator == std::string::npos)
        {
            continue;
        }
        const std::string parentKey =
            childKey.substr(0, separator + 1) +
            EscapeParticleEmitterKeySegment(
                child->asset.spawnSource.parentEmitter);
        const auto parentIt = emitters.find(parentKey);
        if (parentIt == emitters.end())
        {
            continue;
        }
        parentIt->second->emitSpawnEvents = true;
        desiredSources[childKey] = parentIt->second->spawnEvents.buffer;
    }
    // 父发射器选择完成后统一写 spawn 源/父事件/事件输出三个 binding。
    for (auto& [key, emitter] : emitters)
    {
        const vk::Buffer desired = desiredSources.at(key);
        if (emitter->spawnEventSource != desired)
        {
            emitter->spawnEventSource = desired;
            emitter->spawnSourceBindingsDirty = true;
        }
        EnsureSpawnSourceBindings(*emitter);
    }
}

} // namespace VL
