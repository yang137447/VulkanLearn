#pragma once

#include <optional>
#include <memory>
#include <vector>

#include <nlohmann/json.hpp>

#include "mesh/meshAssetTypes.h"

class PipelineFactory;
struct MeshAssetLoadRequest;
struct MeshAssetImportResult;
struct MeshObjectBuildPlan;

namespace VL
{
class RendererBackendVulkan;
struct RendererResourceLoadContext;

struct RendererMeshLoadResult
{
    std::vector<MeshObjectBuildPlan> objectPlans;
    std::optional<SpeedTreeWindProfile> speedTreeWindProfile;
    // 同一份已发布导入结果（含骨骼 section 的绑定顶点/权重）。跨对象解析
    // `collision.mesh` 权重转移时复用，不重新读 FBX。
    std::shared_ptr<const MeshAssetImportResult> importResult;
};

// Creates renderable sections and mesh draw bindings for mesh scene nodes.
// Material creation is delegated to RendererMaterialLoader, while CPU/GPU
// resources are registered in RendererResourceCache.
class RendererMeshLoader
{
public:
    RendererMeshLoader(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        RendererResourceLoadContext& loadContext);

    RendererMeshLoadResult LoadMeshObject(
        const nlohmann::basic_json<>& node,
        const MeshAssetLoadRequest& meshLoadRequest,
        std::shared_ptr<const MeshAssetImportResult> preparedImport = {}) const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
    RendererResourceLoadContext& loadContext;
};

} // namespace VL
