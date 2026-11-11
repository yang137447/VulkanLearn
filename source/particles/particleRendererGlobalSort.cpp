#include "particles/particleRendererInternal.h"

namespace VL
{

void ParticleRenderer::DestroyGlobalSortPool()
{
    // 全局池可能在上一帧仍被 GPU 引用；重建/销毁只发生在发射器集合或
    // 纹理变化时，用一次 WaitIdle 换取安全，而不是每帧等待。
    if (backend != nullptr && globalSortPool.active)
    {
        backend->WaitIdle();
    }
    if (backend != nullptr)
    {
        if (globalSortPool.descriptorPool)
        {
            backend->DestroyDescriptorPool(
                globalSortPool.descriptorPool);
        }
        for (GpuBuffer& buffer : globalSortPool.records)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& buffer : globalSortPool.pairsA)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& buffer : globalSortPool.pairsB)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& buffer : globalSortPool.counters)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& buffer : globalSortPool.indirectArgs)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& buffer : globalSortPool.debugCounters)
            DestroyBuffer(*backend, buffer);
        for (GpuBuffer& uniform : globalSortPool.uniforms)
        {
            DestroyBuffer(*backend, uniform);
        }
        globalSortPool.gpuTimer.Shutdown(*backend);
    }
    globalSortPool = GlobalSortPool{};
}

bool ParticleRenderer::IsGlobalSortEmitter(
    const std::string& key) const
{
    if (!globalSortPool.active)
    {
        return false;
    }
    return std::find(
        globalSortPool.emitterKeys.begin(),
        globalSortPool.emitterKeys.end(),
        key) != globalSortPool.emitterKeys.end();
}

