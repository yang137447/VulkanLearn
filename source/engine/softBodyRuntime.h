#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "physics/fracture/fractureBudget.h"
#include "physics/softbody/rubberShell.h"
#include "physics/runtime/fractureImpactWorld.h"
#include "world/world.h"

namespace VL
{

class World;

class DiagnosticsSubsystem;
class AudioRuntime;
class RendererBackendVulkan;

// GT 侧橡皮软体运行时。它把 World 中的 balloon 描述变成 RubberShell，
// 创建/更新 host-visible 动态 RenderableObject，并把结果作为普通
// WorldMeshObject 发布；RT 和音频线程都不直接接触粒子数组。
class SoftBodyRuntime
{
public:
    SoftBodyRuntime(
        RendererBackendVulkan& rendererBackend,
        DiagnosticsSubsystem& diagnostics,
        AudioRuntime* audioRuntime,
        Physics::FractureImpactWorld& impactWorld,
        Physics::Fracture::FractureBudget& budget,
        Physics::Fracture::FractureEvictionDispatcher& evictionDispatcher);
    ~SoftBodyRuntime();

    // 只在 World 换代时重建；material reload 等同代调用直接返回。
    void SynchronizeWorld(World& world);
    // 共享冲击世界由 EngineLoop 每帧步进一次；这里只消费与 balloon 有关的事件。
    void HandleImpacts(World& world,
        const std::vector<Physics::FractureImpact>& impacts);
    void Tick(World& world, float deltaSeconds);
    // 调试/游戏冲击入口：从 +Z 向指定气球发射一个动态球体。
    void LaunchImpact(const std::string& balloonName, float speed);
    // 全局预算的回收入口：只处理属于本运行时的候选，其余候选忽略。
    void EvictFractureBudgetShards(
        const std::vector<Physics::Fracture::FractureBudgetCandidate>& candidates);
    void Shutdown();

private:
    struct Instance
    {
        WorldSoftBodyObject descriptor;
        std::vector<Physics::Softbody::RubberShell> shells;
        std::vector<std::string> meshObjectNames;
        std::vector<std::string> meshKeys;
        // 与爆裂后的 shells 对齐；完整球阶段为空。neck shard 没有 Jolt body。
        std::vector<Eigen::Matrix4f> shardLastPose;
        std::vector<bool> shardPoseValid;
        // 预算账本中的稳定 id；碎片被回收后索引会移动，不能拿下标当 id。
        std::uint64_t nextShardBudgetId = 0;
        std::vector<std::uint64_t> shardBudgetIds;
        std::vector<float> shardSettledSeconds;
        std::vector<float> shardBoundingRadius;
        bool burst = false;
        // 低能量命中：缓慢泄压塌陷，不生成碎片。
        bool leaking = false;
        float leakAge = 0.0f;
        // 漏气反冲方向（局部空间，指向远离破口的一侧）与自旋轴。
        Eigen::Vector3f leakThrustLocal = Eigen::Vector3f::UnitY();
        Eigen::Vector3f leakSpinAxisLocal = Eigen::Vector3f::UnitX();
        // 破口方向（局部，指向命中点）；用于渲染时挖洞。
        Eigen::Vector3f leakHoleDirection = Eigen::Vector3f::UnitZ();
    };

    // 爆裂瞬间的短寿命空气扰动占位表现：只通过 debug draw 发布一个扩张线框球壳，
    // 不参与物理、音频或 WorldSnapshot；owner 用于在结束时移除同一个 overlay key。
    struct AirPuff
    {
        std::string owner;
        Eigen::Vector3f center = Eigen::Vector3f::Zero();
        float radius = 0.0f;
        float elapsed = 0.0f;
        float duration = 0.18f;
    };

    void InitializeInstance(World& world, const WorldSoftBodyObject& descriptor);
    void AddSoftMesh(
        World& world,
        Instance& instance,
        Physics::Softbody::RubberShell shell,
        const std::string& suffix);
    void UpdateSoftMesh(
        World& world,
        const std::string& meshObjectName,
        const std::string& meshKey,
        const Physics::Softbody::RubberShell& shell);
    // worldImpactPoint 为世界空间命中点；压力自爆时为空，使用确定性的默认破口方向。
    void BurstInstance(World& world, Instance& instance, float impactSpeed,
        const std::optional<Eigen::Vector3f>& worldImpactPoint = std::nullopt);
    void DeflateInstance(Instance& instance, float impactSpeed,
        const Eigen::Vector3f& worldImpactPoint);
    void RemoveMeshObject(World& world, const std::string& meshObjectName,
        const std::string& meshKey);
    void RemoveShardAt(World& world, Instance& instance, size_t shardIndex);
    float CameraDistance(const Eigen::Vector3f& worldPosition) const;
    std::string BuildMeshKey(
        const World& world,
        const std::string& softBodyName,
        const std::string& suffix) const;

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
    std::vector<AirPuff> airPuffs;
};

} // namespace VL
