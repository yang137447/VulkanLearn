#pragma once

#include <vector>

#include "world/worldSnapshot.h"

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
    std::string speedTreeWindProfileKey;
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
    int debugViewMode = 0;
};

} // namespace VL