void ParticleRenderer::RebuildIesLut()
{
    if (backend == nullptr)
    {
        return;
    }
    std::vector<std::string> keys;
    keys.reserve(emitters.size());
    for (const auto& [key, emitter] : emitters)
    {
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    std::vector<std::vector<float>> profiles;
    for (const std::string& key : keys)
    {
        auto& emitter = *emitters.at(key);
        emitter.iesProfileIndex = 0;
        const auto& profile = emitter.asset.light.iesProfile;
        if (profile.empty())
        {
            continue;
        }
        auto found = std::find_if(
            profiles.begin(),
            profiles.end(),
            [&](const std::vector<float>& candidate)
            {
                return candidate == profile;
            });
        if (found == profiles.end())
        {
            if (profiles.size() >= 15)
            {
                continue;
            }
            profiles.push_back(profile);
            found = profiles.end() - 1;
        }
        emitter.iesProfileIndex = static_cast<uint32_t>(
            std::distance(profiles.begin(), found) + 1);
    }
    uint64_t signature = 14695981039346656037ull;
    const auto mix = [&](uint32_t value)
    {
        signature ^= value;
        signature *= 1099511628211ull;
    };
    for (const auto& profile : profiles)
    {
        for (float sample : profile)
        {
            mix(static_cast<uint32_t>(
                std::llround(sample * 1024.0f)));
        }
        mix(0xFFFFFFFFu);
    }
    if (signature == iesLutSignature)
    {
        return;
    }
    iesLutSignature = signature;
    HostImage image;
    image.width = 64;
    image.height = 16;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA8_UNORM;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = 64 * 4;
    image.data.assign(64 * 16 * 4, 255);
    for (size_t profileIndex = 0;
         profileIndex < profiles.size();
         ++profileIndex)
    {
        const auto& profile = profiles[profileIndex];
        for (uint32_t x = 0; x < 64; ++x)
        {
            const float u = (x + 0.5f) / 64.0f;
            const float source =
                u * static_cast<float>(profile.size() - 1);
            const size_t low = static_cast<size_t>(source);
            const size_t high = std::min(
                low + 1, profile.size() - 1);
            const float weight = source - static_cast<float>(low);
            const float value =
                profile[low] * (1.0f - weight) +
                profile[high] * weight;
            const uint8_t byte = static_cast<uint8_t>(std::clamp(
                value / 64.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
            const size_t index =
                ((profileIndex + 1) * 64 + x) * 4;
            image.data[index + 0] = byte;
            image.data[index + 1] = byte;
            image.data[index + 2] = byte;
            image.data[index + 3] = 255;
        }
    }
    Texture::CreateDesc desc;
    desc.semantic = HostImage::TextureSemantic::Data;
    desc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    desc.generateMipmaps = false;
    desc.filter = vk::Filter::eLinear;
    desc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    desc.debugName = "ParticleIesLut";
    iesLutTexture = std::make_shared<Texture>(
        *backend, image, desc);

    // 纹理对象换了，已有 descriptor 必须重写。
    const uint32_t imageCount = backend->GetSwapchainImageCount();
    const vk::DescriptorImageInfo iesInfo =
        iesLutTexture->GetDescriptorInfo();
    for (auto& [key, emitter] : emitters)
    {
        for (uint32_t imageIndex = 0;
             imageIndex < emitter->graphicsSets.size() &&
             imageIndex < imageCount;
             ++imageIndex)
        {
            vk::WriteDescriptorSet write;
            write
                .setDstSet(emitter->graphicsSets.at(imageIndex))
                .setDstBinding(14)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(iesInfo);
            backend->UpdateDescriptorSets({write});
        }
    }
    for (uint32_t imageIndex = 0;
         imageIndex < globalSortPool.drawSets.size() &&
         imageIndex < imageCount;
         ++imageIndex)
    {
        vk::WriteDescriptorSet write;
        write
            .setDstSet(globalSortPool.drawSets.at(imageIndex))
            .setDstBinding(14)
            .setDescriptorCount(1)
            .setDescriptorType(
                vk::DescriptorType::eCombinedImageSampler)
            .setImageInfo(iesInfo);
        backend->UpdateDescriptorSets({write});
    }
}

void ParticleRenderer::AssignParticleLightSlots()
{
    std::vector<std::pair<std::string, uint32_t>> requests;
    requests.reserve(emitters.size());
    for (const auto& [key, emitterPtr] : emitters)
    {
        if (emitterPtr->asset.renderMode == "light")
        {
            requests.emplace_back(
                key,
                std::min(
                    emitterPtr->asset.maxParticles,
                    kMaxParticleLights));
        }
    }
    const std::vector<ParticleLightSlot> slots =
        AllocateParticleLightSlots(std::move(requests));
    for (const ParticleLightSlot& slot : slots)
    {
        const auto emitter = emitters.find(slot.key);
        if (emitter == emitters.end())
        {
            continue;
        }
        emitter->second->lightSlotBase = slot.base;
        emitter->second->lightSlotCapacity = slot.capacity;
    }
}

void ParticleRenderer::SynchronizeGlobalSort()
{
    if (!initialized || backend == nullptr)
    {
        return;
    }

    struct Candidate
    {
        std::string key;
        std::string textureKey;
        uint32_t maxParticles = 0;
        bool ribbon = false;
        uint32_t segments = 1;
    };
    std::vector<Candidate> candidates;
    uint64_t totalCapacity = 0;
    for (const auto& [key, emitterPtr] : emitters)
    {
        const EmitterResources& emitter = *emitterPtr;
        if (!emitter.visible || !emitter.enabled)
        {
            continue;
        }
        // 全局排序接纳 alpha billboard 与 alpha ribbon；mesh 仍保留
        // 逐发射器排序。ribbon shadow 始终走逐发射器 caster。
        const bool billboard =
            emitter.asset.renderMode == "billboard";
        const bool ribbon =
            emitter.asset.renderMode == "ribbon";
        if ((!billboard && !ribbon) ||
            !emitter.asset.sortParticles ||
            emitter.asset.additive)
        {
            continue;
        }
        if (billboard &&
            // 全局 gather 记录不携带逐 emitter 阴影开关；需要 CSM 的
            // billboard 留在逐 emitter 路径，避免静默丢失作者设置。
            emitter.asset.billboardShadows)
        {
            continue;
        }
        const uint32_t segments = ribbon
            ? ClampParticleRibbonSegments(
                emitter.asset.ribbonSegments)
            : 1u;
        const uint64_t emitterCapacity =
            static_cast<uint64_t>(emitter.asset.maxParticles) *
            segments;
        totalCapacity += emitterCapacity;
        if (totalCapacity > kParticleGlobalSortCapacity)
        {
            DestroyGlobalSortPool();
            return;
        }
        candidates.push_back(Candidate{
            key,
            emitter.textureSourceKey,
            emitter.asset.maxParticles,
            ribbon,
            segments});
    }
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& left, const Candidate& right)
        {
            return left.key < right.key;
        });

    std::vector<std::string> emitterKeys;
    std::vector<uint32_t> textureSlots;
    std::vector<std::shared_ptr<Texture>> textures;
    std::unordered_map<std::string, uint32_t> slotByTexture;
    std::string signature;
    for (const Candidate& candidate : candidates)
    {
        const auto slot = slotByTexture.find(candidate.textureKey);
        uint32_t textureSlot = 0;
        if (slot == slotByTexture.end())
        {
            if (textures.size() >= kParticleGlobalTextureSlots)
            {
                DestroyGlobalSortPool();
                return;
            }
            textureSlot = static_cast<uint32_t>(textures.size());
            slotByTexture.emplace(candidate.textureKey, textureSlot);
            const auto emitter = emitters.find(candidate.key);
            textures.push_back(
                emitter != emitters.end() && emitter->second->texture
                ? emitter->second->texture
                : defaultTexture);
        }
        else
        {
            textureSlot = slot->second;
        }
        emitterKeys.push_back(candidate.key);
        textureSlots.push_back(textureSlot);
        signature += candidate.key + "#" + candidate.textureKey +
            "#" + std::to_string(candidate.maxParticles) +
            "#" + (candidate.ribbon ? "ribbon" : "billboard") +
            "#" + std::to_string(candidate.segments) + ";";
    }
    // 场景灯/IBL descriptor 换代也必须重建全局 draw set；否则全局池会
    // 继续引用旧 light SSBO 或旧 prefiltered cubemap。
    signature += "lighting#" +
        std::to_string(sceneLightingGeneration) + ";";

    if (emitterKeys.empty())
    {
        DestroyGlobalSortPool();
        return;
    }
    // 失败签名也缓存，避免每帧重复创建/告警；发射器集合或纹理变化后
    // 签名不同才会重试。
    if (globalSortPool.signature == signature)
    {
        return;
    }

    const uint32_t sortCapacity =
        GlobalSortCapacity(totalCapacity);
    if (!CreateGlobalSortPool(
            emitterKeys,
            textureSlots,
            textures,
            sortCapacity))
    {
        globalSortPool.signature = signature;
        const std::string message =
            "Global particle sort disabled: pool creation failed";
        ProcessLog().Append(
            LogSeverity::Warning, "Particles", message);
        std::cout << "[Particles][Warning] " << message << std::endl;
        DestroyGlobalSortPool();
        globalSortPool.signature = signature;
        return;
    }
    globalSortPool.signature = signature;
    const std::string message =
        "Global particle sort active: emitters=" +
        std::to_string(emitterKeys.size()) +
        " capacity=" + std::to_string(sortCapacity) +
        " textures=" + std::to_string(textures.size());
    ProcessLog().Append(LogSeverity::Info, "Particles", message);
    std::cout << "[Particles][Info] " << message << std::endl;
}

bool ParticleRenderer::CreateGlobalSortPool(
    const std::vector<std::string>& emitterKeys,
    const std::vector<uint32_t>& textureSlots,
    const std::vector<std::shared_ptr<Texture>>& textures,
    uint32_t capacity)
{
    DestroyGlobalSortPool();
    if (backend == nullptr || emitterKeys.empty() ||
        emitterKeys.size() != textureSlots.size() ||
        !globalGatherPipeline || !globalSortInitPipeline ||
        !globalDrawPipeline || !sortPipeline ||
        !finalizeCountPipeline)
    {
        std::cout << "[Particles][Warning] Global particle sort "
            "precondition failed: pipelines="
                  << (globalGatherPipeline ? 1 : 0)
                  << (globalSortInitPipeline ? 1 : 0)
                  << (globalDrawPipeline ? 1 : 0)
                  << (sortPipeline ? 1 : 0)
                  << (finalizeCountPipeline ? 1 : 0)
                  << std::endl;
        return false;
    }

    const uint32_t imageCount = backend->GetSwapchainImageCount();
    const uint32_t emitterCount =
        static_cast<uint32_t>(emitterKeys.size());
    const vk::DeviceSize recordBytes =
        static_cast<vk::DeviceSize>(capacity) *
        kParticleGlobalRecordVec4Count * sizeof(Eigen::Vector4f);
    const vk::DeviceSize pairBytes =
        static_cast<vk::DeviceSize>(capacity) * sizeof(Eigen::Vector4f);

    globalSortPool.capacity = capacity;
    globalSortPool.emitterCount = emitterCount;
    globalSortPool.emitterKeys = emitterKeys;
    globalSortPool.textures = textures;
    for (size_t index = 0; index < emitterKeys.size(); ++index)
    {
        globalSortPool.textureSlotByKey.emplace(
            emitterKeys[index], textureSlots[index]);
    }

    const auto createStorage = [this](
        vk::DeviceSize bytes,
        const std::string& name)
    {
        return backend->CreateBuffer(
            bytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            name);
    };
    globalSortPool.records.resize(imageCount);
    globalSortPool.pairsA.resize(imageCount);
    globalSortPool.pairsB.resize(imageCount);
    globalSortPool.counters.resize(imageCount);
    globalSortPool.indirectArgs.resize(imageCount);
    globalSortPool.debugCounters.resize(imageCount);
    for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
    {
        const std::string suffix = ":" + std::to_string(imageIndex);
        std::tie(globalSortPool.records[imageIndex].buffer,
            globalSortPool.records[imageIndex].memory) =
            createStorage(recordBytes, "ParticleGlobalRecords" + suffix);
        std::tie(globalSortPool.pairsA[imageIndex].buffer,
            globalSortPool.pairsA[imageIndex].memory) =
            createStorage(pairBytes, "ParticleGlobalPairsA" + suffix);
        std::tie(globalSortPool.pairsB[imageIndex].buffer,
            globalSortPool.pairsB[imageIndex].memory) =
            createStorage(pairBytes, "ParticleGlobalPairsB" + suffix);

        const auto counterAllocation = backend->CreateBuffer(
            sizeof(ParticleEventCounters),
            vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eTransferDst |
                vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleGlobalCounters" + suffix);
        globalSortPool.counters[imageIndex].buffer =
            counterAllocation.first;
        globalSortPool.counters[imageIndex].memory =
            counterAllocation.second;
        void* mappedCounters = backend->MapMemory(
            globalSortPool.counters[imageIndex].memory,
            sizeof(ParticleEventCounters));
        if (mappedCounters == nullptr)
        {
            return false;
        }
        ParticleEventCounters initialCounters;
        initialCounters.capacity = capacity;
        std::memcpy(
            mappedCounters, &initialCounters, sizeof(initialCounters));
        backend->UnmapMemory(globalSortPool.counters[imageIndex].memory);

        const auto indirectAllocation = backend->CreateBuffer(
            sizeof(vk::DrawIndirectCommand),
            vk::BufferUsageFlagBits::eIndirectBuffer |
                vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleGlobalIndirect" + suffix);
        globalSortPool.indirectArgs[imageIndex].buffer =
            indirectAllocation.first;
        globalSortPool.indirectArgs[imageIndex].memory =
            indirectAllocation.second;
        void* mappedIndirect = backend->MapMemory(
            globalSortPool.indirectArgs[imageIndex].memory,
            sizeof(vk::DrawIndirectCommand));
        if (mappedIndirect == nullptr)
        {
            return false;
        }
        const vk::DrawIndirectCommand command(6u, capacity, 0u, 0u);
        std::memcpy(mappedIndirect, &command, sizeof(command));
        backend->UnmapMemory(
            globalSortPool.indirectArgs[imageIndex].memory);

        const auto debugCounterAllocation = backend->CreateBuffer(
            sizeof(ParticleOverdrawCounters),
            vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eTransferDst |
                vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            "ParticleGlobalOverdraw" + suffix);
        globalSortPool.debugCounters[imageIndex].buffer =
            debugCounterAllocation.first;
        globalSortPool.debugCounters[imageIndex].memory =
            debugCounterAllocation.second;
    }
    globalSortPool.gpuTimer.Initialize(*backend);

    globalSortPool.uniforms.resize(imageCount);
    for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
    {
        const auto uniformAllocation = backend->CreateBuffer(
            sizeof(FrameUniforms),
            vk::BufferUsageFlagBits::eUniformBuffer |
                vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleGlobalUniforms:" + std::to_string(imageIndex));
        globalSortPool.uniforms[imageIndex].buffer =
            uniformAllocation.first;
        globalSortPool.uniforms[imageIndex].memory =
            uniformAllocation.second;
        globalSortPool.uniforms[imageIndex].mapped =
            backend->MapMemory(
                uniformAllocation.second, sizeof(FrameUniforms));
        if (globalSortPool.uniforms[imageIndex].mapped == nullptr)
        {
            return false;
        }
    }

    const uint32_t sortPassCount =
        ParticleSortDispatchCount(capacity);
    globalSortPool.finalUsesA = (sortPassCount % 2u) == 0u;

    const std::vector<vk::DescriptorSetLayout> gatherLayouts(
        imageCount * emitterCount,
        globalGatherPipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> sortInitLayouts(
        imageCount,
        globalSortInitPipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> sortLayouts(
        imageCount,
        sortPipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> finalizeLayouts(
        imageCount,
        finalizeCountPipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> drawLayouts(
        imageCount,
        globalDrawPipeline->GetDescriptorSetLayouts().at(0));

    std::vector<vk::DescriptorPoolSize> poolSizes{
        {vk::DescriptorType::eUniformBuffer,
         imageCount * (emitterCount + 6)},
        {vk::DescriptorType::eStorageBuffer,
            imageCount * (emitterCount * 3u + 16u)},
        {vk::DescriptorType::eCombinedImageSampler,
            imageCount * (kParticleGlobalTextureSlots + 4u)}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount * (emitterCount + 6));
    globalSortPool.descriptorPool = backend->CreateDescriptorPool(
        poolInfo, "ParticleGlobalDescriptors");

    try
    {
        vk::DescriptorSetAllocateInfo allocate;
        allocate.setDescriptorPool(globalSortPool.descriptorPool);
        allocate.setSetLayouts(gatherLayouts);
        globalSortPool.gatherSets.resize(imageCount * emitterCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.gatherSets);

        allocate.setSetLayouts(sortInitLayouts);
        globalSortPool.sortInitSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.sortInitSets);
        allocate.setSetLayouts(sortLayouts);
        globalSortPool.sortAtoBSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.sortAtoBSets);
        globalSortPool.sortBtoASets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.sortBtoASets);
        allocate.setSetLayouts(finalizeLayouts);
        globalSortPool.finalizeSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.finalizeSets);
        allocate.setSetLayouts(drawLayouts);
        globalSortPool.drawSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate, globalSortPool.drawSets);

        for (uint32_t imageIndex = 0;
             imageIndex < imageCount;
             ++imageIndex)
        {
            const vk::DescriptorBufferInfo recordInfo(
                globalSortPool.records[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo pairAInfo(
                globalSortPool.pairsA[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo pairBInfo(
                globalSortPool.pairsB[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo counterInfo(
                globalSortPool.counters[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo indirectInfo(
                globalSortPool.indirectArgs[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo debugCounterInfo(
                globalSortPool.debugCounters[imageIndex].buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo uniformInfo(
                globalSortPool.uniforms[imageIndex].buffer,
                0,
                sizeof(FrameUniforms));
            for (uint32_t emitterIndex = 0;
                 emitterIndex < emitterCount;
                 ++emitterIndex)
            {
                const auto emitter = emitters.find(
                    emitterKeys[emitterIndex]);
                if (emitter == emitters.end() ||
                    emitter->second->frameUniforms.size() <= imageIndex)
                {
                    throw std::runtime_error(
                        "Global particle gather lost an emitter");
                }
                const vk::DescriptorBufferInfo emitterUniform(
                    emitter->second->frameUniforms
                        .at(imageIndex).buffer,
                    0,
                    sizeof(FrameUniforms));
                const vk::DescriptorBufferInfo particleInfo(
                    emitter->second->particles.buffer,
                    0,
                    VK_WHOLE_SIZE);
                vk::WriteDescriptorSet uniformWrite;
                uniformWrite
                    .setDstSet(globalSortPool.gatherSets.at(
                        imageIndex * emitterCount + emitterIndex))
                    .setDstBinding(0)
                    .setDescriptorCount(1)
                    .setDescriptorType(
                        vk::DescriptorType::eUniformBuffer)
                    .setBufferInfo(emitterUniform);
                vk::WriteDescriptorSet particleWrite = uniformWrite;
                particleWrite
                    .setDstBinding(1)
                    .setDescriptorType(
                        vk::DescriptorType::eStorageBuffer)
                    .setBufferInfo(particleInfo);
                vk::WriteDescriptorSet recordWrite = particleWrite;
                recordWrite
                    .setDstBinding(2)
                    .setBufferInfo(recordInfo);
                vk::WriteDescriptorSet counterWrite = particleWrite;
                counterWrite
                    .setDstBinding(3)
                    .setBufferInfo(counterInfo);
                backend->UpdateDescriptorSets(
                    {uniformWrite,
                     particleWrite,
                     recordWrite,
                     counterWrite});
            }

            vk::WriteDescriptorSet uniformWrite;
            uniformWrite
                .setDstSet(globalSortPool.sortInitSets.at(imageIndex))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uniformInfo);
            vk::WriteDescriptorSet recordWrite = uniformWrite;
            recordWrite
                .setDstBinding(1)
                .setDescriptorType(
                    vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(recordInfo);
            vk::WriteDescriptorSet pairAWrite = recordWrite;
            pairAWrite
                .setDstBinding(2)
                .setBufferInfo(pairAInfo);
            vk::WriteDescriptorSet counterWrite = recordWrite;
            counterWrite
                .setDstBinding(3)
                .setBufferInfo(counterInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite, recordWrite, pairAWrite, counterWrite});

            uniformWrite.setDstSet(
                globalSortPool.sortAtoBSets.at(imageIndex));
            vk::WriteDescriptorSet inA = pairAWrite;
            inA.setDstSet(globalSortPool.sortAtoBSets.at(imageIndex))
                .setDstBinding(1)
                .setBufferInfo(pairAInfo);
            vk::WriteDescriptorSet outB = pairAWrite;
            outB.setDstSet(globalSortPool.sortAtoBSets.at(imageIndex))
                .setDstBinding(2)
                .setBufferInfo(pairBInfo);
            backend->UpdateDescriptorSets({uniformWrite, inA, outB});

            uniformWrite.setDstSet(
                globalSortPool.sortBtoASets.at(imageIndex));
            vk::WriteDescriptorSet inB = inA;
            inB.setDstSet(globalSortPool.sortBtoASets.at(imageIndex))
                .setBufferInfo(pairBInfo);
            vk::WriteDescriptorSet outA = outB;
            outA.setDstSet(globalSortPool.sortBtoASets.at(imageIndex))
                .setBufferInfo(pairAInfo);
            backend->UpdateDescriptorSets({uniformWrite, inB, outA});

            uniformWrite.setDstSet(
                globalSortPool.finalizeSets.at(imageIndex));
            vk::WriteDescriptorSet finalizeCounter = counterWrite;
            finalizeCounter.setDstSet(
                    globalSortPool.finalizeSets.at(imageIndex))
                .setDstBinding(0)
                .setBufferInfo(counterInfo);
            vk::WriteDescriptorSet finalizeArgs = counterWrite;
            finalizeArgs.setDstSet(
                    globalSortPool.finalizeSets.at(imageIndex))
                .setDstBinding(1)
                .setBufferInfo(indirectInfo);
            backend->UpdateDescriptorSets(
                {finalizeCounter, finalizeArgs});

            // draw：UBO + records + 排序 pair + 8 槽纹理数组。
            vk::WriteDescriptorSet drawUniform;
            drawUniform
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uniformInfo);
            vk::WriteDescriptorSet drawRecords = recordWrite;
            drawRecords.setDstSet(
                    globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(1)
                .setBufferInfo(recordInfo);
            vk::WriteDescriptorSet drawPairs = pairAWrite;
            drawPairs.setDstSet(
                    globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(2)
                .setBufferInfo(globalSortPool.finalUsesA
                    ? pairAInfo
                    : pairBInfo);
            std::array<vk::DescriptorImageInfo, kParticleGlobalTextureSlots>
                textureInfos;
            for (uint32_t slotIndex = 0;
                 slotIndex < kParticleGlobalTextureSlots;
                 ++slotIndex)
            {
                const std::shared_ptr<Texture>& texture =
                    slotIndex < textures.size()
                    ? textures[slotIndex]
                    : defaultTexture;
                textureInfos[slotIndex] =
                    texture->GetDescriptorInfo();
            }
            vk::WriteDescriptorSet drawTextures;
            drawTextures
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(3)
                .setDescriptorCount(kParticleGlobalTextureSlots)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(textureInfos);
            vk::WriteDescriptorSet drawDebug;
            drawDebug
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(5)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(debugCounterInfo);
            // 全局 alpha 池与逐发射器 billboard 使用同一组场景光照绑定；
            // 全局记录只携带 billboardLightParams，IBL/灯光本身来自这里。
            vk::WriteDescriptorSet drawSceneLight;
            drawSceneLight
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(6)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(SceneLightInfoForImage(imageIndex));
            vk::WriteDescriptorSet drawParticleLight;
            drawParticleLight
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(7)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(ParticleLightInfoForImage(imageIndex));
            vk::WriteDescriptorSet drawEnvironment;
            drawEnvironment
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(8)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(EnvironmentCubeDescriptor());
            vk::WriteDescriptorSet drawBrdfLut;
            drawBrdfLut
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(9)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(BrdfLutDescriptor());
            vk::WriteDescriptorSet drawVolume;
            drawVolume
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(13)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(translucencyVolumeDescriptor);
            vk::WriteDescriptorSet drawIes;
            drawIes
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(14)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(iesLutTexture->GetDescriptorInfo());
            vk::WriteDescriptorSet drawLightGrid;
            drawLightGrid
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(10)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(LightGridInfoForImage(imageIndex));
            vk::WriteDescriptorSet drawLightIndex;
            drawLightIndex
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(11)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(LightIndexInfoForImage(imageIndex));
            vk::WriteDescriptorSet drawLightGridParam;
            drawLightGridParam
                .setDstSet(globalSortPool.drawSets.at(imageIndex))
                .setDstBinding(12)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(LightGridParamInfoForImage(imageIndex));
            backend->UpdateDescriptorSets(
                {drawUniform,
                 drawRecords,
                 drawPairs,
                 drawTextures,
                 drawDebug,
                 drawSceneLight,
                 drawParticleLight,
                 drawEnvironment,
                 drawBrdfLut,
                 drawVolume,
                 drawIes,
                 drawLightGrid,
                 drawLightIndex,
                 drawLightGridParam});
        }
    }
    catch (const std::exception& exception)
    {
        std::cout << "[Particles][Warning] Global particle sort pool "
            "creation threw: " << exception.what() << std::endl;
        DestroyGlobalSortPool();
        return false;
    }
    catch (...)
    {
        std::cout << "[Particles][Warning] Global particle sort pool "
            "creation threw an unknown exception" << std::endl;
        DestroyGlobalSortPool();
        return false;
    }
    globalSortPool.active = true;
    return true;
}

void ParticleRenderer::RecordGlobalGather(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex)
{
    if (!globalSortPool.active || backend == nullptr)
    {
        return;
    }
    const uint32_t imageCount = backend->GetSwapchainImageCount();
    if (globalSortPool.gatherSets.size() <
        imageCount * globalSortPool.emitterCount)
    {
        return;
    }

    vk::MemoryBarrier previousToClear;
    previousToClear
        .setSrcAccessMask(
            vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        previousToClear,
        nullptr,
        nullptr);
    commandBuffer.fillBuffer(
        globalSortPool.counters[imageIndex].buffer,
        0,
        sizeof(uint32_t) * 5u,
        0);
    commandBuffer.fillBuffer(
        globalSortPool.debugCounters[imageIndex].buffer,
        0,
        sizeof(ParticleOverdrawCounters),
        0);
    vk::BufferMemoryBarrier clearToCompute;
    clearToCompute
        .setBuffer(globalSortPool.counters[imageIndex].buffer)
        .setOffset(0)
        .setSize(VK_WHOLE_SIZE)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(
            vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        nullptr,
        clearToCompute,
        nullptr);
    vk::BufferMemoryBarrier debugClearToGraphics;
    debugClearToGraphics
        .setBuffer(globalSortPool.debugCounters[imageIndex].buffer)
        .setOffset(0)
        .setSize(VK_WHOLE_SIZE)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eFragmentShader,
        {},
        nullptr,
        debugClearToGraphics,
        nullptr);
    globalSortPool.lastImageIndex = imageIndex;
    // simulate 刚写过粒子池；gather 在 compute 阶段读取同一 buffer。
    vk::MemoryBarrier simulateToGather;
    simulateToGather
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        simulateToGather,
        nullptr,
        nullptr);

    globalGatherPipeline->Bind(commandBuffer);
    globalSortPool.gatheredEmitters = 0;
    for (uint32_t emitterIndex = 0;
         emitterIndex < globalSortPool.emitterCount;
         ++emitterIndex)
    {
        const auto emitter = emitters.find(
            globalSortPool.emitterKeys[emitterIndex]);
        if (emitter == emitters.end() ||
            emitter->second->culled ||
            emitter->second->frameUniforms.size() <= imageIndex)
        {
            continue;
        }
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            globalGatherPipeline->GetPipelineLayout(),
            0,
            globalSortPool.gatherSets.at(
                imageIndex * globalSortPool.emitterCount + emitterIndex),
            nullptr);
        const uint32_t groupCount =
            (emitter->second->asset.maxParticles + 255u) / 256u;
        globalGatherPipeline->Dispatch(
            commandBuffer, groupCount, 1, 1);
        ++globalSortPool.gatheredEmitters;
    }
}

void ParticleRenderer::RecordGlobalSort(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const RenderScene& renderScene)
{
    if (!globalSortPool.active ||
        globalSortPool.uniforms.size() <= imageIndex)
    {
        return;
    }
    if (globalSortPool.gatheredEmitters > 0)
    {
        frameStats.sortedEmitters +=
            globalSortPool.gatheredEmitters;
        frameStats.sortCapacityTotal += globalSortPool.capacity;
        frameStats.sortDispatchCount +=
            ParticleSortDispatchCount(globalSortPool.capacity);
    }

    // 全局 UBO：排序与绘制只消费相机矩阵；per-emitter 数据已经烘焙进
    // gathered 记录。
    FrameUniforms uniforms{};
    uniforms.viewProjection = renderScene.camera.viewProjection;
    uniforms.view = renderScene.camera.view;
    uniforms.invProjection = renderScene.camera.projection.inverse();
    uniforms.cameraPosition = Eigen::Vector4f(
        renderScene.camera.position.x(),
        renderScene.camera.position.y(),
        renderScene.camera.position.z(),
        0.0f);
    uniforms.sortParams = Eigen::Vector4f(
        1.0f,
        static_cast<float>(globalSortPool.capacity),
        static_cast<float>(globalSortPool.capacity),
        0.0f);
    uniforms.translucencyVolumeMin = Eigen::Vector4f(
        -4000.0f, -1500.0f, -4000.0f, 1.0f);
    uniforms.translucencyVolumeInvExtent = Eigen::Vector4f(
        1.0f / 8000.0f,
        1.0f / 8000.0f,
        1.0f / 8000.0f,
        0.12f);
    std::memcpy(
        globalSortPool.uniforms.at(imageIndex).mapped,
        &uniforms,
        sizeof(uniforms));

    vk::MemoryBarrier recordWriteToRead;
    recordWriteToRead
        .setSrcAccessMask(
            vk::AccessFlagBits::eShaderWrite |
            vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(
            vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        recordWriteToRead,
        nullptr,
        nullptr);

    const uint32_t groupCount =
        (globalSortPool.capacity + 255u) / 256u;
    globalSortInitPipeline->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        globalSortInitPipeline->GetPipelineLayout(),
        0,
        globalSortPool.sortInitSets.at(imageIndex),
        nullptr);
    globalSortInitPipeline->Dispatch(
        commandBuffer, groupCount, 1, 1);

    bool sourceIsA = true;
    for (uint32_t k = 2;
         k <= globalSortPool.capacity;
         k <<= 1)
    {
        for (uint32_t j = k >> 1; j > 0; j >>= 1)
        {
            InsertBufferBarrier(
                commandBuffer,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderRead,
                vk::PipelineStageFlagBits::eTransfer,
                vk::AccessFlagBits::eTransferWrite);
            const Eigen::Vector4f sortPass(
                static_cast<float>(k),
                static_cast<float>(j),
                static_cast<float>(globalSortPool.capacity),
                0.0f);
            commandBuffer.updateBuffer(
                globalSortPool.uniforms.at(imageIndex).buffer,
                offsetof(FrameUniforms, sortPass),
                sizeof(sortPass),
                &sortPass);
            InsertBufferBarrier(
                commandBuffer,
                vk::PipelineStageFlagBits::eComputeShader |
                    vk::PipelineStageFlagBits::eTransfer,
                vk::AccessFlagBits::eShaderWrite |
                    vk::AccessFlagBits::eTransferWrite,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderRead |
                    vk::AccessFlagBits::eShaderWrite);
            sortPipeline->Bind(commandBuffer);
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eCompute,
                sortPipeline->GetPipelineLayout(),
                0,
                sourceIsA
                    ? globalSortPool.sortAtoBSets.at(imageIndex)
                    : globalSortPool.sortBtoASets.at(imageIndex),
                nullptr);
            sortPipeline->Dispatch(
                commandBuffer, groupCount, 1, 1);
            sourceIsA = !sourceIsA;
        }
    }

    InsertBufferBarrier(
        commandBuffer,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::AccessFlagBits::eShaderWrite,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    finalizeCountPipeline->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        finalizeCountPipeline->GetPipelineLayout(),
        0,
        globalSortPool.finalizeSets.at(imageIndex),
        nullptr);
    finalizeCountPipeline->Dispatch(commandBuffer, 1, 1, 1);
}

void ParticleRenderer::RecordGlobalDraw(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass)
{
    if (!globalSortPool.active || !globalDrawPipeline ||
        globalSortPool.drawSets.size() <= imageIndex ||
        globalSortPool.indirectArgs.size() <= imageIndex ||
        !globalSortPool.indirectArgs[imageIndex].buffer)
    {
        return;
    }
    globalSortPool.gpuTimer.BeginPass(commandBuffer, imageIndex);
    commandBuffer.bindPipeline(
        globalDrawPipeline->GetBindPoint(),
        globalDrawPipeline->GetPipeline());
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        globalDrawPipeline->GetPipelineLayout(),
        0,
        globalSortPool.drawSets.at(imageIndex),
        nullptr);
    bool usesPassSet = false;
    for (const ShaderBinding& binding :
         globalDrawPipeline->GetShaderBindings())
    {
        if (binding.set == PassSetIndex)
        {
            usesPassSet = true;
            break;
        }
    }
    const auto& descriptorSets = renderPass.GetDescriptorSets();
    if (usesPassSet &&
        imageIndex < descriptorSets.size() &&
        PassSetIndex < descriptorSets[imageIndex].size())
    {
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            globalDrawPipeline->GetPipelineLayout(),
            PassSetIndex,
            descriptorSets[imageIndex][PassSetIndex],
            nullptr);
    }
    commandBuffer.drawIndirect(
        globalSortPool.indirectArgs[imageIndex].buffer,
        0,
        1,
        sizeof(vk::DrawIndirectCommand));
    globalSortPool.gpuTimer.EndPass(commandBuffer, imageIndex);
    ++frameStats.estimatedDrawCalls;
    frameStats.estimatedDrawnVertices +=
        static_cast<uint64_t>(globalSortPool.capacity) * 6u;
}

} // namespace VL
