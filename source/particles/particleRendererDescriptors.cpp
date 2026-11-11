#include "particles/particleRendererInternal.h"

namespace VL
{

void ParticleRenderer::CreateDescriptors(
    EmitterResources& emitter)
{
    if (emitter.asset.renderMode == "light")
    {
        CreateLightEmitterDescriptors(emitter);
        return;
    }
    if (emitter.asset.renderMode == "decal")
    {
        CreateDecalEmitterDescriptors(emitter);
        return;
    }

    const uint32_t imageCount =
        backend->GetSwapchainImageCount();
    const bool meshMaterial =
        emitter.asset.renderMode == "mesh" ||
        emitter.asset.renderMode == "skeletalMeshTris" ||
        emitter.asset.renderMode == "skeletalMeshBones";
    const uint32_t materialLayers = meshMaterial
        ? static_cast<uint32_t>(std::max<size_t>(
            !emitter.staticSections.empty()
                ? emitter.staticSections.size()
                : (emitter.skeletalSections.empty()
                    ? 1u
                    : emitter.skeletalSections.size()),
            1u))
        : 0u;
    const uint32_t materialSetCount =
        meshMaterial ? imageCount * materialLayers : 0;
    const std::vector<vk::DescriptorSetLayout> computeLayouts(
        imageCount,
        simulatePipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> graphicsLayouts(
        imageCount,
        additivePipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> sortLayouts(
        imageCount,
        sortInitPipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> finalizeLayouts(
        imageCount,
        finalizeCountPipeline->GetDescriptorSetLayouts().at(0));

    // 每个 image：simulate、graphics、sortInit、AtoB、BtoA、finalize 共 6 个 set。
    // mesh 额外每 image 一个材质 set 1（1 UBO + 4 贴图）。
    // storage 计数：simulate 9（0..9，含运行时请求）+ graphics 8 +
    // sort 三组各 2 + finalize 2 = 25。
    // graphics 新增的 2 个 storage 是场景灯/GPU 粒子灯 SSBO（binding 6/7）；
    // 再新增 2 个 storage 是 tile 灯网格/索引（binding 10/11）。
    // combined 新增 2 个是 IBL cubemap 与 BRDF LUT（binding 8/9）。
    // 骨骼完整实例再追加每 section 每 image 一个 palette storage set 2，
    // 数量与 materialSetCount 相同。
    // 每个 palette set 2 有两个 storage（palette + 影响流）。
    const uint32_t paletteSetCount = materialSetCount * 2u;
    std::vector<vk::DescriptorPoolSize> poolSizes{
        {vk::DescriptorType::eUniformBuffer,
         imageCount * 6 + materialSetCount},
        {vk::DescriptorType::eStorageBuffer,
         imageCount * 25 + materialSetCount + paletteSetCount},
        {vk::DescriptorType::eCombinedImageSampler,
         imageCount * 5 + materialSetCount * 4}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount * 6 + materialSetCount + paletteSetCount);
    emitter.descriptorPool = backend->CreateDescriptorPool(
        poolInfo,
        "ParticleDescriptors:" + emitter.key);

    try
    {
        vk::DescriptorSetAllocateInfo allocate;
        allocate
            .setDescriptorPool(emitter.descriptorPool)
            .setSetLayouts(computeLayouts);
        emitter.computeSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.computeSets);

        allocate.setSetLayouts(graphicsLayouts);
        emitter.graphicsSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.graphicsSets);

        if (meshMaterial && !emitter.staticSections.empty())
        {
            std::vector<vk::DescriptorSetLayout> materialLayouts(
                imageCount,
                meshAdditivePipeline->GetDescriptorSetLayouts().at(
                    kParticleMaterialSetIndex));
            for (auto& section : emitter.staticSections)
            {
                allocate.setSetLayouts(materialLayouts);
                section.materialSets.resize(imageCount);
                backend->AllocateDescriptorSets(
                    allocate,
                    section.materialSets);
            }
        }
        else if (meshMaterial && emitter.skeletalSections.empty())
        {
            std::vector<vk::DescriptorSetLayout> materialLayouts(
                imageCount,
                meshAdditivePipeline->GetDescriptorSetLayouts().at(
                    kParticleMaterialSetIndex));
            allocate.setSetLayouts(materialLayouts);
            emitter.materialSets.resize(imageCount);
            backend->AllocateDescriptorSets(
                allocate,
                emitter.materialSets);
        }
        else if (meshMaterial)
        {
            std::vector<vk::DescriptorSetLayout> materialLayouts(
                imageCount,
                meshAdditivePipeline->GetDescriptorSetLayouts().at(
                    kParticleMaterialSetIndex));
            for (auto& section : emitter.skeletalSections)
            {
                allocate.setSetLayouts(materialLayouts);
                section.materialSets.resize(imageCount);
                backend->AllocateDescriptorSets(
                    allocate,
                    section.materialSets);
            }
        }

        // 骨骼完整实例额外需要 set 2 的 palette；tri/bone 管线没有 set 2，
        // 只给带顶点输入的骨骼网格分配。
        if (meshMaterial && !emitter.skeletalSections.empty() &&
            skeletalMeshAdditivePipeline)
        {
            std::vector<vk::DescriptorSetLayout> paletteLayouts(
                imageCount,
                skeletalMeshAdditivePipeline->GetDescriptorSetLayouts().at(
                    kParticleSkinSetIndex));
            for (auto& section : emitter.skeletalSections)
            {
                allocate.setSetLayouts(paletteLayouts);
                section.paletteSets.resize(imageCount);
                backend->AllocateDescriptorSets(
                    allocate,
                    section.paletteSets);
            }
        }

        allocate.setSetLayouts(sortLayouts);
        emitter.sortInitSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.sortInitSets);
        emitter.sortAtoBSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.sortAtoBSets);
        emitter.sortBtoASets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.sortBtoASets);

