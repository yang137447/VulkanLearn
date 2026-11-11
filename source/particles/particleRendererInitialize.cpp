#include "particles/particleRendererInternal.h"

namespace VL
{

void ParticleRenderer::EnsureInitialized(
    PipelineFactory& factory,
    RendererBackendVulkan& renderer,
    RenderGraph& graph)
{
    Renderpass* particlePass = FindParticlePass(graph);
    ParticleRenderIdentity nextIdentity;
    if (particlePass != nullptr)
    {
        nextIdentity = BuildParticleRenderIdentity(
            graph,
            *particlePass,
            FindDistortionPass(graph),
            renderer);
    }
    if (initialized && backend == &renderer &&
        renderIdentity == nextIdentity)
    {
        return;
    }

    if (initialized && backend != nullptr)
    {
        // 图身份变化只发生在 resize、图重载或启动阶段，不在普通帧循环里。
        // particle pipeline/renderPass 可能仍被已提交帧引用，因此这里等待
        // 设备空闲后立即释放；发射器增删走 retire queue，不会每帧等待。
        backend->WaitIdle();
        Shutdown(*backend);
    }
    Initialize(factory, renderer, graph);
}

void ParticleRenderer::Initialize(
    PipelineFactory& factory,
    RendererBackendVulkan& renderer,
    RenderGraph& graph)
{
    backend = &renderer;
    pipelineFactory = &factory;

    Renderpass* particlePass = FindParticlePass(graph);
    if (particlePass == nullptr)
    {
        // 图没有声明 particle pass 时保持未初始化；渲染路径只跳过粒子，
        // 不把缺少可选 pass 当成启动失败。
        renderIdentity = {};
        initialized = false;
        return;
    }
    Renderpass* distortionPass = FindDistortionPass(graph);

    renderIdentity = BuildParticleRenderIdentity(
        graph,
        *particlePass,
        distortionPass,
        renderer);
    simulatePipeline =
        factory.CreateComputePipeline("particle/particleSimulate");
    sortInitPipeline =
        factory.CreateComputePipeline("particle/particleSortInit");
    sortPipeline =
        factory.CreateComputePipeline("particle/particleSort");
    finalizeCountPipeline =
        factory.CreateComputePipeline("particle/particleFinalizeCount");
    globalGatherPipeline =
        factory.CreateComputePipeline("particle/particleGlobalGather");
    globalSortInitPipeline =
        factory.CreateComputePipeline("particle/particleGlobalSortInit");
    particleLightEmitPipeline =
        factory.CreateComputePipeline("particle/particleLightEmit");
    decalGatherPipeline =
        factory.CreateComputePipeline("particle/particleDecalGather");
    softDepthPipeline =
        factory.CreateComputePipeline("particle/particleSoftDepth");

    ShaderVariantKey variant;
    variant.shaderName = "particle/particle";
    variant.renderMode = RenderMode::TransparentAdditive;
    variant.shadingModelMacro = "SHADING_MODEL_UNLIT";
    const GraphicsShaderVariantArtifact& artifact =
        factory.PrepareGraphicsShaderVariant(variant);

    PassPipelineContractKey contract =
        particlePass->pipelineContractKey;
    contract.useVertexInput = false;
    // 深度测试改在片元里采样 sceneDepth；pass 不再写深度附件。
    contract.depthTestEnable = false;
    contract.depthWriteEnable = false;
    contract.depthCompareOp = vk::CompareOp::eLess;
    // 所有逐发射器图形管线共用这一份 set 0 显式合同；不能依赖各自的
    // shader 反射，否则未被静态使用的 binding 会被裁掉导致 layout 不一致。
    const GraphicsPipelineLayoutDesc sharedLayout =
        BuildParticleSharedLayout();
    additivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        artifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive,
        sharedLayout);
    alphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        artifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        sharedLayout);

    // ribbon 复用同一 pass、descriptor layout 与 particle SSBO，只替换
    // 顶点/片元 shader；顶点数按段数在 RecordGraphics 中决定。
    ShaderVariantKey ribbonVariant = variant;
    ribbonVariant.shaderName = "particle/particleRibbon";
    const GraphicsShaderVariantArtifact& ribbonArtifact =
        factory.PrepareGraphicsShaderVariant(ribbonVariant);
    ribbonAdditivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        ribbonArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive,
        sharedLayout);
    ribbonAlphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        ribbonArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        sharedLayout);

    // beam 同样不使用顶点输入；起止点、段数与噪声参数都来自 FrameUniforms。
    // 与 ribbon 使用同一 pass 合同和 descriptor set 0，避免额外描述符池。
    ShaderVariantKey beamVariant = variant;
    beamVariant.shaderName = "particle/particleBeam";
    const GraphicsShaderVariantArtifact& beamArtifact =
        factory.PrepareGraphicsShaderVariant(beamVariant);
    beamAdditivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        beamArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive,
        sharedLayout);
    beamAlphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        beamArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        sharedLayout);

    // distortion 自己写独立附件，因此必须使用 distortion pass 的 VkRenderPass；
    // set 0 复用共享合同；distortion pass 的图输入只有 sceneDepth，所以 set 3
    // 必须去掉共享合同里的 shadowMap/localShadowMap，否则 layout 与图不一致。
    if (distortionPass != nullptr)
    {
        GraphicsPipelineLayoutDesc distortionLayout = sharedLayout;
        distortionLayout.setBindings[PassSetIndex] = {
            MakeParticlePassSetBinding(0, "sceneDepth"),
            MakeParticlePassSetBinding(3, "softDepth")};

        ShaderVariantKey distortionVariant = variant;
        distortionVariant.shaderName = "particle/particleDistortion";
        const GraphicsShaderVariantArtifact& distortionArtifact =
            factory.PrepareGraphicsShaderVariant(distortionVariant);
        PassPipelineContractKey distortionContract =
            distortionPass->pipelineContractKey;
        distortionContract.useVertexInput = false;
        distortionContract.depthTestEnable = false;
        distortionContract.depthWriteEnable = false;
        distortionContract.depthCompareOp = vk::CompareOp::eLess;
        distortionPipeline = factory.CreateGraphicsPipeline(
            &distortionPass->renderPass,
            distortionContract,
            distortionArtifact,
            vk::CullModeFlagBits::eNone,
            GraphicsPipelineBlendMode::Additive,
            distortionLayout);
    }

    // mesh 粒子读取静态 SM 的 Vertex stream，因此同一条 particle pass
    // 需要使用带 vertex input 的合同；descriptor set 仍与 billboard 相同。
    ShaderVariantKey meshVariant = variant;
    meshVariant.shaderName = "particle/particleMesh";
    const GraphicsShaderVariantArtifact& meshArtifact =
        factory.PrepareGraphicsShaderVariant(meshVariant);
    PassPipelineContractKey meshContract =
        particlePass->pipelineContractKey;
    meshContract.useVertexInput = true;
    meshContract.depthTestEnable = false;
    meshContract.depthWriteEnable = false;
    meshContract.depthCompareOp = vk::CompareOp::eLess;
    // mesh 独占材质 set 1；billboard/ribbon/beam 的共享 layout 不变。
    const GraphicsPipelineLayoutDesc meshLayout =
        BuildParticleMeshLayout();
    meshAdditivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        meshContract,
        meshArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive,
        meshLayout);
    // 骨骼完整实例：同一份 mesh shader，但打开 VL_SKINNED_MESH 并追加
    // set 2 的 palette。骨骼资产无论作者写 mesh 还是骨骼粒子模式都走这条
    // 管线；skeletalMeshTris/Bones 是无顶点输入的独立管线。
    ShaderVariantKey skeletalMeshVariant = meshVariant;
    skeletalMeshVariant.macros.push_back("VL_SKINNED_MESH=1");
    const GraphicsShaderVariantArtifact& skeletalMeshArtifact =
        factory.PrepareGraphicsShaderVariant(skeletalMeshVariant);
    const GraphicsPipelineLayoutDesc skeletalMeshLayout =
        BuildParticleSkeletalLayout();
    skeletalMeshAdditivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        meshContract,
        skeletalMeshArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive,
        skeletalMeshLayout);
    skeletalMeshAlphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        meshContract,
        skeletalMeshArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        skeletalMeshLayout);
    meshAlphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        meshContract,
        meshArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        meshLayout);

    // SkeletalMeshTris/Bones 不读取顶点缓冲，逐粒子用 3/6 个顶点展开。
    PassPipelineContractKey skeletalContract =
        particlePass->pipelineContractKey;
    skeletalContract.useVertexInput = false;
    skeletalContract.depthTestEnable = false;
    skeletalContract.depthWriteEnable = false;
    skeletalContract.depthCompareOp = vk::CompareOp::eLess;
    ShaderVariantKey skeletalTriangleVariant = variant;
    skeletalTriangleVariant.shaderName =
        "particle/particleSkeletalTriangle";
    const GraphicsShaderVariantArtifact& skeletalTriangleArtifact =
        factory.PrepareGraphicsShaderVariant(skeletalTriangleVariant);
    skeletalTriangleAdditivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        skeletalContract,
        skeletalTriangleArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive,
        meshLayout);
    skeletalTriangleAlphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        skeletalContract,
        skeletalTriangleArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        meshLayout);
    ShaderVariantKey skeletalBoneVariant = variant;
    skeletalBoneVariant.shaderName = "particle/particleSkeletalBone";
    const GraphicsShaderVariantArtifact& skeletalBoneArtifact =
        factory.PrepareGraphicsShaderVariant(skeletalBoneVariant);
    skeletalBoneAdditivePipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        skeletalContract,
        skeletalBoneArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::Additive,
        meshLayout);
    skeletalBoneAlphaPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        skeletalContract,
        skeletalBoneArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        meshLayout);

    // 跨发射器 alpha billboard 的统一绘制管线：无顶点输入，读取
    // gathered 记录 + 全局排序 pair 表 + 8 槽纹理数组。
    ShaderVariantKey globalVariant = variant;
    globalVariant.shaderName = "particle/particleGlobal";
    const GraphicsShaderVariantArtifact& globalArtifact =
        factory.PrepareGraphicsShaderVariant(globalVariant);
    globalDrawPipeline = factory.CreateGraphicsPipeline(
        &particlePass->renderPass,
        contract,
        globalArtifact,
        vk::CullModeFlagBits::eNone,
        GraphicsPipelineBlendMode::AlphaBlend,
        BuildParticleGlobalLayout());

    // 无纹理发射器仍需一个合法 sampler；1x1 白图让 shader 走程序化形状分支。
    HostImage whiteImage;
    whiteImage.width = 1;
    whiteImage.height = 1;
    whiteImage.channels = 4;
    whiteImage.format = HostImage::PixelFormat::RGBA8_UNORM;
    whiteImage.semantic = HostImage::TextureSemantic::Color;
    whiteImage.rowStrideBytes = 4;
    whiteImage.data = {255, 255, 255, 255};
    Texture::CreateDesc whiteDesc;
    whiteDesc.semantic = HostImage::TextureSemantic::Color;
    whiteDesc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    whiteDesc.generateMipmaps = false;
    whiteDesc.filter = vk::Filter::eLinear;
    whiteDesc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    whiteDesc.debugName = "ParticleDefaultWhite";
    defaultTexture = std::make_shared<Texture>(
        renderer,
        whiteImage,
        whiteDesc);
    // mesh 材质缺失槽位的常量默认值。flat normal 必须是 (0,0,1)，
    // 否则缺失 normalMap 时会把几何法线整体旋转。
    defaultNormalTexture = CreateParticleSolidTexture(
        renderer,
        {128, 128, 255, 255},
        HostImage::PixelFormat::RGBA8_UNORM,
        HostImage::TextureSemantic::Normal,
        "ParticleDefaultNormal");
    defaultOrmTexture = CreateParticleSolidTexture(
        renderer,
        {255, 255, 255, 255},
        HostImage::PixelFormat::RGBA8_UNORM,
        HostImage::TextureSemantic::Data,
        "ParticleDefaultOrm");
    defaultEmissiveTexture = CreateParticleSolidTexture(
        renderer,
        {0, 0, 0, 255},
        HostImage::PixelFormat::RGBA8_UNORM,
        HostImage::TextureSemantic::Color,
        "ParticleDefaultEmissive");
    HostImage iesImage;
    iesImage.width = 64;
    iesImage.height = 16;
    iesImage.channels = 4;
    iesImage.format = HostImage::PixelFormat::RGBA8_UNORM;
    iesImage.semantic = HostImage::TextureSemantic::Data;
    iesImage.rowStrideBytes = 64 * 4;
    iesImage.data.assign(64 * 16 * 4, 255);
    Texture::CreateDesc iesDesc;
    iesDesc.semantic = HostImage::TextureSemantic::Data;
    iesDesc.transfer = TextureIO::LoadOptions::Transfer::Linear;
    iesDesc.generateMipmaps = false;
    iesDesc.filter = vk::Filter::eLinear;
    iesDesc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
    iesDesc.debugName = "ParticleIesLutDefault";
    iesLutTexture = std::make_shared<Texture>(
        renderer, iesImage, iesDesc);
    // 场景光照 descriptor 的兜底资源必须在第一个发射器创建前就绪。
    CreateFallbackLightingResources();
    EnsureTranslucencyVolume();
    initialized = true;
}

