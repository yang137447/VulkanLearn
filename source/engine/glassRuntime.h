#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

#include <Eigen/Geometry>

#include "physics/fracture/fractureBudget.h"
#include "physics/fracture/fragmentMesh.h"
#include "physics/fracture/glassDamagePolicy.h"
#include "physics/fracture/planarFracture.h"
#include "physics/runtime/fractureImpactWorld.h"
#include "world/world.h"

class RenderableObject;
class World;

namespace VL
{

class DiagnosticsSubsystem;
class AudioRuntime;
class RendererBackendVulkan;

// GT 侧玻璃破碎运行时。完整板、冲击点局部 Voronoi 碎片、Jolt 凸包代理和
// 落地二次碎裂都在运行时生成；不读取任何预制断裂图案。
class GlassRuntime
{
public:
    GlassRuntime(
        RendererBackendVulkan& rendererBackend,
        DiagnosticsSubsystem& diagnostics,
        AudioRuntime* audioRuntime,
        Physics::FractureImpactWorld& impactWorld,
        Physics::Fracture::FractureBudget& budget,
        Physics::Fracture::FractureEvictionDispatcher& evictionDispatcher);
    ~GlassRuntime();

    void SynchronizeWorld(World& world);
    // 共享冲击世界由 EngineLoop 每帧步进一次；这里只消费与 glassPane 有关的事件。
    void HandleImpacts(World& world,
        const std::vector<Physics::FractureImpact>& impacts);
    void Tick(World& world, float deltaSeconds);
    // 调试/游戏冲击入口：从板面法线方向发射一个动态球体。
    void LaunchImpact(const std::string& paneName, float speed);
    // 全局预算的回收入口：只处理属于本运行时的候选，其余候选忽略。
    void EvictFractureBudgetShards(
        const std::vector<Physics::Fracture::FractureBudgetCandidate>& candidates);
    void Shutdown();

private:
    struct Shard
    {
        std::string name;
        // 碎片自身局部平面轮廓，质心在原点；二次碎裂直接用它。
        Physics::Fracture::PlanarShard outline;
        double thickness = 0.6;
        int depth = 0;
        bool hasBody = false;
        bool attached = false;
        std::string meshKey;
        // 预算账本里的稳定 id（与 frag<N> 的 N 一致）；回收候选和记账都用它。
        std::uint64_t budgetId = 0;
        // 碎片局部轮廓的最大半径 × 世界缩放，用于屏幕占比启发式。
        float boundingRadius = 0.0f;
        // 板面局部坐标（未减质心），用于邻接/边框支撑判断。
        Physics::Fracture::Polygon2 paneOutline;
        float age = 0.0f;
        float settledTime = 0.0f;
    };

    struct Instance
    {
        // 每次低能量命中留下的裂纹记录；debug 折线只是它的显示适配，
        // 未来材质裂纹 overlay 可以直接消费这些板面局部参数。
        struct CrackMark
        {
            Eigen::Vector2f localImpact = Eigen::Vector2f::Zero();
            float stopRadius = 0.0f;
            float normalizedEnergy = 0.0f;
            int radialCracks = 0;
            int ringCount = 0;
        };

        WorldGlassObject descriptor;
        Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
        float scale = 1.0f;
        bool fractured = false;
        int nextShardId = 0;
        std::string intactMeshKey;
        std::vector<Shard> shards;
        // 低能量命中只积累裂纹；破碎后该状态不再使用。
        Physics::Fracture::GlassDamagePolicy damage;
        // 每次开裂冲击的确定性编号，用于 debug key 和图案 seed。
        int nextCrackHit = 0;
        std::vector<CrackMark> cracks;
        // 已提交的裂纹折线 key；World 换代或破碎时逐条移除。
        std::vector<std::string> crackDebugKeys;
        // 真实裂纹覆盖层：板面局部 2D 线段 + 单张动态网格。
        std::vector<std::pair<Eigen::Vector2f, Eigen::Vector2f>> crackSegments;
        std::shared_ptr<RenderableObject> crackMesh;
        std::string crackMeshKey;
        bool crackMeshAdded = false;
    };

    void InitializeInstance(World& world, const WorldGlassObject& descriptor);
    // 低能量命中走开裂路径；强命中或累计能量到阈值时进入 FractureInstance。
    void HandlePaneImpact(World& world, Instance& instance,
        const Eigen::Vector3f& worldImpactPoint, float impactSpeed);
    // 用较小的 stopRadius 生成裂纹拓扑，并把每个 cell 边界投影到板面绘制。
    void DrawCrackPattern(World& world, Instance& instance,
        const Eigen::Vector3f& worldImpactPoint, float impactSpeed,
        const Physics::Fracture::GlassHitDecision& decision);
    void ClearCrackMarks(World& world, Instance& instance);
    void RebuildCrackMesh(World& world, Instance& instance);
    void FractureInstance(World& world, Instance& instance,
        const Eigen::Vector3f& worldImpactPoint, float impactSpeed);
    void SpawnShard(World& world, Instance& instance,
        const Physics::Fracture::PlanarShard& outlineLocal,
        const Eigen::Vector3f& localCenter,
        const Eigen::Matrix4f& parentModel,
        const Eigen::Quaternionf& orientation,
        const Eigen::Vector3f& velocity,
        const Eigen::Vector3f& angularVelocity,
        int depth,
        bool attached = false);
    // 把失去边框支撑的静态板片转成自由落体（不切碎）。
    void DropAttachedPlate(World& world, Instance& instance, size_t shardIndex);
    void UpdateSupportIslands(World& world, Instance& instance);
    void UpdateShardTransforms(World& world, Instance& instance, float deltaSeconds);
    std::shared_ptr<RenderableObject> AcquireShardMesh(
        size_t vertexCount, size_t indexCount);
    void CollectShardMeshes();
    void AddIntactMesh(World& world, Instance& instance);
    void RemoveMeshObject(World& world, const std::string& meshObjectName,
        const std::string& meshKey);
    std::string BuildMeshKey(
        const World& world, const std::string& paneName,
        const std::string& suffix) const;
    size_t FindShard(const Instance& instance, const std::string& shardName) const;
    // 命中和回收可能发生在同一帧；按名字重查索引，避免回收后索引失效。
    // 命中外圈静态板片：预算允许时切成动态尖片，否则整块掉落。
    void ShatterAttachedShard(
        World& world, Instance& instance, const std::string& shardName,
        const Eigen::Vector3f& worldImpactPoint);
    void SplitShard(
        World& world, Instance& instance, const std::string& shardName,
        float impactSpeed, bool projectileImpact);
    float CameraDistance(const Eigen::Vector3f& worldPosition) const;

    RendererBackendVulkan& rendererBackend;
    DiagnosticsSubsystem& diagnostics;
    AudioRuntime* audioRuntime = nullptr;
    Physics::FractureImpactWorld& impactWorld;
    Physics::Fracture::FractureBudget& budget;
    Physics::Fracture::FractureEvictionDispatcher& evictionDispatcher;
    // 回收回调没有 World 参数；每次 Tick/HandleImpacts 刷新这个指针。
    World* currentWorld = nullptr;
    uint64_t generation = 0;
    std::unordered_map<std::string, Instance> instances;
    struct PendingMesh
    {
        uint64_t epoch = 0;
        std::shared_ptr<RenderableObject> mesh;
    };
    // 碎片网格池：按容量复用 host-visible 动态网格，等 GPU epoch 完成后回收。
    std::vector<std::shared_ptr<RenderableObject>> freeShardMeshes;
    std::vector<PendingMesh> pendingShardMeshes;
};

} // namespace VL