        allocate.setSetLayouts(finalizeLayouts);
        emitter.finalizeSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.finalizeSets);

        for (uint32_t imageIndex = 0;
             imageIndex < imageCount; ++imageIndex)
        {
            const vk::DescriptorBufferInfo uniformInfo(
                emitter.frameUniforms.at(imageIndex).buffer,
                0,
                sizeof(FrameUniforms));
            const vk::DescriptorBufferInfo particleInfo(
                emitter.particles.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo pairAInfo(
                emitter.sortPairsA.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo pairBInfo(
                emitter.sortPairsB.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo aliveInfo(
                emitter.aliveIndices.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo counterInfo(
                emitter.counters.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo requestInfo(
                emitter.runtimeRequests.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo eventInfo(
                emitter.events.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo sdfInfo(
                emitter.sdfData.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo debugCounterInfo(
                emitter.debugCounters.buffer,
                0,
                VK_WHOLE_SIZE);
            // 排序关闭时 graphics 仍声明 binding 3，绑定 A 但不读取；
            // 排序开启时按 bitonic pass 奇偶选择最终 pair 表。
            const vk::DescriptorBufferInfo& finalPairInfo =
                (!emitter.asset.sortParticles ||
                 emitter.sortFinalUsesA)
                    ? pairAInfo
                    : pairBInfo;

            vk::WriteDescriptorSet uniformWrite;
            uniformWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uniformInfo);
            vk::WriteDescriptorSet particleWrite;
            particleWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(particleInfo);
            vk::WriteDescriptorSet aliveWrite;
            aliveWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(aliveInfo);
            vk::WriteDescriptorSet counterWrite;
            counterWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(3)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(counterInfo);
            vk::WriteDescriptorSet eventWrite;
            eventWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(4)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(eventInfo);
            vk::WriteDescriptorSet sdfWrite;
            sdfWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(5)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(sdfInfo);
            vk::WriteDescriptorSet requestWrite;
            requestWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(9)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(requestInfo);
            // 单个 vk::DescriptorSet 的写入必须一次批量提交：下面会复用
            // uniformWrite/particleWrite 并修改它们的 dstSet，若分批更新，
            // 前面的写入对象会被改写成别的 set，spawn 源/事件三个 binding
            // 就永远停留在默认的不初始化状态。
            std::vector<vk::WriteDescriptorSet> computeWrites{
                uniformWrite,
                particleWrite,
                aliveWrite,
                counterWrite,
                eventWrite,
                sdfWrite,
                requestWrite};
            backend->UpdateDescriptorSets(computeWrites);
            // spawn 源/父事件依赖 BindParentSpawnSources 选择的父发射器，
            // 在 Synchronize 末尾统一写入（见 EnsureSpawnSourceBindings）。
            emitter.spawnSourceBindingsDirty = true;

            vk::WriteDescriptorSet sortedWrite;
            sortedWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(3)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(finalPairInfo);
            uniformWrite.setDstSet(
                emitter.graphicsSets.at(imageIndex));
            particleWrite.setDstSet(
                emitter.graphicsSets.at(imageIndex));
            vk::WriteDescriptorSet textureWrite;
            const vk::DescriptorImageInfo textureInfo =
                emitter.texture->GetDescriptorInfo();
            textureWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(textureInfo);
            vk::WriteDescriptorSet aliveGraphicsWrite;
            aliveGraphicsWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(4)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(aliveInfo);
            vk::WriteDescriptorSet debugGraphicsWrite;
            debugGraphicsWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(5)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(debugCounterInfo);
            // 场景灯与 GPU 粒子灯按 image 绑定各自的 SSBO；IBL 是全局资源，
            // 所有 image 共用同一 cubemap/LUT。
            vk::WriteDescriptorSet sceneLightWrite;
            sceneLightWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(6)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(SceneLightInfoForImage(imageIndex));
            vk::WriteDescriptorSet particleLightWrite;
            particleLightWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(7)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(ParticleLightInfoForImage(imageIndex));
            vk::WriteDescriptorSet environmentWrite;
            environmentWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(8)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(EnvironmentCubeDescriptor());
            vk::WriteDescriptorSet brdfLutWrite;
            brdfLutWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(9)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(BrdfLutDescriptor());
            vk::WriteDescriptorSet volumeWrite;
            volumeWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(13)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(translucencyVolumeDescriptor);
            vk::WriteDescriptorSet iesWrite;
            iesWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(14)
                .setDescriptorCount(1)
                .setDescriptorType(
                    vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(iesLutTexture->GetDescriptorInfo());
            vk::WriteDescriptorSet lightGridWrite;
            lightGridWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(10)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(LightGridInfoForImage(imageIndex));
            vk::WriteDescriptorSet lightIndexWrite;
            lightIndexWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(11)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(LightIndexInfoForImage(imageIndex));
            vk::WriteDescriptorSet lightGridParamWrite;
            lightGridParamWrite
                .setDstSet(emitter.graphicsSets.at(imageIndex))
                .setDstBinding(12)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(LightGridParamInfoForImage(imageIndex));
            backend->UpdateDescriptorSets(
                {uniformWrite,
                 particleWrite,
                 textureWrite,
                 sortedWrite,
                 aliveGraphicsWrite,
                 debugGraphicsWrite,
                 sceneLightWrite,
                 particleLightWrite,
                 environmentWrite,
                 brdfLutWrite,
                 volumeWrite,
                 iesWrite,
                 lightGridWrite,
                 lightIndexWrite,
                 lightGridParamWrite});

            if (meshMaterial &&
                imageIndex < emitter.materialSets.size())
            {
                const vk::DescriptorBufferInfo materialInfo(
                    emitter.materialUniform.buffer,
                    0,
                    sizeof(ParticleMaterialUniforms));
                vk::WriteDescriptorSet materialUniformWrite;
                materialUniformWrite
                    .setDstSet(emitter.materialSets.at(imageIndex))
                    .setDstBinding(kParticleMaterialUniformBinding)
                    .setDescriptorCount(1)
                    .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                    .setBufferInfo(materialInfo);
                const std::shared_ptr<Texture> materialTextures[] = {
                    emitter.materialAlbedo,
                    emitter.materialNormal,
                    emitter.materialOrm,
                    emitter.materialEmissive};
                std::vector<vk::WriteDescriptorSet> materialWrites;
                materialWrites.push_back(materialUniformWrite);
                for (uint32_t slot = 0;
                     slot < kParticleMaterialTextureCount;
                     ++slot)
                {
                    vk::WriteDescriptorSet textureWrite;
                    textureWrite
                        .setDstSet(emitter.materialSets.at(imageIndex))
                        .setDstBinding(
                            kParticleMaterialAlbedoBinding + slot)
                        .setDescriptorCount(1)
                        .setDescriptorType(
                            vk::DescriptorType::eCombinedImageSampler)
                        .setImageInfo(
                            materialTextures[slot]->GetDescriptorInfo());
                    materialWrites.push_back(textureWrite);
                }
                backend->UpdateDescriptorSets(materialWrites);
            }
            else if (meshMaterial &&
                !emitter.staticSections.empty())
            {
                for (auto& section : emitter.staticSections)
                {
                    if (imageIndex >= section.materialSets.size())
                    {
                        continue;
                    }
                    const vk::DescriptorBufferInfo materialInfo(
                        section.materialUniform.buffer,
                        0,
                        sizeof(ParticleMaterialUniforms));
                    vk::WriteDescriptorSet materialUniformWrite;
                    materialUniformWrite
                        .setDstSet(section.materialSets.at(imageIndex))
                        .setDstBinding(
                            kParticleMaterialUniformBinding)
                        .setDescriptorCount(1)
                        .setDescriptorType(
                            vk::DescriptorType::eUniformBuffer)
                        .setBufferInfo(materialInfo);
                    const std::shared_ptr<Texture> materialTextures[] = {
                        section.albedo,
                        section.normal,
                        section.orm,
                        section.emissive};
                    std::vector<vk::WriteDescriptorSet> materialWrites;
                    materialWrites.push_back(materialUniformWrite);
                    for (uint32_t slot = 0;
                         slot < kParticleMaterialTextureCount;
                         ++slot)
                    {
                        vk::WriteDescriptorSet textureWrite;
                        textureWrite
                            .setDstSet(
                                section.materialSets.at(imageIndex))
                            .setDstBinding(
                                kParticleMaterialAlbedoBinding + slot)
                            .setDescriptorCount(1)
                            .setDescriptorType(
                                vk::DescriptorType::
                                    eCombinedImageSampler)
                            .setImageInfo(
                                materialTextures[slot]
                                    ->GetDescriptorInfo());
                        materialWrites.push_back(textureWrite);
                    }
                    backend->UpdateDescriptorSets(materialWrites);
                }
            }
            else if (meshMaterial)
            {
                for (auto& section : emitter.skeletalSections)
                {
                    if (imageIndex >= section.materialSets.size())
                    {
                        continue;
                    }
                    const vk::DescriptorBufferInfo materialInfo(
                        section.materialUniform.buffer,
                        0,
                        sizeof(ParticleMaterialUniforms));
                    vk::WriteDescriptorSet materialUniformWrite;
                    materialUniformWrite
                        .setDstSet(section.materialSets.at(imageIndex))
                        .setDstBinding(kParticleMaterialUniformBinding)
                        .setDescriptorCount(1)
                        .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                        .setBufferInfo(materialInfo);
                    const std::shared_ptr<Texture> materialTextures[] = {
                        section.albedo,
                        section.normal,
                        section.orm,
                        section.emissive};
                    std::vector<vk::WriteDescriptorSet> materialWrites;
                    materialWrites.push_back(materialUniformWrite);
                    for (uint32_t slot = 0;
                         slot < kParticleMaterialTextureCount;
                         ++slot)
                    {
                        vk::WriteDescriptorSet textureWrite;
                        textureWrite
                            .setDstSet(section.materialSets.at(imageIndex))
                            .setDstBinding(
                                kParticleMaterialAlbedoBinding + slot)
                            .setDescriptorCount(1)
                            .setDescriptorType(
                                vk::DescriptorType::eCombinedImageSampler)
                            .setImageInfo(
                                materialTextures[slot]->GetDescriptorInfo());
                        materialWrites.push_back(textureWrite);
                    }
                    // palette 按 image 写：绘制时该 image 的 palette 就是
                    // 本帧姿态，不与在飞的上一帧共享同一段 buffer。
                    if (imageIndex < section.paletteSets.size() &&
                        imageIndex < section.skinPalettes.size())
                    {
                        const vk::DescriptorBufferInfo paletteInfo(
                            section.skinPalettes.at(imageIndex).buffer,
                            0,
                            VK_WHOLE_SIZE);
                        vk::WriteDescriptorSet paletteWrite;
                        paletteWrite
                            .setDstSet(section.paletteSets.at(imageIndex))
                            .setDstBinding(kParticleSkinPaletteBinding)
                            .setDescriptorCount(1)
                            .setDescriptorType(
                                vk::DescriptorType::eStorageBuffer)
                            .setBufferInfo(paletteInfo);
                        const vk::DescriptorBufferInfo influenceInfo(
                            section.skinInfluences.buffer,
                            0,
                            VK_WHOLE_SIZE);
                        vk::WriteDescriptorSet influenceWrite;
                        influenceWrite
                            .setDstSet(section.paletteSets.at(imageIndex))
                            .setDstBinding(kParticleSkinInfluenceBinding)
                            .setDescriptorCount(1)
                            .setDescriptorType(
                                vk::DescriptorType::eStorageBuffer)
                            .setBufferInfo(influenceInfo);
                        // 材质 set 1 与蒙皮 set 2 是两个不同的 descriptor set，
                        // 可以合并到同一次 UpdateDescriptorSets；分开提交不会
                        // 互相覆盖写入对象。
                        materialWrites.push_back(paletteWrite);
                        materialWrites.push_back(influenceWrite);
                    }
                    backend->UpdateDescriptorSets(materialWrites);
                }
            }

            uniformWrite.setDstSet(
                emitter.sortInitSets.at(imageIndex));
            particleWrite
                .setDstSet(emitter.sortInitSets.at(imageIndex))
                .setDstBinding(1);
            vk::WriteDescriptorSet pairAWrite;
            pairAWrite
                .setDstSet(emitter.sortInitSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(pairAInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite, particleWrite, pairAWrite});

            uniformWrite.setDstSet(
                emitter.sortAtoBSets.at(imageIndex));
            vk::WriteDescriptorSet pairAInputWrite;
            pairAInputWrite
                .setDstSet(emitter.sortAtoBSets.at(imageIndex))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(pairAInfo);
            vk::WriteDescriptorSet pairBOutputWrite;
            pairBOutputWrite
                .setDstSet(emitter.sortAtoBSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(pairBInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite,
                 pairAInputWrite,
                 pairBOutputWrite});

            uniformWrite.setDstSet(
                emitter.sortBtoASets.at(imageIndex));
            vk::WriteDescriptorSet pairBInputWrite;
            pairBInputWrite
                .setDstSet(emitter.sortBtoASets.at(imageIndex))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(pairBInfo);
            vk::WriteDescriptorSet pairAOutputWrite;
            pairAOutputWrite
                .setDstSet(emitter.sortBtoASets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(pairAInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite,
                 pairBInputWrite,
                 pairAOutputWrite});

            // finalize：counters -> indirectArgs[1]（instanceCount）。
            vk::WriteDescriptorSet finalizeCounterWrite;
            finalizeCounterWrite
                .setDstSet(emitter.finalizeSets.at(imageIndex))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(counterInfo);
            vk::DescriptorBufferInfo indirectInfo(
                emitter.indirectArgs.buffer,
                0,
                VK_WHOLE_SIZE);
            vk::WriteDescriptorSet finalizeArgsWrite;
            finalizeArgsWrite
                .setDstSet(emitter.finalizeSets.at(imageIndex))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(indirectInfo);
            backend->UpdateDescriptorSets(
                {finalizeCounterWrite, finalizeArgsWrite});
        }
        emitter.sceneLightingGeneration = sceneLightingGeneration;
    }
    catch (...)
    {
        if (emitter.descriptorPool)
        {
            backend->DestroyDescriptorPool(
                emitter.descriptorPool);
            emitter.descriptorPool = nullptr;
        }
        throw;
    }
}

void ParticleRenderer::CreateDecalEmitterDescriptors(
    EmitterResources& emitter)
{
    const uint32_t imageCount =
        backend->GetSwapchainImageCount();
    if (decalGatherPipeline == nullptr ||
        emitter.decalGatherImages.size() < imageCount)
    {
        throw std::runtime_error(
            "particle decal gather resources are unavailable");
    }

    const std::vector<vk::DescriptorSetLayout> simulateLayouts(
        imageCount,
        simulatePipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> gatherLayouts(
        imageCount,
        decalGatherPipeline->GetDescriptorSetLayouts().at(0));

    // simulate：1 UBO + 9 storage（0..9，含运行时请求）；gather：2 UBO + 4 storage。
    std::vector<vk::DescriptorPoolSize> poolSizes{
        {vk::DescriptorType::eUniformBuffer, imageCount * 3u},
        {vk::DescriptorType::eStorageBuffer, imageCount * 13u}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount * 2u);
    emitter.descriptorPool = backend->CreateDescriptorPool(
        poolInfo,
        "ParticleDecalDescriptors:" + emitter.key);

    try
    {
        vk::DescriptorSetAllocateInfo allocate;
        allocate
            .setDescriptorPool(emitter.descriptorPool)
            .setSetLayouts(simulateLayouts);
        emitter.computeSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.computeSets);

        allocate.setSetLayouts(gatherLayouts);
        std::vector<vk::DescriptorSet> gatherSets;
        backend->AllocateDescriptorSets(allocate, gatherSets);
        for (size_t imageIndex = 0;
             imageIndex < emitter.decalGatherImages.size() &&
             imageIndex < gatherSets.size();
             ++imageIndex)
        {
            emitter.decalGatherImages[imageIndex].set =
                gatherSets[imageIndex];
        }

        for (uint32_t imageIndex = 0;
             imageIndex < imageCount;
             ++imageIndex)
        {
            const vk::DescriptorBufferInfo uniformInfo(
                emitter.frameUniforms.at(imageIndex).buffer,
                0,
                sizeof(FrameUniforms));
            const vk::DescriptorBufferInfo particleInfo(
                emitter.particles.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo aliveInfo(
                emitter.aliveIndices.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo counterInfo(
                emitter.counters.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo eventInfo(
                emitter.events.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo sdfInfo(
                emitter.sdfData.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo requestInfo(
                emitter.runtimeRequests.buffer,
                0,
                VK_WHOLE_SIZE);

            std::vector<vk::WriteDescriptorSet> simulateWrites;
            for (uint32_t binding = 0; binding <= 5; ++binding)
            {
                vk::WriteDescriptorSet write;
                write
                    .setDstSet(emitter.computeSets.at(imageIndex))
                    .setDstBinding(binding)
                    .setDescriptorCount(1)
                    .setDescriptorType(
                        binding == 0
                            ? vk::DescriptorType::eUniformBuffer
                            : vk::DescriptorType::eStorageBuffer);
                const vk::DescriptorBufferInfo* info =
                    binding == 0
                        ? &uniformInfo
                        : (binding == 1
                            ? &particleInfo
                            : (binding == 2
                                ? &aliveInfo
                                : (binding == 3
                                    ? &counterInfo
                                    : (binding == 4
                                        ? &eventInfo
                                        : &sdfInfo))));
                write.setBufferInfo(*info);
                simulateWrites.push_back(write);
            }
            backend->UpdateDescriptorSets(simulateWrites);
            vk::WriteDescriptorSet requestWrite;
            requestWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(9)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(requestInfo);
            backend->UpdateDescriptorSets({requestWrite});
            emitter.spawnSourceBindingsDirty = true;

            EmitterResources::DecalGatherImage& gather =
                emitter.decalGatherImages.at(imageIndex);
            const vk::DescriptorBufferInfo paramsInfo(
                gather.params.buffer,
                0,
                sizeof(DecalGatherUniforms));
            const vk::DescriptorBufferInfo outputInfo(
                gather.output.buffer,
                0,
                VK_WHOLE_SIZE);
            std::vector<vk::WriteDescriptorSet> gatherWrites;
            vk::WriteDescriptorSet frameWrite;
            frameWrite
                .setDstSet(gather.set)
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uniformInfo);
            gatherWrites.push_back(frameWrite);
            vk::WriteDescriptorSet particleRead;
            particleRead
                .setDstSet(gather.set)
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(particleInfo);
            gatherWrites.push_back(particleRead);
            vk::WriteDescriptorSet aliveRead;
            aliveRead
                .setDstSet(gather.set)
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(aliveInfo);
            gatherWrites.push_back(aliveRead);
            vk::WriteDescriptorSet counterRead;
            counterRead
                .setDstSet(gather.set)
                .setDstBinding(3)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(counterInfo);
            gatherWrites.push_back(counterRead);
            vk::WriteDescriptorSet paramsWrite;
            paramsWrite
                .setDstSet(gather.set)
                .setDstBinding(4)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(paramsInfo);
            gatherWrites.push_back(paramsWrite);
            vk::WriteDescriptorSet outputWrite;
            outputWrite
                .setDstSet(gather.set)
                .setDstBinding(5)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(outputInfo);
            gatherWrites.push_back(outputWrite);
            backend->UpdateDescriptorSets(gatherWrites);
        }
    }
    catch (...)
    {
        if (emitter.descriptorPool)
        {
            backend->DestroyDescriptorPool(
                emitter.descriptorPool);
            emitter.descriptorPool = nullptr;
        }
        throw;
    }
}

void ParticleRenderer::CreateLightEmitterDescriptors(
    EmitterResources& emitter)
{
    const uint32_t imageCount =
        backend->GetSwapchainImageCount();
    const bool hasLightOutput =
        particleLightBuffers.size() >= imageCount;
    if (!hasLightOutput && !warnedMissingParticleLightBuffer)
    {
        warnedMissingParticleLightBuffer = true;
        const std::string message =
            "Particle light output buffer is unavailable; "
            "renderMode=\"light\" emitters will simulate but emit no lights";
        ProcessLog().Append(
            LogSeverity::Warning,
            "Particles",
            message);
        std::cout << "[Particles][Warning] " << message << std::endl;
    }

    const std::vector<vk::DescriptorSetLayout> computeLayouts(
        imageCount,
        simulatePipeline->GetDescriptorSetLayouts().at(0));
    const std::vector<vk::DescriptorSetLayout> lightEmitLayouts(
        imageCount,
        particleLightEmitPipeline->GetDescriptorSetLayouts().at(0));

    // simulate 固定 1 UBO + 9 storage（binding 1..9，含运行时请求）；
    // light emit 固定 1 UBO + 2 storage。
    const uint32_t setCountPerImage = hasLightOutput ? 2u : 1u;
    std::vector<vk::DescriptorPoolSize> poolSizes{
        {vk::DescriptorType::eUniformBuffer,
         imageCount * setCountPerImage},
        {vk::DescriptorType::eStorageBuffer,
         imageCount * (9u + (hasLightOutput ? 2u : 0u))}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount * setCountPerImage);
    emitter.descriptorPool = backend->CreateDescriptorPool(
        poolInfo,
        "ParticleLightDescriptors:" + emitter.key);

    try
    {
        vk::DescriptorSetAllocateInfo allocate;
        allocate
            .setDescriptorPool(emitter.descriptorPool)
            .setSetLayouts(computeLayouts);
        emitter.computeSets.resize(imageCount);
        backend->AllocateDescriptorSets(
            allocate,
            emitter.computeSets);

        if (hasLightOutput)
        {
            allocate.setSetLayouts(lightEmitLayouts);
            emitter.lightEmitSets.resize(imageCount);
            backend->AllocateDescriptorSets(
                allocate,
                emitter.lightEmitSets);
        }

        for (uint32_t imageIndex = 0;
             imageIndex < imageCount; ++imageIndex)
        {
            const vk::DescriptorBufferInfo uniformInfo(
                emitter.frameUniforms.at(imageIndex).buffer,
                0,
                sizeof(FrameUniforms));
            const vk::DescriptorBufferInfo particleInfo(
                emitter.particles.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo aliveInfo(
                emitter.aliveIndices.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo counterInfo(
                emitter.counters.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo eventInfo(
                emitter.events.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo sdfInfo(
                emitter.sdfData.buffer,
                0,
                VK_WHOLE_SIZE);
            const vk::DescriptorBufferInfo requestInfo(
                emitter.runtimeRequests.buffer,
                0,
                VK_WHOLE_SIZE);

            vk::WriteDescriptorSet uniformWrite;
            uniformWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uniformInfo);
            vk::WriteDescriptorSet particleWrite;
            particleWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(particleInfo);
            vk::WriteDescriptorSet aliveWrite;
            aliveWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(aliveInfo);
            vk::WriteDescriptorSet counterWrite;
            counterWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(3)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(counterInfo);
            vk::WriteDescriptorSet eventWrite;
            eventWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(4)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(eventInfo);
            vk::WriteDescriptorSet sdfWrite;
            sdfWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(5)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(sdfInfo);
            vk::WriteDescriptorSet requestWrite;
            requestWrite
                .setDstSet(emitter.computeSets.at(imageIndex))
                .setDstBinding(9)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(requestInfo);
            backend->UpdateDescriptorSets(
                {uniformWrite,
                 particleWrite,
                 aliveWrite,
                 counterWrite,
                 eventWrite,
                 sdfWrite,
                 requestWrite});

            if (!hasLightOutput)
            {
                continue;
            }
            vk::DescriptorBufferInfo lightOutputInfo(
                particleLightBuffers.at(imageIndex),
                0,
                VK_WHOLE_SIZE);
            vk::WriteDescriptorSet lightUniformWrite;
            lightUniformWrite
                .setDstSet(emitter.lightEmitSets.at(imageIndex))
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uniformInfo);
            vk::WriteDescriptorSet lightParticleWrite;
            lightParticleWrite
                .setDstSet(emitter.lightEmitSets.at(imageIndex))
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(particleInfo);
            vk::WriteDescriptorSet lightOutputWrite;
            lightOutputWrite
                .setDstSet(emitter.lightEmitSets.at(imageIndex))
                .setDstBinding(2)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(lightOutputInfo);
            backend->UpdateDescriptorSets(
                {lightUniformWrite,
                 lightParticleWrite,
                 lightOutputWrite});
        }
    }
    catch (...)
    {
        if (emitter.descriptorPool)
        {
            backend->DestroyDescriptorPool(
                emitter.descriptorPool);
            emitter.descriptorPool = nullptr;
        }
        throw;
    }
    emitter.lightBufferGeneration =
        particleLightBufferGeneration;
    emitter.sceneLightingGeneration = sceneLightingGeneration;
}

} // namespace VL
