#pragma once

#include <string_view>
#include <vector>
#include "../../meshAssetTypes.h"

// Load request shared by scene validation and runtime mesh loading.
// It contains resolved mesh descriptor data and the import request consumed by model importers.
struct MeshAssetLoadRequest
{
    MeshAssetBuildPlan buildPlan;
    ModelImportRequest importRequest;
    // 与已解析描述同次读取的身份；导入前后复核，防止旧模型配上新数据库统计。
    std::string descriptorIdentity;
    std::optional<TressfxSimulationConfig> tressfxEffective;
    // 组索引 0 = 资产默认值 + 放置覆盖；k = 资产默认值 + 第 k 组覆盖 + 放置覆盖。
    // CPU 参考 solver、GPU group config SSBO 与行为身份都消费这一份解析结果。
    std::vector<TressfxSimulationConfig> tressfxGroupConfigs;
    std::shared_ptr<const TressfxPlacementResources> tressfxResources;
};

// Result of importing one mesh asset into CPU-side sections plus per-section material mapping.
// RendererMeshLoader consumes this output to create renderables and bind
// material instances.
struct MeshAssetImportResult
{
    MeshAssetBuildPlan buildPlan;
    ModelResource modelResource;
    std::vector<MeshSectionLoadPlan> sectionPlans;
};

// Mesh-side loader used by scene preflight and RendererMeshLoader. It
// centralizes mesh JSON resolve/validate/import operations and keeps that
// policy out of WorldBuilder.
class MeshAssetLoader
{
public:
    static MeshAssetLoadRequest Prepare(std::string_view meshAssetPath);
    static void PrepareTressfxPlacement(
        MeshAssetLoadRequest& request, const nlohmann::json& sceneObject);
    static void ValidateTressfxPlacementSources(const MeshAssetLoadRequest& request);
    static void ValidateImportRequest(const MeshAssetLoadRequest& loadRequest);
    static void ValidateSectionLoadable(const MeshAssetLoadRequest& loadRequest);
    static MeshAssetImportResult ImportSections(const MeshAssetLoadRequest& loadRequest);
};