void ParticleRenderer::Shutdown(RendererBackendVulkan& renderer)
{
    frameStats = ParticleFrameStats{};
    if (backend == nullptr)
    {
        return;
    }
    // 调用方必须已等待设备空闲（renderer.WaitIdle 或引擎关闭路径）；
    // 活跃发射器可以立即析构，已退休的包仍由 ResourceRetireQueue 持有。
    emitters.clear();
    particleLightBuffers.clear();
    particleLightBufferGeneration = 0;
    sceneLightInfos.clear();
    sceneParticleLightInfos.clear();
    environmentCubeDescriptor = vk::DescriptorImageInfo{};
    brdfLutDescriptor = vk::DescriptorImageInfo{};
    sceneLightingGeneration = 0;
    fallbackSceneLightInfo = vk::DescriptorBufferInfo{};
    fallbackParticleLightInfo = vk::DescriptorBufferInfo{};
    DestroyFallbackLightingResources();
    DestroyTranslucencyVolume();
    DestroySoftDepthResources();
    warnedMissingParticleLightBuffer = false;
    textureCache.clear();
    DestroyGlobalSortPool();
    simulatePipeline.reset();
    sortInitPipeline.reset();
    sortPipeline.reset();
    finalizeCountPipeline.reset();
    globalGatherPipeline.reset();
    globalSortInitPipeline.reset();
    particleLightEmitPipeline.reset();
    decalGatherPipeline.reset();
    softDepthPipeline.reset();
    additivePipeline.reset();
    alphaPipeline.reset();
    ribbonAdditivePipeline.reset();
    ribbonAlphaPipeline.reset();
    beamAdditivePipeline.reset();
    beamAlphaPipeline.reset();
    distortionPipeline.reset();
    meshAdditivePipeline.reset();
    meshAlphaPipeline.reset();
    skeletalMeshAdditivePipeline.reset();
    skeletalMeshAlphaPipeline.reset();
    skeletalTriangleAdditivePipeline.reset();
    skeletalTriangleAlphaPipeline.reset();
    skeletalBoneAdditivePipeline.reset();
    skeletalBoneAlphaPipeline.reset();
    shadowPipelines.clear();
    pipelineFactory = nullptr;
    globalDrawPipeline.reset();
    defaultTexture.reset();
    defaultNormalTexture.reset();
    defaultOrmTexture.reset();
    defaultEmissiveTexture.reset();
    iesLutTexture.reset();
    drawOrder.clear();
    decalGpuInstances.clear();
    renderIdentity = {};
    initialized = false;
    backend = nullptr;
    (void)renderer;
}

void ParticleRenderer::InvalidatePipelines(
    RendererBackendVulkan& renderer)
{
    frameStats = ParticleFrameStats{};
    if (backend == nullptr || backend != &renderer)
    {
        return;
    }
    // reload 提交后没有尚未完成的粒子工作；等待设备空闲再走 Shutdown，
    // 避免释放仍被已提交帧引用的 pipeline/renderPass。延迟退休队列里的
    // 旧发射器包仍由队列按 epoch 持有，不会在这里提前析构。
    backend->WaitIdle();
    Shutdown(renderer);
}

} // namespace VL
