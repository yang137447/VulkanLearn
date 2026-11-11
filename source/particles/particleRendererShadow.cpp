#include "particles/particleRendererInternal.h"

namespace VL
{

std::shared_ptr<PipelineBase>
ParticleRenderer::GetOrCreateShadowPipeline(
    const Renderpass& renderPass,
    ParticleShadowCasterMode mode)
{
    if (!initialized ||
        pipelineFactory == nullptr ||
        mode == ParticleShadowCasterMode::None)
    {
        return nullptr;
    }

    // 同一图里的 CSM、localShadow 层各自拥有 VkRenderPass 句柄；管线状态
    // 完全相同也必须按句柄分开缓存，否则会把 CSM 管线拿去绑 localShadow。
    // mesh/billboard/ribbon 的顶点输入与 shader 不同，因此句柄之外还要
    // 把 caster 几何类型编进 key。
    const uint64_t renderPassKey =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
            static_cast<VkRenderPass>(renderPass.renderPass)));
    const uint64_t cacheKey =
        (renderPassKey << 2) |
        static_cast<uint64_t>(mode);
    const auto existing = shadowPipelines.find(cacheKey);
    if (existing != shadowPipelines.end())
    {
        return existing->second;
    }

    const char* shaderName = nullptr;
    bool useVertexInput = false;
    switch (mode)
    {
    case ParticleShadowCasterMode::Mesh:
        shaderName = "particle/particleShadow";
        useVertexInput = true;
        break;
    case ParticleShadowCasterMode::Billboard:
        shaderName = "particle/particleBillboardShadow";
        break;
    case ParticleShadowCasterMode::Ribbon:
        shaderName = "particle/particleRibbonShadow";
        break;
    default:
        return nullptr;
    }

    ShaderVariantKey variant;
    variant.shaderName = shaderName;
    variant.renderMode = RenderMode::TransparentAdditive;
    variant.shadingModelMacro = "SHADING_MODEL_UNLIT";
    const GraphicsShaderVariantArtifact& artifact =
        pipelineFactory->PrepareGraphicsShaderVariant(variant);

    PassPipelineContractKey contract =
        renderPass.pipelineContractKey;
    // billboard/ribbon caster 由 gl_VertexIndex 生成四边形，没有顶点缓冲；
    // mesh caster 读取静态 SM 的顶点/索引流。
    contract.useVertexInput = useVertexInput;

    // caster shader 只声明 set 0；显式保留与可见粒子完全相同的 set 0
    // 合同（graphicsSets 就是用这份 layout 分配的），但不把 pass set 3
    // 带进阴影管线布局。
    GraphicsPipelineLayoutDesc layout = BuildParticleSharedLayout();
    layout.overrideSets[PassSetIndex] = false;
    layout.setBindings[PassSetIndex].clear();

    // PipelineFactory 的旧接口接受非 const 句柄指针；这里传一份副本，
    // 不把 render pass 的只读缓存暴露给可写接口。
    vk::RenderPass renderPassHandle = renderPass.renderPass;
    std::shared_ptr<PipelineBase> pipeline =
        pipelineFactory->CreateGraphicsPipeline(
            &renderPassHandle,
            contract,
            artifact,
            vk::CullModeFlagBits::eNone,
            GraphicsPipelineBlendMode::Opaque,
            layout);
    if (pipeline)
    {
        shadowPipelines.emplace(cacheKey, pipeline);
    }
    return pipeline;
}

ParticleShadowCasterMode ParticleRenderer::ResolveShadowCasterMode(
    const EmitterResources& emitter,
    uint32_t imageIndex,
    const Eigen::Vector3f& lightPosition,
    float farCm)
{
    const ParticleShadowCasterMode mode =
        ParticleShadowCasterModeFor(
            emitter.asset,
            emitter.meshFallbackBillboard);
    if (mode == ParticleShadowCasterMode::None)
    {
        return ParticleShadowCasterMode::None;
    }
    if (emitter.culled ||
        !emitter.visible ||
        !emitter.enabled ||
        emitter.frameUniforms.size() <= imageIndex ||
        emitter.graphicsSets.size() <= imageIndex)
    {
        return ParticleShadowCasterMode::None;
    }
    // 骨骼完整实例的几何在 section 缓冲里（meshVertices/meshIndices 为空），
    // 因此这里按 section 判定可用性，不能只看静态 mesh 缓冲。
    const bool skeletalGeometryValid =
        !emitter.skeletalSections.empty() &&
        emitter.meshIndexCount > 0;
    const bool staticSectionGeometryValid =
        !emitter.staticSections.empty() &&
        emitter.meshIndexCount > 0;
    if (mode == ParticleShadowCasterMode::Mesh &&
        !skeletalGeometryValid &&
        !staticSectionGeometryValid &&
        (emitter.meshIndexCount == 0 ||
         !emitter.meshVertices.buffer ||
         !emitter.meshIndices.buffer))
    {
        return ParticleShadowCasterMode::None;
    }
    if (!ParticleMeshCasterIntersectsShadowVolume(
            emitter.worldCenter,
            emitter.boundsRadius,
            lightPosition,
            farCm))
    {
        return ParticleShadowCasterMode::None;
    }
    return mode;
}

