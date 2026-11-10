#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <Eigen/Geometry>

#include "physics/runtime/fractureImpactWorld.h"
#include "vertexDataStruct.h"
#include "world/world.h"

class Camera;

namespace VL
{

class DiagnosticsSubsystem;
class RendererBackendVulkan;

// 试玩关卡的可见箭矢层。它只负责发射、箭头网格和命中后的冻结姿态；
// 碰撞与破坏判定仍由共享 FractureImpactWorld / 各破碎运行时完成。
class ProjectileRuntime
{
public:
    ProjectileRuntime(
        RendererBackendVulkan& rendererBackend,
        DiagnosticsSubsystem& diagnostics,
        Physics::FractureImpactWorld& impactWorld);
    ~ProjectileRuntime();

    void SynchronizeWorld(World& world);
    void Tick(World& world, float deltaSeconds,
        const std::vector<Physics::FractureImpact>& impacts);
    // 从相机中心射出一支箭；没有可复用材质时返回 false。
    bool Fire(World& world, const Camera& camera);
    void Clear(World& world);
    void Shutdown();

private:
    struct Arrow
    {
        uint32_t id = 0;
        std::string name;
        std::string meshKey;
        bool stuck = false;
        float stuckTimer = 0.0f;
        float age = 0.0f;
        Eigen::Vector3f lastVelocity = Eigen::Vector3f::Zero();
        Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    };

    void BuildArrowTemplate();
    void AddArrowMesh(World& world, Arrow& arrow);
    void RemoveArrowMesh(World& world, const Arrow& arrow);
    std::string BuildMeshKey(const World& world, uint64_t index) const;

    RendererBackendVulkan& rendererBackend;
    DiagnosticsSubsystem& diagnostics;
    Physics::FractureImpactWorld& impactWorld;
    uint64_t generation = 0;
    uint64_t nextArrowId = 0;
    float fireCooldown = 0.0f;
    std::vector<Arrow> arrows;
    std::vector<Vertex> arrowVertices;
    std::vector<uint32_t> arrowIndices;
    std::string arrowMaterialKey;
    std::string arrowMaterialInstanceKey;
    ProjectileSettings settings;
};

} // namespace VL
