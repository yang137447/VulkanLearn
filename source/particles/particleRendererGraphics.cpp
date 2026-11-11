#include "particles/particleRendererInternal.h"

namespace VL
{

void ParticleRenderer::RecordGraphics(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass)
{
    frameStats.estimatedDrawCalls = 0;
    frameStats.estimatedDrawnVertices = 0;
    if (!initialized)
    {
        return;
    }

    const auto recordEmitter = [&](EmitterResources& emitter)
    {
        if (emitter.culled ||
            emitter.asset.renderMode == "distortion" ||
            emitter.asset.renderMode == "light" ||
            emitter.asset.renderMode == "decal" ||
            IsGlobalSortEmitter(emitter.key) ||
            emitter.graphicsSets.size() <= imageIndex)
        {
            return;
        }
        const bool staticSectionMesh =
            emitter.asset.renderMode == "mesh" &&
            !emitter.meshFallbackBillboard &&
            !emitter.staticSections.empty();
        const bool mesh =
            emitter.asset.renderMode == "mesh" &&
            !staticSectionMesh &&
            !emitter.meshFallbackBillboard &&
            emitter.meshIndexCount > 0 &&
            emitter.meshVertices.buffer &&
            emitter.meshIndices.buffer;
        const bool skeletalMesh =
            emitter.asset.renderMode == "mesh" &&
            !emitter.skeletalSections.empty();
        const bool skeletalTriangle =
            emitter.asset.renderMode == "skeletalMeshTris" &&
            !emitter.skeletalSections.empty();
        const bool skeletalBone =
            emitter.asset.renderMode == "skeletalMeshBones" &&
            !emitter.skeletalSections.empty();
        const bool beam =
            emitter.asset.renderMode == "beam";
        std::shared_ptr<PipelineBase> pipeline;
        if (skeletalTriangle)
        {
            pipeline = emitter.asset.additive
                ? skeletalTriangleAdditivePipeline
                : skeletalTriangleAlphaPipeline;
        }
        else if (skeletalBone)
        {
            pipeline = emitter.asset.additive
                ? skeletalBoneAdditivePipeline
                : skeletalBoneAlphaPipeline;
        }
        else if (mesh || staticSectionMesh || skeletalMesh)
        {
            // 骨骼完整实例必须使用 VL_SKINNED_MESH 变体并绑定 set 2，
            // 静态 mesh 继续走不带蒙皮的管线。
            const bool skinned = skeletalMesh;
            pipeline = emitter.asset.additive
                ? (skinned
                    ? skeletalMeshAdditivePipeline
                    : meshAdditivePipeline)
                : (skinned
                    ? skeletalMeshAlphaPipeline
                    : meshAlphaPipeline);
        }
        else if (beam)
        {
            pipeline = emitter.asset.additive
                ? beamAdditivePipeline
                : beamAlphaPipeline;
        }
        else if (emitter.asset.renderMode == "ribbon")
        {
            pipeline = emitter.asset.additive
                ? ribbonAdditivePipeline
                : ribbonAlphaPipeline;
        }
        else
        {
            pipeline = emitter.asset.additive
                ? additivePipeline
                : alphaPipeline;
        }
        if (!pipeline)
        {
            return;
        }
        emitter.gpuTimer.BeginPass(commandBuffer, imageIndex);
        commandBuffer.bindPipeline(
            pipeline->GetBindPoint(),
            pipeline->GetPipeline());
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            pipeline->GetPipelineLayout(),
            0,
            emitter.graphicsSets.at(imageIndex),
            nullptr);

        // sceneDepth 由 RenderGraph 的 pass 输入描述符集提供；只有 shader 反射
        // 真正声明了 set 3 时才绑定，避免改变无深度采样的旧管线契约。
        bool usesPassSet = false;
        for (const ShaderBinding& binding : pipeline->GetShaderBindings())
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
                pipeline->GetPipelineLayout(),
                PassSetIndex,
                descriptorSets[imageIndex][PassSetIndex],
                nullptr);
        }
        uint64_t drawnVertices = 0;
        uint32_t emittedDrawCalls = 1;
        if (skeletalMesh)
        {
            emittedDrawCalls = static_cast<uint32_t>(
                std::max<size_t>(
                    emitter.skeletalSections.size(), 1u));
            for (auto& section : emitter.skeletalSections)
            {
                if (imageIndex < section.materialSets.size())
                {
                    commandBuffer.bindDescriptorSets(
                        vk::PipelineBindPoint::eGraphics,
                        pipeline->GetPipelineLayout(),
                        kParticleMaterialSetIndex,
                        section.materialSets.at(imageIndex),
                        nullptr);
                }
                // 骨骼完整实例的顶点着色器按 set 2 的当前姿态 palette 蒙皮。
                if (imageIndex < section.paletteSets.size())
                {
                    commandBuffer.bindDescriptorSets(
                        vk::PipelineBindPoint::eGraphics,
                        pipeline->GetPipelineLayout(),
                        kParticleSkinSetIndex,
                        section.paletteSets.at(imageIndex),
                        nullptr);
                }
                const vk::DeviceSize vertexOffset = 0;
                commandBuffer.bindVertexBuffers(
                    0,
                    1,
                    &section.vertices.buffer,
                    &vertexOffset);
                commandBuffer.bindIndexBuffer(
                    section.indices.buffer,
                    0,
                    vk::IndexType::eUint32);
                commandBuffer.drawIndexed(
                    section.indexCount,
                    emitter.asset.maxParticles,
                    0,
                    0,
                    0);
                drawnVertices += ParticleMeshDrawnVertexCount(
                    section.indexCount,
                    emitter.asset.maxParticles);
            }
        }
        else if (staticSectionMesh)
        {
            emittedDrawCalls = static_cast<uint32_t>(
                emitter.staticSections.size());
            const vk::DeviceSize vertexOffset = 0;
            const vk::DeviceSize indirectStride =
                sizeof(vk::DrawIndexedIndirectCommand);
            for (size_t sectionIndex = 0;
                 sectionIndex < emitter.staticSections.size();
                 ++sectionIndex)
            {
                auto& section =
                    emitter.staticSections[sectionIndex];
                if (imageIndex < section.materialSets.size())
                {
                    commandBuffer.bindDescriptorSets(
                        vk::PipelineBindPoint::eGraphics,
                        pipeline->GetPipelineLayout(),
                        kParticleMaterialSetIndex,
                        section.materialSets.at(imageIndex),
                        nullptr);
                }
                commandBuffer.bindVertexBuffers(
                    0,
                    1,
                    &section.vertices.buffer,
                    &vertexOffset);
                commandBuffer.bindIndexBuffer(
                    section.indices.buffer,
                    0,
                    vk::IndexType::eUint32);
                if (emitter.indirectArgs.buffer)
                {
                    commandBuffer.drawIndexedIndirect(
                        emitter.indirectArgs.buffer,
                        static_cast<vk::DeviceSize>(sectionIndex) *
                            indirectStride,
                        1,
                        indirectStride);
                }
                else
                {
                    commandBuffer.drawIndexed(
                        section.indexCount,
                        emitter.asset.maxParticles,
                        0,
                        0,
                        0);
                }
                drawnVertices += ParticleMeshDrawnVertexCount(
                    section.indexCount,
                    emitter.asset.maxParticles);
            }
        }
        else if (mesh)
        {
            if (imageIndex < emitter.materialSets.size())
            {
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    pipeline->GetPipelineLayout(),
                    kParticleMaterialSetIndex,
                    emitter.materialSets.at(imageIndex),
                    nullptr);
            }
            const vk::DeviceSize vertexOffset = 0;
            commandBuffer.bindVertexBuffers(
                0,
                1,
                &emitter.meshVertices.buffer,
                &vertexOffset);
            commandBuffer.bindIndexBuffer(
                emitter.meshIndices.buffer,
                0,
                vk::IndexType::eUint32);
            if (emitter.indirectArgs.buffer)
            {
                commandBuffer.drawIndexedIndirect(
                    emitter.indirectArgs.buffer,
                    0,
                    1,
                    sizeof(vk::DrawIndexedIndirectCommand));
            }
            else
            {
                commandBuffer.drawIndexed(
                    emitter.meshIndexCount,
                    emitter.asset.maxParticles,
                    0,
                    0,
                    0);
            }
            drawnVertices = ParticleMeshDrawnVertexCount(
                emitter.meshIndexCount,
                emitter.asset.maxParticles);
        }
        else if (skeletalTriangle || skeletalBone)
        {
            const uint32_t verticesPerParticle =
                skeletalTriangle ? 3u : 6u;
            if (imageIndex < emitter.skeletalSections.front()
                    .materialSets.size())
            {
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    pipeline->GetPipelineLayout(),
                    kParticleMaterialSetIndex,
                    emitter.skeletalSections.front()
                        .materialSets.at(imageIndex),
                    nullptr);
            }
            if (emitter.indirectArgs.buffer)
            {
                commandBuffer.drawIndirect(
                    emitter.indirectArgs.buffer,
                    0,
                    1,
                    sizeof(vk::DrawIndirectCommand));
            }
            else
            {
                commandBuffer.draw(
                    verticesPerParticle,
                    emitter.asset.maxParticles,
                    0,
                    0);
            }
            drawnVertices =
                static_cast<uint64_t>(verticesPerParticle) *
                emitter.asset.maxParticles;
        }
        else
        {
            const bool ribbon =
                emitter.asset.renderMode == "ribbon";
            const uint32_t verticesPerParticle = ribbon
                ? ParticleRibbonVerticesPerParticle(
                    emitter.asset.ribbonSegments)
                : 6u;
            if (beam)
            {
                // beam 的实例数来自 count * (1 + branchCount)，而不是存活粒子数；必须绕过
                // indirect/finalize，否则零粒子发射器会被画成 0 条弧。
                const uint32_t segments =
                    ClampParticleBeamSegments(
                        emitter.asset.beam.segments);
                const uint32_t count = emitter.asset.beam.count;
                const uint32_t branchCount =
                    emitter.asset.beam.branchCount;
                const uint32_t totalInstances =
                    static_cast<uint32_t>(
                        ParticleBeamInstanceCount(
                            count,
                            branchCount));
                commandBuffer.draw(
                    segments * kParticleBeamVerticesPerSegment,
                    totalInstances,
                    0,
                    0);
                drawnVertices = ParticleBeamVertexCount(
                    segments,
                    count,
                    branchCount);
            }
            else if (emitter.indirectArgs.buffer)
            {
                commandBuffer.drawIndirect(
                    emitter.indirectArgs.buffer,
                    0,
                    1,
                    sizeof(vk::DrawIndirectCommand));
                // indirect 的 instanceCount 由 GPU 改写；统计仍沿用本帧
                // 上界，与 direct fallback 保持一致，不能漏记成 0。
                drawnVertices =
                    static_cast<uint64_t>(verticesPerParticle) *
                    emitter.asset.maxParticles;
            }
            else
            {
                commandBuffer.draw(
                    verticesPerParticle,
                    emitter.asset.maxParticles,
                    0,
                    0);
                drawnVertices =
                    static_cast<uint64_t>(verticesPerParticle) *
                    emitter.asset.maxParticles;
            }
        }
        emitter.gpuTimer.EndPass(commandBuffer, imageIndex);
        frameStats.estimatedDrawCalls += emittedDrawCalls;
        frameStats.estimatedDrawnVertices += drawnVertices;
    };

    std::unordered_set<std::string> orderedKeys;
    orderedKeys.reserve(drawOrder.size());
    for (const std::string& key : drawOrder)
    {
        // 全局池在排序表里占一个特殊 item；轮到它时一次性画完池内
        // 所有已 gather 的粒子，而不是固定拖到所有 emitter 之后。
        if (key == kParticleGlobalDrawKey)
        {
            RecordGlobalDraw(commandBuffer, imageIndex, renderPass);
            continue;
        }
        orderedKeys.insert(key);
        const auto emitterIt = emitters.find(key);
        if (emitterIt != emitters.end())
        {
            recordEmitter(*emitterIt->second);
        }
    }

    // Synchronize 可能在本帧排序后增删发射器；未进入排序表的池按原 map
    // 顺序补画，保证它们不会因为 drawOrder 短暂失配而消失。
    for (auto& [key, emitterPtr] : emitters)
    {
        if (orderedKeys.find(key) == orderedKeys.end())
        {
            recordEmitter(*emitterPtr);
        }
    }
}

} // namespace VL