void ParticleRenderer::PrepareShadow(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Eigen::Matrix4f& viewProjection,
    int mode,
    int octant,
    const Eigen::Vector3f& lightPosition,
    float farCm,
    float biasCm)
{
    // RecordShadow 与 PrepareShadow 严格按 pass 交替调用；这里缓存本次
    // shadow 视图的剔除参数，避免绘制阶段再依赖已经更新的全局 UBO。
    shadowCullLightPosition = lightPosition;
    shadowCullFarCm = farCm;
    if (!initialized || emitters.empty())
    {
        return;
    }

    struct ShadowCasterUniforms
    {
        Eigen::Matrix4f viewProjection;
        Eigen::Vector4f parameters;
        Eigen::Vector4f lightPosition;
    };
    static_assert(
        sizeof(ShadowCasterUniforms) == 96,
        "Shadow caster UBO block must match particleCommon.glsl");
    static_assert(
        offsetof(FrameUniforms, shadowCasterParams) -
                offsetof(FrameUniforms, shadowViewProj) ==
            64,
        "shadowCasterParams must directly follow shadowViewProj");
    static_assert(
        offsetof(FrameUniforms, shadowLightPosition) -
                offsetof(FrameUniforms, shadowViewProj) ==
            80,
        "shadowLightPosition must directly follow shadowCasterParams");

    bool hasCaster = false;
    for (const auto& [key, emitter] : emitters)
    {
        (void)key;
        if (ResolveShadowCasterMode(
                *emitter,
                imageIndex,
                lightPosition,
                farCm) != ParticleShadowCasterMode::None)
        {
            hasCaster = true;
            break;
        }
    }
    if (!hasCaster)
    {
        return;
    }

    // 上一段 shadow draw 的顶点着色器可能仍在读同一 per-image UBO；
    // 有序 updateBuffer 之前必须先把 uniform read 与 transfer write 隔开。
    vk::MemoryBarrier readToTransfer;
    readToTransfer
        .setSrcAccessMask(vk::AccessFlagBits::eUniformRead)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eVertexShader,
        vk::PipelineStageFlagBits::eTransfer,
        {},
        readToTransfer,
        nullptr,
        nullptr);

    ShadowCasterUniforms uniforms{};
    uniforms.viewProjection = viewProjection;
    uniforms.parameters = Eigen::Vector4f(
        static_cast<float>(mode),
        static_cast<float>(octant),
        farCm,
        biasCm);
    uniforms.lightPosition = Eigen::Vector4f(
        lightPosition.x(),
        lightPosition.y(),
        lightPosition.z(),
        0.0f);
    for (auto& [key, emitter] : emitters)
    {
        (void)key;
        if (ResolveShadowCasterMode(
                *emitter,
                imageIndex,
                lightPosition,
                farCm) == ParticleShadowCasterMode::None)
        {
            continue;
        }
        commandBuffer.updateBuffer(
            emitter->frameUniforms.at(imageIndex).buffer,
            offsetof(FrameUniforms, shadowViewProj),
            sizeof(uniforms),
            &uniforms);
    }

    vk::MemoryBarrier transferToVertex;
    transferToVertex
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eUniformRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eVertexShader,
        {},
        transferToVertex,
        nullptr,
        nullptr);
}

