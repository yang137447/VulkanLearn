#pragma once

#include <vector>

#include "world/worldSnapshot.h"
#include "world/sceneWindRuntime.h"

namespace VL
{

struct RenderDrawPacket
{
    std::shared_ptr<const Animation::SkinPose> skinPose;
    RuntimeId objectId = 0;
    bool selected = false;
    std::string debugName;
    std::string sceneObjectIdentity;
    float simulationDeltaSeconds = 0.0f;
    uint32_t materialSlotIndex = 0;
    std::string materialSlotName;
    ResourceHandle mesh;
    ResourceHandle material;
    ResourceHandle materialInstance;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f previousModel = Eigen::Matrix4f::Identity();
    Eigen::Matrix3f speedTreeWorldToLocalDirection = Eigen::Matrix3f::Identity();
    Eigen::Vector3f worldBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f worldBoundsMax = Eigen::Vector3f::Zero();
    std::string windResponseProfileKey;
    // 放置级网格（per-placement）：非空模板键表示本帧 resolve 之前渲染线程必须为
    // mesh.key 克隆一份独立 RenderableObject，并按 placementBinding 的 Kind 解析绑定。
    // 普通共享对象两个字段为空。
    std::string placementTemplateMeshKey;
    PlacementBinding placementBinding;
};

struct MaterialInstanceDrawGroup
{
    ResourceHandle materialInstance;
    std::vector<size_t> drawPacketIndices;
};

struct MaterialDrawGroup
{
    ResourceHandle material;
    std::vector<MaterialInstanceDrawGroup> materialInstances;
};

// Renderer-owned view of one frozen WorldSnapshot. RenderScene is still CPU
// data; Vulkan descriptors, command buffers, and pass state stay in the backend
// and frame graph layers.
struct RenderScene
{
    uint64_t worldGeneration = 0;
    uint64_t frameIndex = 0;
    float hairSimulationDeltaSeconds = 0.0f;
    CameraSnapshot camera;
    std::vector<RenderDrawPacket> drawPackets;
    std::vector<MaterialDrawGroup> materialGroups;
    std::vector<LightSnapshot> lights;
    EnvironmentSnapshot environment;
    // 本帧冻结的场景风。头发与植被都从这里取方向和强度，避免各自读取资产或
    // World 的另一个风源。windTimeSeconds 由 RenderSystem 在本帧推进风时钟后
    // 写入，供阵风相位使用；GT 构建 RenderScene 时保持 0。
    WorldWind wind;
    // 本帧解析后的场景风：已缓动并含阵风，头发/植被共用。wind 仍是作者目标，
    // 供编辑器显示和下一帧缓动使用。
    ResolvedSceneWind resolvedWind;
    double windTimeSeconds = 0.0;
    int debugViewMode = 0;
};

} // namespace VL
