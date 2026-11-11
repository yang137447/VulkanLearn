#include "particles/particleRendererInternal.h"

namespace VL
{

namespace
{


// 骨骼网格粒子资源构建期间使用的 CPU 蒙皮参考。palette/matrix 语义与
// particleSkeletalSkin.glsl 完全一致：p = Σ w · (model_j · inverseBind_j) · p_bind。
template <typename Section>
void SkinParticleSkeletalSection(
    Section& section,
    const std::vector<Eigen::Matrix4f>& models)
{
    section.skinnedVertices = section.sourceVertices;
    for (size_t vertexIndex = 0;
         vertexIndex < section.sourceVertices.size();
         ++vertexIndex)
    {
        Eigen::Matrix4f skin = Eigen::Matrix4f::Zero();
        const uint32_t begin = section.influenceOffsets.at(vertexIndex);
        const uint32_t end = section.influenceOffsets.at(vertexIndex + 1);
        for (uint32_t index = begin; index < end; ++index)
        {
            const auto& influence = section.influences.at(index);
            if (influence.joint >= models.size() ||
                influence.joint >= section.inverseBinds.size())
            {
                continue;
            }
            skin += influence.weight *
                models[influence.joint] *
                section.inverseBinds[influence.joint];
        }
        Vertex& target = section.skinnedVertices[vertexIndex];
        const Vertex& source = section.sourceVertices[vertexIndex];
        target.position =
            (skin * Eigen::Vector4f(
                source.position.x(),
                source.position.y(),
                source.position.z(),
                1.0f)).head<3>();
        const Eigen::Matrix3f linear = skin.block<3, 3>(0, 0);
        target.normal = (linear.inverse().transpose() * source.normal).normalized();
        Eigen::Vector3f tangent = linear * source.tangent.head<3>();
        tangent -= target.normal * target.normal.dot(tangent);
        if (tangent.squaredNorm() > 1.0e-10f)
        {
            target.tangent.head<3>() = tangent.normalized();
        }
    }
}

ParticleMaterialParams LoadParticleMaterialInstance(
    const std::string& path,
    std::string& outError)
{
    ParticleMaterialParams params;
    try
    {
        RequireValidParticleMeshPath(path, "materialInstancePath");
        std::ifstream file(CommonFunction::Path(path));
        if (!file.is_open())
        {
            outError = "file not found";
            return params;
        }
        nlohmann::json json;
        file >> json;
        params = ParseParticleMaterialInstanceJson(json);
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
    }
    return params;
}

} // namespace

void ParticleRenderer::CreateEmitter(
    const ParticleEmitterAsset& asset,
    const Eigen::Matrix4f& model,
    const std::string& key,
    bool visible,
    bool enabled,
    float spawnRateScale,
    const Eigen::Vector4f& colorTint)
{
    std::shared_ptr<EmitterResources> emitter(
        new EmitterResources(),
        [rendererBackend = backend](EmitterResources* resources)
        {
            DestroyEmitterResources(*rendererBackend, *resources);
            delete resources;
        });
    emitter->key = key;
    emitter->asset = asset;
    emitter->model = model;
    emitter->visible = visible;
    emitter->enabled = enabled;
    emitter->spawnRateScale = spawnRateScale;
    emitter->colorTint = colorTint;
    emitter->textureSourceKey =
        BuildEmitterTextureSourceKey(asset);
    emitter->meshSourceKey =
        BuildEmitterMeshSourceKey(asset);
    emitter->meshScale = asset.meshScale;
    emitter->meshAlignment = asset.meshAlignment;
    emitter->flipbookTable = BuildParticleFlipbookGpuTable(asset);
    if (asset.renderMode == "mesh" &&
        !asset.materialInstancePath.empty())
    {
        std::string materialError;
        emitter->material = LoadParticleMaterialInstance(
            asset.materialInstancePath, materialError);
        if (!emitter->material.valid)
        {
            WarnMaterialFallbackOnce(
                key,
                asset.materialInstancePath,
                materialError.empty()
                    ? "material instance parse failed"
                    : materialError);
        }
    }
    if (asset.renderMode == "mesh")
    {
        LoadEmitterMaterialTextures(*emitter, key);
    }
    emitter->elapsedSeconds = 0.0f;
    emitter->needsClear = true;
    emitter->prewarm = true;
    if (asset.texturePath.empty())
    {
        emitter->texture = defaultTexture;
    }
    else
    {
        const bool generateMipmaps =
            asset.flipbookColumns * asset.flipbookRows <= 1;
        auto cached = textureCache.find(emitter->textureSourceKey);
        if (cached != textureCache.end())
        {
            emitter->texture = cached->second.lock();
            if (!emitter->texture)
                textureCache.erase(cached);
        }
        if (!emitter->texture)
        {
            try
            {
                TextureIO::LoadOptions options;
                options.semantic = HostImage::TextureSemantic::Color;
                options.flipY = TextureIO::LoadOptions::FlipYMode::ForceOff;
                // distortion 是方向/覆盖率数据纹理，不能走 sRGB 解码，
                // 否则作者化的 0.5 中点会变成约 0.214。
                const bool dataTexture =
                    asset.renderMode == "distortion";
                options.semantic = dataTexture
                    ? HostImage::TextureSemantic::Data
                    : HostImage::TextureSemantic::Color;
                options.transfer = dataTexture
                    ? TextureIO::LoadOptions::Transfer::Linear
                    : TextureIO::LoadOptions::Transfer::SRGB;
                options.forceChannels = 4;
                const std::optional<HostImage> cpuImage =
                    TextureIO::Load(
                        CommonFunction::Path(asset.texturePath),
                        options);
                if (!cpuImage.has_value())
                {
                    throw std::runtime_error(
                        "texel decoder returned no image");
                }
                Texture::CreateDesc desc;
                desc.semantic = options.semantic;
                desc.transfer = options.transfer;
                // 多帧图集不能生成 mipmap：低 mip 会把相邻 cell 混在一起，
                // 线过滤再叠加半 texel 内缩即可避免帧间渗色。单帧纹理保留原行为。
                desc.generateMipmaps = generateMipmaps;
                desc.filter = vk::Filter::eLinear;
                desc.wrapMode = vk::SamplerAddressMode::eClampToEdge;
                desc.debugName = "ParticleTexture:" + asset.texturePath;
                emitter->texture = std::make_shared<Texture>(
                    *backend,
                    *cpuImage,
                    desc);
                // 同路径+文件版本+mipmap 配置的活跃纹理可被多个发射器共享。
                textureCache[emitter->textureSourceKey] =
                    emitter->texture;
            }
            catch (const std::exception& exception)
            {
                WarnTextureFallbackOnce(
                    key,
                    asset.texturePath,
                    exception.what());
                emitter->texture = defaultTexture;
                emitter->textureFallback = true;
            }
            catch (...)
            {
                WarnTextureFallbackOnce(
                    key,
                    asset.texturePath,
                    "unknown texture load failure");
                emitter->texture = defaultTexture;
                emitter->textureFallback = true;
            }
        }
    }
    LoadEmitterMesh(*emitter, key);
    ComputeEmitterBounds(
        asset,
        model,
        emitter->worldCenter,
        emitter->boundsRadius,
        emitter->meshLocalBoundsRadius);
    CreateBuffers(*emitter);
    CreateDescriptors(*emitter);
    emitter->gpuTimer.Initialize(*backend);
    emitters.emplace(key, std::move(emitter));
}

void ParticleRenderer::LoadEmitterMesh(
    EmitterResources& emitter,
    const std::string& emitterKey)
{
    if (emitter.asset.renderMode != "mesh" &&
        emitter.asset.renderMode != "skeletalMeshTris" &&
        emitter.asset.renderMode != "skeletalMeshBones")
    {
        return;
    }
    try
    {
        // v1 只接受静态网格：骨骼动画需要 skinning vertex factory，
        // 与粒子实例化不兼容。
        MeshAssetLoadRequest request =
            MeshAssetLoader::Prepare(emitter.asset.meshPath);
        if (request.buildPlan.IsSkeletal())
        {
            const MeshAssetImportResult imported =
                MeshAssetLoader::ImportSections(request);
            LoadSkeletalEmitterMesh(
                emitter, emitterKey, imported);
            return;
        }
        const MeshAssetImportResult imported =
            MeshAssetLoader::ImportSections(request);

        // G2：sectionPlans 与几何 section 一一对应时保留每 section 几何与
        // 材质；没有 sectionPlans 的旧资产继续走下方合并 fallback。
        const bool useSectionPlans =
            !imported.sectionPlans.empty() &&
            imported.sectionPlans.size() ==
                imported.modelResource.sections.size() &&
            imported.sectionPlans.size() <= 64;
        if (useSectionPlans)
        {
            for (size_t sectionIndex = 0;
                 sectionIndex < imported.modelResource.sections.size();
                 ++sectionIndex)
            {
                const MeshSection& meshSection =
                    imported.modelResource.sections[sectionIndex];
                if (meshSection.vertices.empty() ||
                    meshSection.indices.empty())
                {
                    continue;
                }
                for (uint32_t index : meshSection.indices)
                {
                    if (index >= meshSection.vertices.size())
                    {
                        throw std::runtime_error(
                            "mesh section index is out of range");
                    }
                }

                EmitterResources::StaticSection section;
                const vk::DeviceSize vertexBytes =
                    static_cast<vk::DeviceSize>(
                        meshSection.vertices.size() * sizeof(Vertex));
                const auto vertexAllocation = backend->CreateBuffer(
                    vertexBytes,
                    vk::BufferUsageFlagBits::eVertexBuffer,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "ParticleMeshVertices:" + emitterKey + "/" +
                        std::to_string(sectionIndex));
                section.vertices.buffer = vertexAllocation.first;
                section.vertices.memory = vertexAllocation.second;
                void* mappedVertices = backend->MapMemory(
                    section.vertices.memory, vertexBytes);
                if (mappedVertices == nullptr)
                {
                    throw std::runtime_error(
                        "mesh section vertex map failed");
                }
                std::memcpy(
                    mappedVertices,
                    meshSection.vertices.data(),
                    static_cast<size_t>(vertexBytes));
                backend->UnmapMemory(section.vertices.memory);

                const vk::DeviceSize indexBytes =
                    static_cast<vk::DeviceSize>(
                        meshSection.indices.size() * sizeof(uint32_t));
                const auto indexAllocation = backend->CreateBuffer(
                    indexBytes,
                    vk::BufferUsageFlagBits::eIndexBuffer,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "ParticleMeshIndices:" + emitterKey + "/" +
                        std::to_string(sectionIndex));
                section.indices.buffer = indexAllocation.first;
                section.indices.memory = indexAllocation.second;
                void* mappedIndices = backend->MapMemory(
                    section.indices.memory, indexBytes);
                if (mappedIndices == nullptr)
                {
                    throw std::runtime_error(
                        "mesh section index map failed");
                }
                std::memcpy(
                    mappedIndices,
                    meshSection.indices.data(),
                    static_cast<size_t>(indexBytes));
                backend->UnmapMemory(section.indices.memory);

                for (const Vertex& vertex : meshSection.vertices)
                {
                    const Eigen::Vector3f scaled(
                        vertex.position.x() *
                            emitter.asset.meshScale.x(),
                        vertex.position.y() *
                            emitter.asset.meshScale.y(),
                        vertex.position.z() *
                            emitter.asset.meshScale.z());
                    section.localBoundsRadius = std::max(
                        section.localBoundsRadius, scaled.norm());
                }
                section.indexCount = static_cast<uint32_t>(
                    meshSection.indices.size());
                emitter.meshIndexCount += section.indexCount;
                emitter.meshLocalBoundsRadius = std::max(
                    emitter.meshLocalBoundsRadius,
                    section.localBoundsRadius);

                std::string materialPath =
                    imported.sectionPlans[sectionIndex]
                        .materialInstancePath;
                if (materialPath.empty())
                {
                    materialPath =
                        emitter.asset.materialInstancePath;
                }
                if (!materialPath.empty())
                {
                    std::string materialError;
                    section.material = LoadParticleMaterialInstance(
                        materialPath, materialError);
                    if (!section.material.valid &&
                        !materialError.empty())
                    {
                        WarnMaterialFallbackOnce(
                            emitterKey + "#" +
                                std::to_string(sectionIndex),
                            materialPath,
                            materialError);
                    }
                }
                section.albedo = ResolveMaterialTexture(
                    emitterKey + "#" +
                        std::to_string(sectionIndex),
                    section.material.albedoMap,
                    HostImage::TextureSemantic::Color,
                    defaultTexture);
                section.normal = ResolveMaterialTexture(
                    emitterKey + "#" +
                        std::to_string(sectionIndex),
                    section.material.normalMap,
                    HostImage::TextureSemantic::Normal,
                    defaultNormalTexture);
                section.orm = ResolveMaterialTexture(
                    emitterKey + "#" +
                        std::to_string(sectionIndex),
                    section.material.ormMap,
                    HostImage::TextureSemantic::Data,
                    defaultOrmTexture);
                section.emissive = ResolveMaterialTexture(
                    emitterKey + "#" +
                        std::to_string(sectionIndex),
                    section.material.emissiveMap,
                    HostImage::TextureSemantic::Color,
                    defaultEmissiveTexture);

                const auto materialAllocation = backend->CreateBuffer(
                    sizeof(ParticleMaterialUniforms),
                    vk::BufferUsageFlagBits::eUniformBuffer,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "ParticleMeshMaterial:" + emitterKey + "/" +
                        std::to_string(sectionIndex));
                section.materialUniform.buffer =
                    materialAllocation.first;
                section.materialUniform.memory =
                    materialAllocation.second;
                void* mappedMaterial = backend->MapMemory(
                    section.materialUniform.memory,
                    sizeof(ParticleMaterialUniforms));
                if (mappedMaterial == nullptr)
                {
                    throw std::runtime_error(
                        "mesh section material map failed");
                }
                const ParticleMaterialParams& params =
                    section.material;
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
                backend->UnmapMemory(section.materialUniform.memory);
                emitter.staticSections.push_back(
                    std::move(section));
            }
            if (emitter.staticSections.empty())
            {
                throw std::runtime_error(
                    "mesh has no non-empty sections");
            }
            emitter.meshFallbackBillboard = false;
            return;
        }

        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        for (const MeshSection& section : imported.modelResource.sections)
        {
            if (section.vertices.empty() || section.indices.empty())
            {
                continue;
            }
            if (vertices.size() + section.vertices.size() >
                static_cast<size_t>(
                    std::numeric_limits<uint32_t>::max()))
            {
                throw std::runtime_error(
                    "mesh has too many vertices");
            }
            const uint32_t baseVertex =
                static_cast<uint32_t>(vertices.size());
            vertices.insert(
                vertices.end(),
                section.vertices.begin(),
                section.vertices.end());
            indices.reserve(indices.size() + section.indices.size());
            for (const uint32_t index : section.indices)
            {
                if (index >= section.vertices.size())
                {
                    throw std::runtime_error(
                        "mesh section index is out of range");
                }
                indices.push_back(baseVertex + index);
            }
        }
        if (vertices.empty() || indices.empty())
        {
            throw std::runtime_error(
                "mesh has no non-empty sections");
        }

        const vk::DeviceSize vertexBytes =
            static_cast<vk::DeviceSize>(
                vertices.size() * sizeof(Vertex));
        const auto vertexAllocation = backend->CreateBuffer(
            vertexBytes,
            vk::BufferUsageFlagBits::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleMeshVertices:" + emitterKey);
        emitter.meshVertices.buffer = vertexAllocation.first;
        emitter.meshVertices.memory = vertexAllocation.second;
        void* mappedVertices = backend->MapMemory(
            emitter.meshVertices.memory,
            vertexBytes);
        if (mappedVertices == nullptr)
        {
            throw std::runtime_error("mesh vertex map failed");
        }
        std::memcpy(
            mappedVertices,
            vertices.data(),
            static_cast<size_t>(vertexBytes));
        backend->UnmapMemory(emitter.meshVertices.memory);

        const vk::DeviceSize indexBytes =
            static_cast<vk::DeviceSize>(
                indices.size() * sizeof(uint32_t));
        const auto indexAllocation = backend->CreateBuffer(
            indexBytes,
            vk::BufferUsageFlagBits::eIndexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "ParticleMeshIndices:" + emitterKey);
        emitter.meshIndices.buffer = indexAllocation.first;
        emitter.meshIndices.memory = indexAllocation.second;
        void* mappedIndices = backend->MapMemory(
            emitter.meshIndices.memory,
            indexBytes);
        if (mappedIndices == nullptr)
        {
            throw std::runtime_error("mesh index map failed");
        }
        std::memcpy(
            mappedIndices,
            indices.data(),
            static_cast<size_t>(indexBytes));
        backend->UnmapMemory(emitter.meshIndices.memory);

        float localRadius = 0.0f;
        for (const Vertex& vertex : vertices)
        {
            const Eigen::Vector3f scaled(
                vertex.position.x() * emitter.asset.meshScale.x(),
                vertex.position.y() * emitter.asset.meshScale.y(),
                vertex.position.z() * emitter.asset.meshScale.z());
            localRadius = std::max(localRadius, scaled.norm());
        }
        emitter.meshIndexCount =
            static_cast<uint32_t>(indices.size());
        emitter.meshLocalBoundsRadius = localRadius;
        emitter.meshFallbackBillboard = false;
    }
    catch (const std::exception& exception)
    {
        DestroyBuffer(*backend, emitter.meshVertices);
        DestroyBuffer(*backend, emitter.meshIndices);
        for (auto& section : emitter.staticSections)
        {
            DestroyBuffer(*backend, section.vertices);
            DestroyBuffer(*backend, section.indices);
            DestroyBuffer(*backend, section.materialUniform);
        }
        emitter.staticSections.clear();
        emitter.meshIndexCount = 0;
        emitter.meshLocalBoundsRadius = 0.0f;
        emitter.meshFallbackBillboard = true;
        WarnMeshFallbackOnce(
            emitterKey,
            emitter.asset.meshPath,
            exception.what());
    }
    catch (...)
    {
        DestroyBuffer(*backend, emitter.meshVertices);
        DestroyBuffer(*backend, emitter.meshIndices);
        for (auto& section : emitter.staticSections)
        {
            DestroyBuffer(*backend, section.vertices);
            DestroyBuffer(*backend, section.indices);
            DestroyBuffer(*backend, section.materialUniform);
        }
        emitter.staticSections.clear();
        emitter.meshIndexCount = 0;
        emitter.meshLocalBoundsRadius = 0.0f;
        emitter.meshFallbackBillboard = true;
        WarnMeshFallbackOnce(
            emitterKey,
            emitter.asset.meshPath,
            "unknown mesh load failure");
    }
}

void ParticleRenderer::LoadSkeletalEmitterMesh(
    EmitterResources& emitter,
    const std::string& emitterKey,
    const MeshAssetImportResult& imported)
{
    using namespace VL::Animation;
    if (!imported.modelResource.skeletalAsset)
    {
        throw std::runtime_error(
            "skeletal mesh import did not return a SkeletalAsset");
    }
    emitter.skeletalAsset = imported.modelResource.skeletalAsset;
    const SkeletalAsset& asset = *emitter.skeletalAsset;
    // 加载期预检：关节 palette、section、影响区间与骨段数必须在烘焙 GPU
    // 资源之前成立。失败直接抛出，上层沿用 mesh 的 billboard 回退与一次告警，
    // 避免运行时读空的 palette 或缺失面片。
    ParticleSkeletalPreflightInput preflightInput;
    preflightInput.jointCount = asset.mesh.joints.size();
    preflightInput.jointParents.reserve(asset.mesh.joints.size());
    for (const Joint& joint : asset.mesh.joints)
    {
        preflightInput.jointParents.push_back(joint.parent);
    }
    for (const SkinSection& section : asset.mesh.sections)
    {
        ParticleSkeletalSectionInspection inspection;
        inspection.vertexCount = section.vertices.size();
        inspection.indexCount = section.indices.size();
        inspection.influenceCount = section.influences.size();
        inspection.inverseBindCount = section.inverseBinds.size();
        inspection.influenceOffsetCount = section.influenceOffsets.size();
        for (size_t vertexIndex = 0;
             vertexIndex + 1 < section.influenceOffsets.size();
             ++vertexIndex)
        {
            const uint32_t begin = section.influenceOffsets[vertexIndex];
            const uint32_t end = section.influenceOffsets[vertexIndex + 1];
            if (begin > end || end > section.influences.size())
            {
                inspection.structuralError =
                    "influence range is out of bounds at vertex " +
                    std::to_string(vertexIndex);
                break;
            }
            inspection.maxInfluencesPerVertex = std::max(
                inspection.maxInfluencesPerVertex, end - begin);
            for (uint32_t influenceIndex = begin;
                 influenceIndex < end;
                 ++influenceIndex)
            {
                const Influence& influence =
                    section.influences[influenceIndex];
                if (influence.joint >= asset.mesh.joints.size())
                {
                    inspection.structuralError =
                        "influence references out-of-range joint " +
                        std::to_string(influence.joint);
                    break;
                }
                if (!std::isfinite(influence.weight) ||
                    influence.weight < 0.0f)
                {
                    inspection.structuralError =
                        "influence weight must be finite and non-negative";
                    break;
                }
            }
            if (!inspection.structuralError.empty())
            {
                break;
            }
        }
        preflightInput.sections.push_back(std::move(inspection));
    }
    preflightInput.wantTriangles =
        emitter.asset.renderMode == "skeletalMeshTris";
    preflightInput.wantBones =
        emitter.asset.renderMode == "skeletalMeshBones";
    const ParticleSkeletalPreflight preflight =
        ValidateParticleSkeletalPreflight(preflightInput);
    if (!preflight.ok())
    {
        throw std::runtime_error(preflight.error);
    }
    if (preflight.requiredMaxParticles > emitter.asset.maxParticles)
    {
        // 源项数量超过池容量时 shader 会按 sourceIndex < count 静默丢弃；
        // 加载期告警一次，让作者把 maxParticles 调到需要的数量。
        WarnMeshFallbackOnce(
            emitterKey + "#skeletalCapacity",
            emitter.asset.meshPath,
            "skeletal sources (" +
                std::to_string(preflight.requiredMaxParticles) +
                ") exceed maxParticles (" +
                std::to_string(emitter.asset.maxParticles) + ")");
    }
    emitter.skeletalClipIndex = 0;
    if (!emitter.asset.skeletalPose.clip.empty())
    {
        const auto clip = std::find_if(
            asset.clips.begin(),
            asset.clips.end(),
            [&](const Clip& candidate)
            {
                return candidate.id == emitter.asset.skeletalPose.clip;
            });
        if (clip == asset.clips.end())
        {
            throw std::runtime_error(
                "skeletalPose.clip is unknown: " +
                emitter.asset.skeletalPose.clip);
        }
        emitter.skeletalClipIndex = static_cast<size_t>(
            std::distance(asset.clips.begin(), clip));
    }
    emitter.skeletalPoseTime = emitter.asset.skeletalPose.time;
    emitter.skeletalPoseRate = emitter.asset.skeletalPose.rate;
    emitter.skeletalPoseLoop = emitter.asset.skeletalPose.loop;
    // 资产生成：绑定/静止姿态只用来烘焙 spawn 源与包围半径；每帧播放姿态
    // 由 RecordSimulation 写入各 image 的 palette，顶点着色器完成蒙皮。
    const std::shared_ptr<const std::vector<Eigen::Matrix4f>> restModels =
        SampleParticleSkeletalModels(
            asset, emitter.skeletalClipIndex, 0.0f, false);
    const auto createHostBuffer = [&](vk::DeviceSize bytes,
                                      vk::BufferUsageFlags usage,
                                      const std::string& name)
    {
        GpuBuffer result;
        const auto allocation = backend->CreateBuffer(
            bytes,
            usage,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            name);
        result.buffer = allocation.first;
        result.memory = allocation.second;
        return result;
    };
    const auto uploadHostBuffer = [&](GpuBuffer& buffer,
                                      const void* data,
                                      vk::DeviceSize bytes,
                                      const char* what)
    {
        void* mapped = backend->MapMemory(buffer.memory, bytes);
        if (mapped == nullptr)
        {
            throw std::runtime_error(
                std::string("skeletal ") + what + " map failed");
        }
        std::memcpy(mapped, data, static_cast<size_t>(bytes));
        backend->UnmapMemory(buffer.memory);
    };

    std::vector<ParticleSpawnSourceRecord> sourceRecords;
    std::vector<float> sourceWeights;
    std::vector<GpuBuffer> pendingBuffers;
    const auto releasePending = [&]()
    {
        for (GpuBuffer& buffer : pendingBuffers)
        {
            DestroyBuffer(*backend, buffer);
        }
        pendingBuffers.clear();
    };
    try
    {
        for (size_t sectionIndex = 0;
             sectionIndex < asset.mesh.sections.size();
             ++sectionIndex)
        {
            const SkinSection& source = asset.mesh.sections[sectionIndex];
            if (source.vertices.empty() || source.indices.empty())
            {
                continue;
            }
            EmitterResources::SkeletalSection section;
            section.sourceVertices = source.vertices;
            section.sourceIndices = source.indices;
            section.influenceOffsets = source.influenceOffsets;
            section.influences.reserve(source.influences.size());
            for (const Influence& influence : source.influences)
            {
                section.influences.push_back(
                    {influence.joint, influence.weight});
            }
            section.inverseBinds = source.inverseBinds;
            section.meshToModel = source.meshToModel;
            section.indexCount =
                static_cast<uint32_t>(section.sourceIndices.size());
            // 顶点缓冲保存绑定姿态：GPU skinning 的输入，绝不能写蒙皮后的
            // 顶点，否则每帧再蒙皮会让形变逐帧累积。
            section.vertices = createHostBuffer(
                static_cast<vk::DeviceSize>(
                    section.sourceVertices.size() * sizeof(Vertex)),
                vk::BufferUsageFlagBits::eVertexBuffer,
                "ParticleSkeletalVertices:" + emitterKey +
                    "/" + std::to_string(sectionIndex));
            pendingBuffers.push_back(section.vertices);
            uploadHostBuffer(
                section.vertices,
                section.sourceVertices.data(),
                static_cast<vk::DeviceSize>(
                    section.sourceVertices.size() * sizeof(Vertex)),
                "vertex");
            section.indices = createHostBuffer(
                static_cast<vk::DeviceSize>(
                    section.sourceIndices.size() * sizeof(uint32_t)),
                vk::BufferUsageFlagBits::eIndexBuffer,
                "ParticleSkeletalIndices:" + emitterKey +
                    "/" + std::to_string(sectionIndex));
            pendingBuffers.push_back(section.indices);
            uploadHostBuffer(
                section.indices,
                section.sourceIndices.data(),
                static_cast<vk::DeviceSize>(
                    section.sourceIndices.size() * sizeof(uint32_t)),
                "index");
            // CSR 影响流与 GPU 顶点缓冲同生命周期；权重按 uint bitcast
            // 打包，与 particleSkeletalSkin.glsl 的 uintBitsToFloat 对齐。
            std::vector<uint32_t> packedInfluences;
            packedInfluences.reserve(
                section.influenceOffsets.size() +
                section.influences.size() * 2u);
            for (uint32_t offset : section.influenceOffsets)
            {
                packedInfluences.push_back(offset * 2u);
            }
            for (const EmitterResources::SkeletalSection::Influence& influence :
                 section.influences)
            {
                uint32_t weightBits = 0;
                std::memcpy(
                    &weightBits, &influence.weight, sizeof(float));
                packedInfluences.push_back(influence.joint);
                packedInfluences.push_back(weightBits);
            }
            section.skinInfluences = createHostBuffer(
                static_cast<vk::DeviceSize>(
                    std::max<size_t>(packedInfluences.size(), 1u) *
                    sizeof(uint32_t)),
                vk::BufferUsageFlagBits::eStorageBuffer,
                "ParticleSkeletalInfluences:" + emitterKey +
                    "/" + std::to_string(sectionIndex));
            pendingBuffers.push_back(section.skinInfluences);
            if (!packedInfluences.empty())
            {
                uploadHostBuffer(
                    section.skinInfluences,
                    packedInfluences.data(),
                    static_cast<vk::DeviceSize>(
                        packedInfluences.size() * sizeof(uint32_t)),
                    "influence");
            }
            // 每个 image 一份 palette：CPU 在 RecordSimulation 里写当前姿态，
            // 不能与仍在飞的帧共用同一段 buffer。
            const vk::DeviceSize paletteBytes =
                static_cast<vk::DeviceSize>(std::max<size_t>(
                    section.inverseBinds.size(), 1u)) *
                sizeof(Eigen::Matrix4f) * 2u;
            const uint32_t imageCount =
                backend->GetSwapchainImageCount();
            section.skinPalettes.reserve(imageCount);
            for (uint32_t imageIndex = 0;
                 imageIndex < imageCount;
                 ++imageIndex)
            {
                GpuBuffer palette = createHostBuffer(
                    paletteBytes,
                    vk::BufferUsageFlagBits::eStorageBuffer,
                    "ParticleSkeletalPalette:" + emitterKey +
                        "/" + std::to_string(sectionIndex) +
                        "/" + std::to_string(imageIndex));
                pendingBuffers.push_back(palette);
                section.skinPalettes.push_back(palette);
            }

            // 静态包围球按绑定姿态估计，与播放姿态的差异由剔除的保守
            // 留白吸收；这里不追求精确。
            SkinParticleSkeletalSection(section, *restModels);
            for (const Vertex& vertex : section.skinnedVertices)
            {
                const Eigen::Vector3f scaled(
                    vertex.position.x() * emitter.asset.meshScale.x(),
                    vertex.position.y() * emitter.asset.meshScale.y(),
                    vertex.position.z() * emitter.asset.meshScale.z());
                section.localBoundsRadius = std::max(
                    section.localBoundsRadius, scaled.norm());
            }
            emitter.meshLocalBoundsRadius = std::max(
                emitter.meshLocalBoundsRadius,
                section.localBoundsRadius);

            std::string materialPath =
                sectionIndex < imported.sectionPlans.size()
                    ? imported.sectionPlans[sectionIndex].materialInstancePath
                    : std::string();
            if (materialPath.empty())
            {
                materialPath = emitter.asset.materialInstancePath;
            }
            if (!materialPath.empty())
            {
                std::string materialError;
                section.material = LoadParticleMaterialInstance(
                    materialPath, materialError);
                if (!section.material.valid && !materialError.empty())
                {
                    WarnMaterialFallbackOnce(
                        emitterKey + "#" + std::to_string(sectionIndex),
                        materialPath,
                        materialError);
                }
            }
            section.albedo = ResolveMaterialTexture(
                emitterKey + "#" + std::to_string(sectionIndex),
                section.material.albedoMap,
                HostImage::TextureSemantic::Color,
                defaultTexture);
            section.normal = ResolveMaterialTexture(
                emitterKey + "#" + std::to_string(sectionIndex),
                section.material.normalMap,
                HostImage::TextureSemantic::Normal,
                defaultNormalTexture);
            section.orm = ResolveMaterialTexture(
                emitterKey + "#" + std::to_string(sectionIndex),
                section.material.ormMap,
                HostImage::TextureSemantic::Data,
                defaultOrmTexture);
            section.emissive = ResolveMaterialTexture(
                emitterKey + "#" + std::to_string(sectionIndex),
                section.material.emissiveMap,
                HostImage::TextureSemantic::Color,
                defaultEmissiveTexture);
            section.materialUniform = createHostBuffer(
                sizeof(ParticleMaterialUniforms),
                vk::BufferUsageFlagBits::eUniformBuffer,
                "ParticleSkeletalMaterial:" + emitterKey +
                    "/" + std::to_string(sectionIndex));
            pendingBuffers.push_back(section.materialUniform);
            const ParticleMaterialParams& params = section.material;
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
            uploadHostBuffer(
                section.materialUniform,
                &uniforms,
                sizeof(ParticleMaterialUniforms),
                "material uniform");

            if (emitter.asset.renderMode == "skeletalMeshTris")
            {
                for (size_t index = 0;
                     index + 2 < section.sourceIndices.size();
                     index += 3)
                {
                    const Vertex& v0 =
                        section.skinnedVertices[section.sourceIndices[index]];
                    const Vertex& v1 =
                        section.skinnedVertices[section.sourceIndices[index + 1]];
                    const Vertex& v2 =
                        section.skinnedVertices[section.sourceIndices[index + 2]];
                    sourceRecords.push_back(
                        BuildParticleSpawnTriangleRecord(
                            v0.position, v1.position, v2.position));
                }
            }
            else if (emitter.asset.renderMode == "skeletalMeshBones")
            {
                for (size_t jointIndex = 0;
                     jointIndex < asset.mesh.joints.size();
                     ++jointIndex)
                {
                    const int parent = asset.mesh.joints[jointIndex].parent;
                    if (parent < 0 ||
                        static_cast<size_t>(parent) >= restModels->size() ||
                        jointIndex >= restModels->size())
                    {
                        continue;
                    }
                    sourceRecords.push_back(
                        BuildParticleSpawnBoneRecord(
                            restModels->at(
                                static_cast<size_t>(parent)).block<3, 1>(0, 3),
                            restModels->at(jointIndex).block<3, 1>(0, 3)));
                }
            }
            emitter.skeletalSections.push_back(std::move(section));
        }
        if (emitter.skeletalSections.empty())
        {
            throw std::runtime_error(
                "skeletal mesh has no non-empty sections");
        }
        emitter.meshIndexCount = 0;
        for (const EmitterResources::SkeletalSection& section :
             emitter.skeletalSections)
        {
            emitter.meshIndexCount += section.indexCount;
        }
        emitter.meshFallbackBillboard = false;

        // spawn 源（三角面面积加权 / 骨段长度加权）写成 host-visible SSBO，
        // 与 particleSimulate.comp 的逆 CDF 采样共享同一份前缀和布局。
        std::vector<float> packedFloats;
        const uint32_t kind =
            emitter.asset.renderMode == "skeletalMeshTris"
                ? kParticleSpawnSourceTriangle
                : kParticleSpawnSourceBone;
        const uint32_t recordCount = PackParticleSpawnSource(
            sourceRecords, kind, packedFloats);
        emitter.spawnSourceRecordCount = recordCount;
        if (recordCount > 0 && !emitter.spawnSource.buffer)
        {
            // 真实记录一旦就绪就替换 CreateBuffers 的 16 字节占位缓冲；
            // 占位缓冲此时尚未被任何 descriptor 引用，可以安全销毁。
            emitter.spawnSource = createHostBuffer(
                static_cast<vk::DeviceSize>(packedFloats.size() * sizeof(float)),
                vk::BufferUsageFlagBits::eStorageBuffer,
                "ParticleSpawnSource:" + emitterKey);
            uploadHostBuffer(
                emitter.spawnSource,
                packedFloats.data(),
                static_cast<vk::DeviceSize>(packedFloats.size() * sizeof(float)),
                "spawn source");
        }
    }
    catch (...)
    {
        releasePending();
        for (EmitterResources::SkeletalSection& section :
             emitter.skeletalSections)
        {
            section.skinPalettes.clear();
        }
        emitter.skeletalSections.clear();
        throw;
    }
}

std::shared_ptr<Texture> ParticleRenderer::ResolveMaterialTexture(
    const std::string& emitterKey,
    const std::string& assetPath,
    HostImage::TextureSemantic semantic,
    const std::shared_ptr<Texture>& fallback)
{
    if (assetPath.empty())
    {
        return fallback;
    }
    try
    {
        TextureBindingLoadDesc loadDesc =
            LoadTextureAssetDesc(assetPath);
        // 法线/ORM/emissive 的数据语义由纹理资产 colorSpace 决定；
        // 这里只覆盖 runtime 用途，供调试工具与设备层区分。
        loadDesc.semantic = semantic;
        const std::string cacheKey =
            BuildTextureCacheKey(loadDesc);
        auto cached = textureCache.find(cacheKey);
        if (cached != textureCache.end())
        {
            std::shared_ptr<Texture> texture =
                cached->second.lock();
            if (texture)
            {
                return texture;
            }
            textureCache.erase(cached);
        }
        std::shared_ptr<Texture> texture =
            std::make_shared<Texture>(
                *backend,
                loadDesc.sources.front(),
                ToTextureCreateDesc(loadDesc));
        textureCache[cacheKey] = texture;
        return texture;
    }
    catch (const std::exception& exception)
    {
        WarnTextureFallbackOnce(
            emitterKey + "|" + assetPath,
            assetPath,
            exception.what());
    }
    catch (...)
    {
        WarnTextureFallbackOnce(
            emitterKey + "|" + assetPath,
            assetPath,
            "unknown material texture load failure");
    }
    return fallback;
}

void ParticleRenderer::LoadEmitterMaterialTextures(
    EmitterResources& emitter,
    const std::string& emitterKey)
{
    // 无论 MI 是否存在都绑定完整 set 1：缺失槽位使用常量默认纹理，
    // shader 通过 textureFlags 关闭对应采样分支，视觉回到原数值材质。
    const ParticleMaterialParams& material = emitter.material;
    emitter.materialAlbedo = material.valid
        ? ResolveMaterialTexture(
            emitterKey,
            material.albedoMap,
            HostImage::TextureSemantic::Color,
            defaultTexture)
        : defaultTexture;
    emitter.materialNormal = material.valid
        ? ResolveMaterialTexture(
            emitterKey,
            material.normalMap,
            HostImage::TextureSemantic::Normal,
            defaultNormalTexture)
        : defaultNormalTexture;
    emitter.materialOrm = material.valid
        ? ResolveMaterialTexture(
            emitterKey,
            material.ormMap,
            HostImage::TextureSemantic::Data,
            defaultOrmTexture)
        : defaultOrmTexture;
    emitter.materialEmissive = material.valid
        ? ResolveMaterialTexture(
            emitterKey,
            material.emissiveMap,
            HostImage::TextureSemantic::Color,
            defaultEmissiveTexture)
        : defaultEmissiveTexture;
}

} // namespace VL
