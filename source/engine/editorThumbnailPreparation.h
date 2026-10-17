#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "mesh/loader/common/meshAssetLoader.h"
#include "pipeline/graphicsShaderVariantArtifact.h"
#include "pipeline/passPipelineContractKey.h"
#include "shader/build/contentHash.h"
#include "shaderVariant.h"
#include "textureAssetLoader.h"

namespace VL
{

// 会话冻结图的 pass 合同与源路径；CPU worker 输出导入几何和 shader 候选，不创建 GPU 资源。
struct EditorThumbnailPreparationRequest
{
    MeshAssetLoadRequest meshRequest;
    bool importMesh = true;
    std::string materialOverride;
    std::filesystem::path shaderRoot;
    std::map<RenderMode, PassPipelineContractKey> surfaceContracts;
    bool supportsDualSourceBlend = false;
    std::unordered_set<std::string> preparedShaderKeys;
};

struct EditorThumbnailPreparedShader
{
    std::string cacheKey;
    ShaderBuildRequest request;
    ShaderBuildArtifact artifact;
    GraphicsShaderVariantArtifact graphicsArtifact;
};

struct EditorThumbnailPreparedTexture
{
    TextureBindingLoadDesc description;
    std::string cacheKey;
    HostImage image;
};

// GT 消费前复核 shader 和材质摘要；陈旧请求整个丢弃，不发布 manifest 或活动缓存。
struct EditorThumbnailPreparationResult
{
    std::shared_ptr<const MeshAssetImportResult> mesh;
    std::vector<EditorThumbnailPreparedShader> shaders;
    std::vector<EditorThumbnailPreparedTexture> textures;
    std::map<std::filesystem::path, ContentDigest> materialDigests;
    std::map<std::filesystem::path, nlohmann::json> materialSources;
};

EditorThumbnailPreparationResult PrepareEditorThumbnailAsset(
    EditorThumbnailPreparationRequest request);

} // namespace VL
