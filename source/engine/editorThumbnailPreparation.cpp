#include "engine/editorThumbnailPreparation.h"

#include <fstream>
#include <stdexcept>

#include "commonFunction.h"
#include "material/compiler/materialShaderComposer.h"
#include "material/loader/materialInstanceResolver.h"
#include "materialInstanceValidator.h"
#include "pipeline/pipelineFactory.h"
#include "shaderCompiler.h"

namespace VL
{
namespace
{

nlohmann::json ReadMaterialSource(
    const std::string& path, EditorThumbnailPreparationResult& result)
{
    const std::filesystem::path absolute = CommonFunction::Path(path);
    std::ifstream file(absolute, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read thumbnail material: " + path);
    const std::string bytes(std::istreambuf_iterator<char>(file), {});
    result.materialDigests.emplace(absolute, ContentHasher::HashString(bytes));
    auto source = nlohmann::json::parse(bytes);
    result.materialSources.emplace(absolute, source);
    return source;
}

MaterialInstanceResolveResult ResolveMaterial(
    const std::string& path, EditorThumbnailPreparationResult& result)
{
    const auto instance = ReadMaterialSource(path, result);
    const auto definition = ReadMaterialSource(instance.at("material"), result);
    auto resolved = MaterialInstanceResolver::ResolveWithDefinition(path, instance, definition);
    if (!resolved.materialJson.value("realtimeSupported", true))
        throw std::runtime_error("Material has no realtime implementation: " + path);
    return resolved;
}

void PrepareShader(
    ShaderCompiler& compiler, EditorThumbnailPreparationRequest& input,
    EditorThumbnailPreparationResult& result, const ShaderVariantKey& variant,
    const std::optional<MaterialShaderCompileRequest>& composed)
{
    const std::string key = composed
        ? PipelineFactory::GetMaterialShaderVariantCacheKey(*composed)
        : PipelineFactory::GetGraphicsShaderVariantCacheKey(variant);
    if (!input.preparedShaderKeys.insert(key).second) return;
    auto request = composed
        ? compiler.CreateMaterialGraphicsBuildRequest(*composed, MaterialShaderComposer::Compose(*composed))
        : compiler.CreateGraphicsVariantBuildRequest(variant);
    // 整个读取/冻结/编译/反射阶段都在 CPU worker；不发布正式文件。
    compiler.FreezeSourceSnapshot(request);
    auto artifact = compiler.PrepareCandidate(request);
    auto graphics = BuildGraphicsShaderVariantArtifact(
        artifact, composed ? composed->GetDisplayName() : variant.GetDisplayName());
    result.shaders.push_back({key, std::move(request), std::move(artifact), std::move(graphics)});
}

void PrepareMaterial(
    ShaderCompiler& compiler, EditorThumbnailPreparationRequest& input,
    EditorThumbnailPreparationResult& result, const std::string& path,
    const MaterialInstanceResolveResult& resolved)
{
    const RenderMode mode = MaterialInstanceValidator::ResolveRenderMode(resolved.effectiveMaterialInstanceJson);
    const auto plan = MaterialInstanceValidator::BuildLoadPlan(
        path, input.surfaceContracts.at(mode), resolved.effectiveMaterialInstanceJson,
        resolved.materialPath, resolved.materialJson, input.supportsDualSourceBlend);
    PrepareShader(compiler, input, result, plan.shaderVariantKey, plan.baseShaderCompileRequest);

    // 与 BuildMaterialShadowPipeline 相同的路由：完整 override 优先，否则仅为有需求的非透明材质装配。
    const auto shadowBase = input.shaderRoot / "glsl" / (plan.shaderVariantKey.shaderName + ".shadow");
    const bool explicitShadow = std::filesystem::is_regular_file(shadowBase.string() + ".vert") &&
        std::filesystem::is_regular_file(shadowBase.string() + ".frag");
    if (explicitShadow)
    {
        auto variant = plan.shaderVariantKey;
        variant.shaderName += ".shadow";
        PrepareShader(compiler, input, result, variant, {});
    }
    else if (!IsTransparentRenderMode(mode) && plan.baseShaderCompileRequest &&
        (plan.materialFeatureKey.usesOpacityMask || plan.materialFeatureKey.modifiesMeshPosition ||
         plan.materialFeatureKey.twoSided))
    {
        auto shadow = *plan.baseShaderCompileRequest;
        shadow.pass = MaterialPass::ShadowDepth;
        PrepareShader(compiler, input, result, plan.shaderVariantKey, shadow);
    }

    for (const auto& pair : resolved.effectiveMaterialInstanceJson.at("textures").items())
    {
        const std::string texturePath = pair.value().get<std::string>();
        ValidateTextureAssetReference(pair.key(), texturePath, path);
        const auto descriptorPath = CommonFunction::Path(texturePath);
        const auto descriptorDigest = ContentHasher::HashFile(descriptorPath);
        auto description = LoadTextureAssetDesc(texturePath);
        result.materialDigests.emplace(descriptorPath, descriptorDigest);
        const auto key = BuildTextureCacheKey(description);
        bool prepared = false;
        for (const auto& texture : result.textures)
            if (texture.cacheKey == key) { prepared = true; break; }
        if (prepared) continue;
        const auto sourcePath = CommonFunction::Path(description.source);
        const auto sourceDigest = ContentHasher::HashFile(sourcePath);
        result.materialDigests.emplace(sourcePath, sourceDigest);
        // 与同步 Texture 入口共用解码合同，保持 sRGB/linear、RGBA 与一次 V 翻转一致。
        auto image = TextureIO::Load(CommonFunction::Path(description.source),
            Texture::MaterialLoadOptions(ToTextureCreateDesc(description)));
        if (!image) throw std::runtime_error("Failed to load texture: " + description.source);
        if (ContentHasher::HashFile(sourcePath) != sourceDigest ||
            ContentHasher::HashFile(descriptorPath) != descriptorDigest)
            throw std::runtime_error("Texture changed during CPU preparation: " + texturePath);
        result.textures.push_back({std::move(description), key, std::move(*image)});
    }
}

} // namespace

EditorThumbnailPreparationResult PrepareEditorThumbnailAsset(EditorThumbnailPreparationRequest input)
{
    EditorThumbnailPreparationResult result;
    // 先拒绝 CPU-only MI，再触发预览球导入；不支持的资产不浪费几何准备。
    std::optional<MaterialInstanceResolveResult> overrideMaterial;
    if (!input.materialOverride.empty())
        overrideMaterial = ResolveMaterial(input.materialOverride, result);
    if (input.importMesh)
    {
        const auto descriptor = CommonFunction::Path(input.meshRequest.buildPlan.meshAssetPath);
        const auto model = std::filesystem::path(input.meshRequest.importRequest.absoluteModelPath);
        result.materialDigests.emplace(descriptor, ContentHasher::HashFile(descriptor));
        result.materialDigests.emplace(model, ContentHasher::HashFile(model));
        result.mesh = std::make_shared<const MeshAssetImportResult>(MeshAssetLoader::ImportSections(input.meshRequest));
        for (const auto& source : result.materialDigests)
            if (ContentHasher::HashFile(source.first) != source.second)
                throw std::runtime_error("Mesh changed during CPU preparation: " + source.first.string());
    }

    // 独立 compiler 只读取 manifest 并准备内存候选，不访问 live PipelineFactory/编译统计。
    ShaderCompiler compiler;
    compiler.Initialize(input.shaderRoot);
    if (overrideMaterial)
        PrepareMaterial(compiler, input, result, input.materialOverride, *overrideMaterial);
    else
    {
        std::unordered_set<std::string> materials;
        for (const auto& section : result.mesh->sectionPlans)
        {
            if (materials.insert(section.materialInstancePath).second)
                PrepareMaterial(compiler, input, result, section.materialInstancePath,
                    ResolveMaterial(section.materialInstancePath, result));
        }
    }
    return result;
}

} // namespace VL