void ParticleRenderer::RecordShadow(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass)
{
    if (!initialized || emitters.empty())
    {
        return;
    }
    // 同一 pass 内可能混有 mesh/billboard/ribbon caster。管线只在几何
    // 类型切换时重绑，descriptor set 仍按发射器绑定（每个发射器有自己
    // 的粒子池与纹理）。
    ParticleShadowCasterMode boundMode =
        ParticleShadowCasterMode::None;
    std::shared_ptr<PipelineBase> pipeline;
    for (auto& [key, emitterPtr] : emitters)
    {
        (void)key;
        EmitterResources& emitter = *emitterPtr;
        const ParticleShadowCasterMode mode =
            ResolveShadowCasterMode(
                emitter,
                imageIndex,
                shadowCullLightPosition,
                shadowCullFarCm);
        if (mode == ParticleShadowCasterMode::None)
        {
            continue;
        }

        if (mode != boundMode)
        {
            pipeline = GetOrCreateShadowPipeline(renderPass, mode);
            if (!pipeline)
            {
                continue;
            }
            commandBuffer.bindPipeline(
                pipeline->GetBindPoint(),
                pipeline->GetPipeline());
            boundMode = mode;
        }
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            pipeline->GetPipelineLayout(),
            0,
            emitter.graphicsSets.at(imageIndex),
            nullptr);

        if (mode == ParticleShadowCasterMode::Mesh &&
            !emitter.skeletalSections.empty())
        {
            // 骨骼 caster 逐 section 绘制；palette 已由 RecordSimulation 写入
            // 当前 image，阴影与可见网格看到同一姿态。
            const vk::DeviceSize vertexOffset = 0;
            for (auto& section : emitter.skeletalSections)
            {
                if (imageIndex < section.paletteSets.size())
                {
                    commandBuffer.bindDescriptorSets(
                        vk::PipelineBindPoint::eGraphics,
                        pipeline->GetPipelineLayout(),
                        kParticleSkinSetIndex,
                        section.paletteSets.at(imageIndex),
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
                        0,
                        1,
                        sizeof(vk::DrawIndexedIndirectCommand));
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
            }
        }
        else if (mode == ParticleShadowCasterMode::Mesh &&
            !emitter.staticSections.empty())
        {
            const vk::DeviceSize vertexOffset = 0;
            const vk::DeviceSize indirectStride =
                sizeof(vk::DrawIndexedIndirectCommand);
            for (size_t sectionIndex = 0;
                 sectionIndex < emitter.staticSections.size();
                 ++sectionIndex)
            {
                auto& section =
                    emitter.staticSections[sectionIndex];
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
            }
        }
        else if (mode == ParticleShadowCasterMode::Mesh)
        {
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
        }
        else if (emitter.indirectArgs.buffer)
        {
            // billboard 固定 6 顶点/实例，ribbon 为 segments*6；两者都由
            // 创建期写入的 indirect shape 决定顶点数，GPU finalize 只改
            // instanceCount。caster 用 aliveIndices，因此这个实例数正好
            // 对应存活粒子数。
            commandBuffer.drawIndirect(
                emitter.indirectArgs.buffer,
                0,
                1,
                sizeof(vk::DrawIndirectCommand));
        }
        else
        {
            commandBuffer.draw(
                emitter.indirectShape.vertexCount,
                emitter.asset.maxParticles,
                0,
                0);
        }
    }
}

void ParticleRenderer::RecordDistortion(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass)
{
    if (!initialized || !distortionPipeline)
    {
        return;
    }

    const auto& descriptorSets = renderPass.GetDescriptorSets();
    bool usesPassSet = false;
    for (const ShaderBinding& binding :
         distortionPipeline->GetShaderBindings())
    {
        if (binding.set == PassSetIndex)
        {
            usesPassSet = true;
            break;
        }
    }

    for (auto& [key, emitterPtr] : emitters)
    {
        (void)key;
        EmitterResources& emitter = *emitterPtr;
        if (emitter.culled ||
            emitter.asset.renderMode != "distortion" ||
            emitter.graphicsSets.size() <= imageIndex)
        {
            continue;
        }

        emitter.gpuTimer.BeginPass(commandBuffer, imageIndex);
        commandBuffer.bindPipeline(
            distortionPipeline->GetBindPoint(),
            distortionPipeline->GetPipeline());
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            distortionPipeline->GetPipelineLayout(),
            0,
            emitter.graphicsSets.at(imageIndex),
            nullptr);
        if (usesPassSet &&
            imageIndex < descriptorSets.size() &&
            PassSetIndex < descriptorSets[imageIndex].size())
        {
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eGraphics,
                distortionPipeline->GetPipelineLayout(),
                PassSetIndex,
                descriptorSets[imageIndex][PassSetIndex],
                nullptr);
        }

        // 与 billboard 相同：indirect 的 instanceCount 由 GPU finalize
        // 写入存活数；没有 indirect 缓冲时按池上限绘制，由 shader 丢弃死亡槽。
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
                6u,
                emitter.asset.maxParticles,
                0,
                0);
        }
        // 与 particle pass 的统计口径一致：indirect 的实例数由 GPU 改写，
        // 这里按池上限记录本帧上界；dump 也按同一公式估算顶点数。
        ++frameStats.estimatedDrawCalls;
        frameStats.estimatedDrawnVertices +=
            static_cast<uint64_t>(6u) * emitter.asset.maxParticles;
        emitter.gpuTimer.EndPass(commandBuffer, imageIndex);
    }
}

} // namespace VL
