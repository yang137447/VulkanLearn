#include "particles/particleRendererInternal.h"

namespace VL
{

void ParticleRenderer::CreateBuffers(EmitterResources& emitter)
{
    emitter.sortCapacity = SortedCapacity(emitter.asset.maxParticles);
    const uint32_t sortPassCount =
        ParticleSortDispatchCount(emitter.sortCapacity);
    emitter.sortFinalUsesA = (sortPassCount % 2u) == 0u;

    const vk::DeviceSize particleBytes =
        sizeof(GpuParticle) *
        static_cast<vk::DeviceSize>(emitter.asset.maxParticles);
    const auto particleAllocation = backend->CreateBuffer(
        particleBytes,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst |
            vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticlePool:" + emitter.key);
    emitter.particles.buffer = particleAllocation.first;
    emitter.particles.memory = particleAllocation.second;

    // 每个 emitter 一份当前帧 spawn/death/collision 输出，供子 emitter 读取。
    // 父 emitter 写 header + 最多 1024 条记录；子 emitter 只消费当前帧。
    constexpr uint32_t kMaxSpawnEvents = 1024;
    const vk::DeviceSize spawnEventBytes =
        sizeof(uint32_t) * 4 +
        static_cast<vk::DeviceSize>(kMaxSpawnEvents) *
            sizeof(float) * 16;
    const auto spawnEventAllocation = backend->CreateBuffer(
        spawnEventBytes,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleSpawnEvents:" + emitter.key);
    emitter.spawnEvents.buffer = spawnEventAllocation.first;
    emitter.spawnEvents.memory = spawnEventAllocation.second;
    if (!emitter.spawnSource.buffer)
    {
        const auto sourceAllocation = backend->CreateBuffer(
            16,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleSpawnSourceEmpty:" + emitter.key);
        emitter.spawnSource.buffer = sourceAllocation.first;
        emitter.spawnSource.memory = sourceAllocation.second;
        void* mapped = backend->MapMemory(
            emitter.spawnSource.memory, 16);
        if (mapped != nullptr)
        {
            std::memset(mapped, 0, 16);
            backend->UnmapMemory(emitter.spawnSource.memory);
        }
    }

    // GPU 计数（alive/spawn/death/collision）每帧 fillBuffer 清零。
    const auto counterAllocation = backend->CreateBuffer(
        sizeof(ParticleEventCounters),
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst |
            vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleCounters:" + emitter.key);
    emitter.counters.buffer = counterAllocation.first;
    emitter.counters.memory = counterAllocation.second;
    void* mappedCounters = backend->MapMemory(
        emitter.counters.memory, sizeof(ParticleEventCounters));
    if (mappedCounters == nullptr)
    {
        throw std::runtime_error("particle counter map failed");
    }
    ParticleEventCounters initialCounters;
    initialCounters.capacity = emitter.asset.maxParticles;
    std::memcpy(
        mappedCounters, &initialCounters, sizeof(initialCounters));
    backend->UnmapMemory(emitter.counters.memory);

    // 运行时 kill/spawn 请求：header（uvec4）+ 16 条 32 字节记录。
    // 每帧由 RecordSimulation 在 GPU 提交前覆盖写，host-coherent 即可。
    constexpr uint32_t kParticleRuntimeRequestCapacity = 16;
    const vk::DeviceSize requestBytes =
        sizeof(uint32_t) * 4 +
        sizeof(ParticleRuntimeRequestGpu) *
            kParticleRuntimeRequestCapacity;
    const auto requestAllocation = backend->CreateBuffer(
        requestBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleRuntimeRequests:" + emitter.key);
    emitter.runtimeRequests.buffer = requestAllocation.first;
    emitter.runtimeRequests.memory = requestAllocation.second;
    emitter.runtimeRequests.mapped = backend->MapMemory(
        emitter.runtimeRequests.memory, requestBytes);
    if (emitter.runtimeRequests.mapped == nullptr)
    {
        throw std::runtime_error(
            "particle runtime request map failed");
    }
    std::memset(
        emitter.runtimeRequests.mapped,
        0,
        static_cast<size_t>(requestBytes));

    // 紧凑存活索引：simulate 原子追加，未排序绘制与 CPU 统计共用。
    const vk::DeviceSize aliveBytes =
        sizeof(uint32_t) *
        static_cast<vk::DeviceSize>(
            std::max(emitter.asset.maxParticles, 1u));
    const auto aliveAllocation = backend->CreateBuffer(
        aliveBytes,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleAliveIndices:" + emitter.key);
    emitter.aliveIndices.buffer = aliveAllocation.first;
    emitter.aliveIndices.memory = aliveAllocation.second;

    // 有界事件缓冲；每帧从 slot 0 覆盖写，计数器超容量表示截断。
    const vk::DeviceSize eventBytes =
        sizeof(ParticleEventRecord) * kParticleEventCapacity;
    const auto eventAllocation = backend->CreateBuffer(
        eventBytes,
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleEvents:" + emitter.key);
    emitter.events.buffer = eventAllocation.first;
    emitter.events.memory = eventAllocation.second;

    // 图形 overdraw 计数：只写 uint，但需要 transfer 清除和回读。
    const auto debugCounterAllocation = backend->CreateBuffer(
        sizeof(ParticleOverdrawCounters),
        vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst |
            vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleOverdraw:" + emitter.key);
    emitter.debugCounters.buffer = debugCounterAllocation.first;
    emitter.debugCounters.memory = debugCounterAllocation.second;

    // SDF 体素上传；无 SDF 时绑定 1 个 float 的占位 buffer，保持
    // descriptor layout 与 shader binding 5 始终有效。
    emitter.sdfVoxelCount = 0;
    const vk::DeviceSize sdfBytes =
        emitter.asset.collisionSdf &&
            !emitter.asset.collisionSdf->Empty()
        ? static_cast<vk::DeviceSize>(
              emitter.asset.collisionSdf->distances.size()) *
              sizeof(float)
        : sizeof(float);
    const auto sdfAllocation = backend->CreateBuffer(
        sdfBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleSdf:" + emitter.key);
    emitter.sdfData.buffer = sdfAllocation.first;
    emitter.sdfData.memory = sdfAllocation.second;
    void* mappedSdf = backend->MapMemory(
        emitter.sdfData.memory, sdfBytes);
    if (mappedSdf == nullptr)
    {
        throw std::runtime_error("particle SDF map failed");
    }
    if (emitter.asset.collisionSdf &&
        !emitter.asset.collisionSdf->Empty())
    {
        emitter.sdf = emitter.asset.collisionSdf;
        emitter.sdfVoxelCount = static_cast<uint32_t>(
            emitter.sdf->distances.size());
        std::memcpy(
            mappedSdf,
            emitter.sdf->distances.data(),
            static_cast<size_t>(sdfBytes));
    }
    else
    {
        const float empty = 0.0f;
        std::memcpy(mappedSdf, &empty, sizeof(float));
    }
    backend->UnmapMemory(emitter.sdfData.memory);

    // pair 表始终存在：graphics shader 无条件声明 binding 3，排序关闭时
    // 绑定 A 但不读取，排序开启时按 pass 奇偶决定最终绑定 A 还是 B。
    const vk::DeviceSize pairBytes =
        sizeof(Eigen::Vector4f) *
        static_cast<vk::DeviceSize>(emitter.sortCapacity);
    const auto pairAAllocation = backend->CreateBuffer(
        pairBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleSortPairsA:" + emitter.key);
    emitter.sortPairsA.buffer = pairAAllocation.first;
    emitter.sortPairsA.memory = pairAAllocation.second;
    const auto pairBAllocation = backend->CreateBuffer(
        pairBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ParticleSortPairsB:" + emitter.key);
    emitter.sortPairsB.buffer = pairBAllocation.first;
    emitter.sortPairsB.memory = pairBAllocation.second;

    // 间接参数与 emitter 池同生命周期；内容目前是固定顶点/索引与实例数，
    // 但录制路径已经不再关心具体计数，后续 GPU culling 只需更新该缓冲。
    const bool meshUsable =
        !emitter.meshFallbackBillboard &&
        ((emitter.meshVertices.buffer && emitter.meshIndices.buffer) ||
         !emitter.staticSections.empty());
    const ParticleIndirectDrawShape indirectShape =
        BuildParticleIndirectDrawShape(
            emitter.asset.renderMode,
            emitter.asset.maxParticles,
            emitter.asset.ribbonSegments,
            meshUsable,
            emitter.staticSections.empty()
                ? emitter.meshIndexCount
                : emitter.staticSections.front().indexCount,
            emitter.asset.beam.segments,
            emitter.asset.beam.count,
            emitter.asset.beam.branchCount);
    const uint32_t indirectCommandCount =
        emitter.asset.renderMode == "mesh" &&
            !emitter.staticSections.empty()
        ? static_cast<uint32_t>(emitter.staticSections.size())
        : 1u;
    // finalize compute 固定同步最多 64 条 indexed 命令的 instanceCount；
    // 所有 emitter 的 indirect 缓冲都至少保留这段空间，避免 SSBO 越界。
    const vk::DeviceSize minimumIndirectBytes =
        sizeof(uint32_t) * 5u * 64u;
    const vk::DeviceSize indirectBytes = std::max(
        minimumIndirectBytes,
        indirectShape.indexed
            ? sizeof(vk::DrawIndexedIndirectCommand) *
                indirectCommandCount
            : sizeof(vk::DrawIndirectCommand));
    const auto indirectAllocation = backend->CreateBuffer(
        indirectBytes,
        vk::BufferUsageFlagBits::eIndirectBuffer |
            vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "ParticleIndirect:" + emitter.key);
    emitter.indirectArgs.buffer = indirectAllocation.first;
    emitter.indirectArgs.memory = indirectAllocation.second;
    void* mappedIndirect = backend->MapMemory(
        emitter.indirectArgs.memory,
        indirectBytes);
    if (mappedIndirect == nullptr)
    {
        throw std::runtime_error("particle indirect parameter map failed");
    }
    if (indirectShape.indexed)
    {
        auto* commands = static_cast<vk::DrawIndexedIndirectCommand*>(
            mappedIndirect);
        for (uint32_t commandIndex = 0;
             commandIndex < indirectCommandCount;
             ++commandIndex)
        {
            const uint32_t indexCount = emitter.staticSections.empty()
                ? indirectShape.indexCount
                : emitter.staticSections[commandIndex].indexCount;
            commands[commandIndex] = vk::DrawIndexedIndirectCommand(
                indexCount,
                indirectShape.instanceCount,
                0,
                0,
                0);
        }
    }
    else
    {
        const vk::DrawIndirectCommand command(
            indirectShape.vertexCount,
            indirectShape.instanceCount,
            0,
            0);
        std::memcpy(mappedIndirect, &command, sizeof(command));
    }
    backend->UnmapMemory(emitter.indirectArgs.memory);
    emitter.indirectShape = indirectShape;
    emitter.indirectIndexed = indirectShape.indexed;

    // 静态 mesh 的单材质 UBO；骨骼网格的材质 UBO 由 LoadSkeletalEmitterMesh
    // 按 section 创建，与 per-section materialSets 一一对应，这里不再创建。
    if (emitter.asset.renderMode == "mesh" &&
        emitter.staticSections.empty())
    {
        const auto materialAllocation = backend->CreateBuffer(
            sizeof(ParticleMaterialUniforms),
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleMaterialUniform:" + emitter.key);
        emitter.materialUniform.buffer = materialAllocation.first;
        emitter.materialUniform.memory = materialAllocation.second;
        void* mappedMaterial = backend->MapMemory(
            emitter.materialUniform.memory,
            sizeof(ParticleMaterialUniforms));
        if (mappedMaterial == nullptr)
        {
            throw std::runtime_error(
                "particle material uniform map failed");
        }
        const ParticleMaterialParams& params = emitter.material;
        ParticleMaterialUniforms uniforms{};
        uniforms.uvScaleOffset = Eigen::Vector4f(
            params.uvScale.x(),
            params.uvScale.y(),
            params.uvOffset.x(),
            params.uvOffset.y());
        uniforms.textureFlags = Eigen::Vector4f(
            params.albedoMap.empty() ? 0.0f : 1.0f,
            params.normalMap.empty() ? 0.0f : 1.0f,
            params.ormMap.empty() ? 0.0f : 1.0f,
            params.emissiveMap.empty() ? 0.0f : 1.0f);
        uniforms.materialParams = Eigen::Vector4f(
            params.alphaClipThreshold,
            params.ambientOcclusion,
            params.normalStrength,
            0.0f);
        std::memcpy(
            mappedMaterial,
            &uniforms,
            sizeof(ParticleMaterialUniforms));
        backend->UnmapMemory(emitter.materialUniform.memory);
        emitter.materialUniform.mapped = nullptr;
    }

    const uint32_t imageCount =
        backend->GetSwapchainImageCount();
    emitter.frameUniforms.reserve(imageCount);
    for (uint32_t imageIndex = 0;
         imageIndex < imageCount; ++imageIndex)
    {
        const auto uniformAllocation = backend->CreateBuffer(
            sizeof(FrameUniforms),
            // sortPass 每个 bitonic pass 都用 vkCmdUpdateBuffer 顺序写入；
            // 普通 memcpy 会在提交时只留下最后一次值。
            vk::BufferUsageFlagBits::eUniformBuffer |
                vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleUniforms:" + emitter.key + ":" +
                std::to_string(imageIndex));
        GpuBuffer uniformBuffer;
        uniformBuffer.buffer = uniformAllocation.first;
        uniformBuffer.memory = uniformAllocation.second;
        uniformBuffer.mapped = backend->MapMemory(
            uniformBuffer.memory,
            sizeof(FrameUniforms));
        emitter.frameUniforms.push_back(uniformBuffer);
    }

    // GPU decal gather：每个 image 一份参数 UBO、设备本地输出与
    // host-visible 读回副本。读回缓冲创建后保持映射并清零，首帧尚未
    // dispatch gather 时 count=0，不会读到未初始化内存。
    if (emitter.asset.renderMode == "decal")
    {
        emitter.decalGatherImages.reserve(imageCount);
        for (uint32_t imageIndex = 0;
             imageIndex < imageCount;
             ++imageIndex)
        {
            EmitterResources::DecalGatherImage image;
            const auto paramsAllocation = backend->CreateBuffer(
                sizeof(DecalGatherUniforms),
                vk::BufferUsageFlagBits::eUniformBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                "ParticleDecalParams:" + emitter.key + ":" +
                    std::to_string(imageIndex));
            image.params.buffer = paramsAllocation.first;
            image.params.memory = paramsAllocation.second;
            image.params.mapped = backend->MapMemory(
                image.params.memory,
                sizeof(DecalGatherUniforms));
            if (image.params.mapped == nullptr)
            {
                throw std::runtime_error(
                    "particle decal params map failed");
            }
            std::memset(
                image.params.mapped,
                0,
                sizeof(DecalGatherUniforms));

            const auto outputAllocation = backend->CreateBuffer(
                kParticleDecalGatherBytes,
                vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eTransferSrc |
                    vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eDeviceLocal,
                "ParticleDecalGather:" + emitter.key + ":" +
                    std::to_string(imageIndex));
            image.output.buffer = outputAllocation.first;
            image.output.memory = outputAllocation.second;

            const auto readbackAllocation = backend->CreateBuffer(
                kParticleDecalGatherBytes,
                vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                "ParticleDecalReadback:" + emitter.key + ":" +
                    std::to_string(imageIndex));
            image.readback.buffer = readbackAllocation.first;
            image.readback.memory = readbackAllocation.second;
            image.readback.mapped = backend->MapMemory(
                image.readback.memory,
                kParticleDecalGatherBytes);
            if (image.readback.mapped == nullptr)
            {
                throw std::runtime_error(
                    "particle decal readback map failed");
            }
            std::memset(
                image.readback.mapped,
                0,
                static_cast<size_t>(kParticleDecalGatherBytes));
            emitter.decalGatherImages.push_back(std::move(image));
        }
    }
}

void ParticleRenderer::DestroyEmitterResources(
    RendererBackendVulkan& rendererBackend,
    EmitterResources& emitter)
{
    emitter.gpuTimer.Shutdown(rendererBackend);
    if (emitter.descriptorPool)
    {
        rendererBackend.DestroyDescriptorPool(emitter.descriptorPool);
        emitter.descriptorPool = nullptr;
    }
    emitter.computeSets.clear();
    emitter.graphicsSets.clear();
    emitter.sortInitSets.clear();
    emitter.sortAtoBSets.clear();
    emitter.sortBtoASets.clear();
    emitter.finalizeSets.clear();
    emitter.lightEmitSets.clear();
    emitter.materialSets.clear();
    emitter.texture.reset();
    emitter.materialAlbedo.reset();
    emitter.materialNormal.reset();
    emitter.materialOrm.reset();
    emitter.materialEmissive.reset();
    DestroyBuffer(rendererBackend, emitter.materialUniform);
    DestroyBuffer(rendererBackend, emitter.meshVertices);
    DestroyBuffer(rendererBackend, emitter.meshIndices);
    for (auto& section : emitter.staticSections)
    {
        DestroyBuffer(rendererBackend, section.vertices);
        DestroyBuffer(rendererBackend, section.indices);
        DestroyBuffer(rendererBackend, section.materialUniform);
        section.materialSets.clear();
    }
    emitter.staticSections.clear();
    DestroyBuffer(rendererBackend, emitter.spawnEvents);
    DestroyBuffer(rendererBackend, emitter.spawnSource);
    for (auto& section : emitter.skeletalSections)
    {
        DestroyBuffer(rendererBackend, section.vertices);
        DestroyBuffer(rendererBackend, section.indices);
        DestroyBuffer(rendererBackend, section.skinInfluences);
        for (GpuBuffer& palette : section.skinPalettes)
        {
            DestroyBuffer(rendererBackend, palette);
        }
        section.skinPalettes.clear();
        section.paletteSets.clear();
        section.materialSets.clear();
        DestroyBuffer(rendererBackend, section.materialUniform);
    }
    emitter.skeletalSections.clear();
    DestroyBuffer(rendererBackend, emitter.particles);
    DestroyBuffer(rendererBackend, emitter.counters);
    DestroyBuffer(rendererBackend, emitter.aliveIndices);
    DestroyBuffer(rendererBackend, emitter.events);
    DestroyBuffer(rendererBackend, emitter.runtimeRequests);
    DestroyBuffer(rendererBackend, emitter.debugCounters);
    DestroyBuffer(rendererBackend, emitter.sdfData);
    DestroyBuffer(rendererBackend, emitter.sortPairsA);
    DestroyBuffer(rendererBackend, emitter.sortPairsB);
    DestroyBuffer(rendererBackend, emitter.indirectArgs);
    for (GpuBuffer& buffer : emitter.frameUniforms)
    {
        DestroyBuffer(rendererBackend, buffer);
    }
    emitter.frameUniforms.clear();
    for (EmitterResources::DecalGatherImage& gather :
         emitter.decalGatherImages)
    {
        DestroyBuffer(rendererBackend, gather.params);
        DestroyBuffer(rendererBackend, gather.output);
        DestroyBuffer(rendererBackend, gather.readback);
    }
    emitter.decalGatherImages.clear();
}

void ParticleRenderer::DestroyBuffer(
    RendererBackendVulkan& rendererBackend,
    GpuBuffer& buffer)
{
    if (buffer.mapped != nullptr)
    {
        rendererBackend.UnmapMemory(buffer.memory);
        buffer.mapped = nullptr;
    }
    if (buffer.buffer)
    {
        rendererBackend.DestroyBuffer(buffer.buffer, buffer.memory);
    }
    buffer = {};
}

void ParticleRenderer::RetireEmitterResources(
    std::shared_ptr<EmitterResources> emitter)
{
    if (!emitter)
    {
        return;
    }
    // 先固定 debugName 再 move 载荷：C++ 未规定实参求值顺序，若编译器先
    // 构造最后一个 shared_ptr 参数，再读 emitter->key 就会解引用空指针。
    const std::string debugName =
        "ParticleEmitter:" + emitter->key;
    ResourceRetireQueue& retireQueue =
        ResourceRetireQueue::GetInstance();
    const uint64_t lastUsedEpoch =
        retireQueue.GetLastSubmittedEpoch();
    // 资源可能在最近若干帧都被 compute/graphics 引用。记录最后一次已提交
    // epoch，等对应 fence 完成后由队列析构整个 EmitterResources 包。
    retireQueue.RetireShared(
        debugName,
        renderIdentity.ownerGeneration,
        lastUsedEpoch,
        std::move(emitter));
}

void ParticleRenderer::WarnTextureFallbackOnce(
    const std::string& key,
    const std::string& texturePath,
    const std::string& error)
{
    if (!warnedTextureFallbacks.insert(key + "|" + texturePath).second)
    {
        return;
    }
    const std::string message =
        "Particle texture fallback for emitter '" + key +
        "' (" + texturePath + "): " + error +
        "; using the default white texture";
    ProcessLog().Append(
        LogSeverity::Warning,
        "Particles",
        message);
    std::cout << "[Particles][Warning] " << message << std::endl;
}

void ParticleRenderer::WarnMeshFallbackOnce(
    const std::string& key,
    const std::string& meshPath,
    const std::string& error)
{
    if (!warnedMeshFallbacks.insert(key + "|" + meshPath).second)
    {
        return;
    }
    const std::string message =
        "Particle mesh fallback for emitter '" + key +
        "' (" + meshPath + "): " + error +
        "; using billboard rendering";
    ProcessLog().Append(
        LogSeverity::Warning,
        "Particles",
        message);
    std::cout << "[Particles][Warning] " << message << std::endl;
}

void ParticleRenderer::WarnMaterialFallbackOnce(
    const std::string& key,
    const std::string& materialPath,
    const std::string& error)
{
    if (!warnedMaterialFallbacks.insert(key + "|" + materialPath).second)
    {
        return;
    }
    const std::string message =
        "Particle material fallback for emitter '" + key +
        "' (" + materialPath + "): " + error +
        "; using built-in default PBR factors";
    ProcessLog().Append(
        LogSeverity::Warning,
        "Particles",
        message);
    std::cout << "[Particles][Warning] " << message << std::endl;
}

void ParticleRenderer::InsertBufferBarrier(
    vk::CommandBuffer commandBuffer,
    vk::PipelineStageFlags sourceStage,
    vk::AccessFlags sourceAccess,
    vk::PipelineStageFlags destinationStage,
    vk::AccessFlags destinationAccess)
{
    vk::MemoryBarrier barrier;
    barrier
        .setSrcAccessMask(sourceAccess)
        .setDstAccessMask(destinationAccess);
    commandBuffer.pipelineBarrier(
        sourceStage,
        destinationStage,
        {},
        barrier,
        nullptr,
        nullptr);
}

void ParticleRenderer::ClearParticleRuntimeCounters(
    vk::CommandBuffer commandBuffer,
    EmitterResources& emitter)
{
    if (!emitter.counters.buffer && !emitter.debugCounters.buffer)
    {
        return;
    }
    // 上一轮 compute 写 alive/事件，fragment 写 overdraw；都必须在 transfer
    // 清零之前完成（即使调用方已做提交级串行化，这里仍保留显式依赖）。
    vk::MemoryBarrier toTransfer;
    toTransfer
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        toTransfer,
        nullptr,
        nullptr);
    if (emitter.counters.buffer)
    {
        commandBuffer.fillBuffer(
            emitter.counters.buffer,
            0,
            sizeof(uint32_t) * 5u,
            0);
    }
    if (emitter.debugCounters.buffer)
    {
        commandBuffer.fillBuffer(
            emitter.debugCounters.buffer,
            0,
            sizeof(ParticleOverdrawCounters),
            0);
    }
}

void ParticleRenderer::ReadbackDecalGather(
    EmitterResources& emitter,
    uint32_t imageIndex)
{
    if (emitter.asset.renderMode != "decal" ||
        imageIndex >= emitter.decalGatherImages.size())
    {
        return;
    }
    const EmitterResources::DecalGatherImage& gather =
        emitter.decalGatherImages[imageIndex];
    auto& cache = decalGpuInstances[emitter.key];
    cache.clear();
    if (gather.readback.mapped == nullptr)
    {
        return;
    }
    const auto* header = static_cast<const DecalGpuHeader*>(
        gather.readback.mapped);
    const uint32_t recordCount = std::min(
        header->count,
        kParticleDecalGpuCapacity);
    if (recordCount == 0)
    {
        return;
    }
    const auto* records = reinterpret_cast<
        const ParticleDecalGpuInstance*>(
        static_cast<const unsigned char*>(gather.readback.mapped) +
        sizeof(DecalGpuHeader));
    cache.assign(records, records + recordCount);
}

void ParticleRenderer::RecordDecalGather(
    vk::CommandBuffer commandBuffer,
    EmitterResources& emitter,
    uint32_t imageIndex,
    float lodRenderScale)
{
    if (decalGatherPipeline == nullptr ||
        imageIndex >= emitter.decalGatherImages.size())
    {
        return;
    }
    EmitterResources::DecalGatherImage& gather =
        emitter.decalGatherImages[imageIndex];
    if (!gather.set || !gather.output.buffer ||
        !gather.readback.buffer)
    {
        return;
    }

    if (gather.params.mapped != nullptr)
    {
        auto* params = static_cast<DecalGatherUniforms*>(
            gather.params.mapped);
        params->size = Eigen::Vector4f(
            emitter.asset.decal.size.x() * lodRenderScale,
            emitter.asset.decal.size.y() * lodRenderScale,
            emitter.asset.decal.size.z() * lodRenderScale,
            0.0f);
        params->rotation = Eigen::Vector4f(
            emitter.asset.decal.rotation.x(),
            emitter.asset.decal.rotation.y(),
            emitter.asset.decal.rotation.z(),
            0.0f);
        params->colorOpacity = Eigen::Vector4f(
            emitter.asset.decal.color.x(),
            emitter.asset.decal.color.y(),
            emitter.asset.decal.color.z(),
            emitter.asset.decal.opacity);
        params->reserved = Eigen::Vector4f::Zero();
    }

    // 上一帧的 header.count 是原子累加结果，本帧必须先清零；用 fillBuffer
    // 覆盖 16 字节 header，再把可见性交给 compute。
    vk::BufferMemoryBarrier previousWriteToTransfer;
    previousWriteToTransfer
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(gather.output.buffer)
        .setOffset(0)
        .setSize(sizeof(DecalGpuHeader));
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        nullptr,
        previousWriteToTransfer,
        nullptr);
    commandBuffer.fillBuffer(
        gather.output.buffer, 0, sizeof(DecalGpuHeader), 0u);
    vk::BufferMemoryBarrier clearToCompute;
    clearToCompute
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(
            vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(gather.output.buffer)
        .setOffset(0)
        .setSize(VK_WHOLE_SIZE);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        nullptr,
        clearToCompute,
        nullptr);

    // simulate 写粒子池；gather 读取同一段数据并写输出，必须显式建立
    // compute write -> compute read 依赖。
    InsertBufferBarrier(
        commandBuffer,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::AccessFlagBits::eShaderWrite,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::AccessFlagBits::eShaderRead);
    decalGatherPipeline->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        decalGatherPipeline->GetPipelineLayout(),
        0,
        gather.set,
        nullptr);
    decalGatherPipeline->Dispatch(commandBuffer, 1, 1, 1);

    vk::BufferMemoryBarrier gatherToTransfer;
    gatherToTransfer
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(gather.output.buffer)
        .setOffset(0)
        .setSize(VK_WHOLE_SIZE);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        nullptr,
        gatherToTransfer,
        nullptr);
    commandBuffer.copyBuffer(
        gather.output.buffer,
        gather.readback.buffer,
        vk::BufferCopy(
            0, 0, kParticleDecalGatherBytes));
    vk::BufferMemoryBarrier transferToHost;
    transferToHost
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eHostRead)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(gather.readback.buffer)
        .setOffset(0)
        .setSize(VK_WHOLE_SIZE);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eHost,
        {},
        nullptr,
        transferToHost,
        nullptr);
}

} // namespace VL
