#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "baseStructs.h"
#include "decalResponse.h"
#include "environmentType.h"
#include "particles/particleAsset.h"
#include "particles/particleRuntimeState.h"
#include "render/camera/cameraExposure.h"

namespace VL
{
namespace Animation { struct SkinPose; }

using RuntimeId = uint64_t;

// Stable resource reference copied into a snapshot. It is intentionally a
// logical handle, not a pointer to mutable gameplay or Vulkan objects.
struct ResourceHandle
{
    std::string key;
    uint64_t generation = 0;

    bool IsValid() const { return !key.empty(); }
};

struct CameraSnapshot
{
    CameraExposure exposure;
    Eigen::Matrix4f view = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f projection = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f viewProjection = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f previousViewProjection = Eigen::Matrix4f::Identity();
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    Eigen::Vector3f forward = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    Eigen::Vector3f right = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    Eigen::Vector3f up = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    float horizontalFovDegrees = 60.0f;
    float clipNear = 0.1f;
    float clipFar = 1000.0f;
};

struct MeshDrawSnapshot
{
    std::shared_ptr<const Animation::SkinPose> skinPose;
    RuntimeId objectId = 0;
    bool selected = false;
    std::string debugName;
    std::string sceneObjectIdentity;
    uint32_t materialSlotIndex = 0;
    std::string materialSlotName;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f previousModel = Eigen::Matrix4f::Identity();
    Eigen::Vector3f worldBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f worldBoundsMax = Eigen::Vector3f::Zero();
    ResourceHandle mesh;
    ResourceHandle material;
    ResourceHandle materialInstance;
    std::string speedTreeWindProfileKey;
};

enum class LightSnapshotType
{
    Directional,
    Point,
    Spot,
    SquareArea
};

struct LightSnapshot
{
    RuntimeId lightId = 0;
    LightSnapshotType type = LightSnapshotType::Point;
    Eigen::Vector3f color = Eigen::Vector3f::Ones();
    // 按 type 固定为 Directional: lux，Point/Spot: cd，SquareArea: cd/m^2。
    // 所有 position/size/clip 统一为 cm；颜色为线性 RGB tint。
    float intensity = 1.0f;
    Eigen::Vector3f position = Eigen::Vector3f::Zero();
    float radius = 0.0f;
    Eigen::Vector3f direction = Eigen::Vector3f::UnitZ();
    Eigen::Matrix3f worldToLight = Eigen::Matrix3f::Identity();
    float coneAngleOuter = 0.0f;
    float coneAngleInner = 0.0f;
    float squareSize = 0.0f;
    // 仅 Point 消费。period<=0 表示无脉冲；其余字段在脉冲窗口内按
    // (1 - t/duration)^2 叠加到 intensity（cd）上。
    float pulsePeriod = 0.0f;
    float pulseDuration = 0.0f;
    float pulsePhase = 0.0f;
    float pulsePeakIntensity = 0.0f;
};

struct EnvironmentSnapshot
{
    EnvironmentType type = EnvironmentType::ProceduralSky;
    ResourceHandle cube;
    ResourceHandle prefilteredCube;
    ResourceHandle brdfLut;
    float intensity = 1.0f;
    SkyParametersGPU skyParameters;
};

// 粒子运行时遥测分成两层：CPU 作者态计数由 World 维护，alive/预算执行结果
// 只有在渲染器回填后才可用。aliveAvailable/available 为 false 时调用方不得
// 把 0 当作“没有粒子”，必须显示为未知。
struct ParticleEmitterTelemetry
{
    uint32_t stableIndex = 0;
    std::string name;
    bool active = false;
    bool enabled = false;
    // 渲染器可比较该 epoch 的变化执行单 emitter 重启；World 只负责递增。
    uint32_t restartEpoch = 0;
    uint32_t maxParticles = 0;
    // 预算缩放/截断后本帧实际使用的池容量；0 且 active=true 表示不分配池。
    uint32_t budgetedMaxParticles = 0;
    uint32_t aliveParticles = 0;
    bool aliveAvailable = false;
};

struct ParticleSystemTelemetry
{
    uint32_t emitterCount = 0;
    uint32_t activeEmitterCount = 0;
    uint32_t enabledEmitterCount = 0;
    // enabled 再叠加实例可见性；隐藏实例的发射器不是 visible。
    uint32_t visibleEmitterCount = 0;
    // 作者池容量总和，以及按预算缩放/截断后的实际容量。
    uint32_t authoredCapacity = 0;
    uint32_t budgetedCapacity = 0;
    bool available = false;
    uint32_t aliveParticles = 0;
    // 渲染器上一帧实际提交的预算；0 且 available=false 表示还没有回填。
    uint32_t lastFrameBudget = 0;
    std::vector<ParticleEmitterTelemetry> emitters;
};

struct ParticleSystemBudget
{
    // 实例值为 0 表示没有实例覆盖，沿用 World 上限；World 值为 0 表示
    // World 不设总量上限。实例显式上限优先于 World 值。
    uint32_t maxTotalParticles = 0;
    float emitterBudgetScale = 1.0f;
    uint32_t effectiveCapacity = 0;
};

// 附着目标只在快照构建时解析；resolved=false 表示目标当前不在 World 中，
// 快照仍保留作者 TRS，避免目标暂时消失时粒子瞬移到原点。
struct ParticleSystemAttachment
{
    std::string targetKind; // ""|mesh|component
    std::string targetName;
    std::string componentName;
    Eigen::Vector3f localPosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f localRotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f localScale = Eigen::Vector3f::Ones();
    bool resolved = false;

    bool IsEnabled() const { return !targetKind.empty(); }
};

struct ParticleSystemSnapshot
{
    RuntimeId particleSystemId = 0;
    std::string name;
    std::string assetPath;
    ParticleSystemAsset asset;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    // 隐藏实例仍保留在快照中：渲染器靠它让发射器池按名字继续存活并冻结，
    // 不能像网格/灯光那样在快照构建处直接过滤掉。
    bool visible = true;
    // 运行时覆盖与可见性同样逐帧冻结进快照，渲染器不直接读取 World。
    ParticleSystemRuntimeOverrides runtime;
    ParticleSystemBudget budget;
    ParticleSystemTelemetry telemetry;
    std::unordered_map<std::string, float> userParameters;
    ParticleSystemAttachment attachment;
};

// BeforeLighting DBuffer decal 的冻结渲染输入。decalToWorld 已包含
// 单位 cube 的半尺寸缩放；世界位置落在该矩阵的 [-1,1] 局部立方体内时
// 才参与投影。颜色/normal/roughness/AO/metallic 通过 DBuffer A/B/C/D
// 在 deferredLighting 之前合成，不直接读写 GBuffer。
struct DecalSnapshot
{
    RuntimeId decalId = 0;
    std::string name;
    Eigen::Matrix4f decalToWorld = Eigen::Matrix4f::Identity();
    Eigen::Vector4f color =
        Eigen::Vector4f(0.02f, 0.018f, 0.015f, 1.0f);
    float opacity = 0.85f;
    float edgeSoftness = 0.35f;
    float noiseStrength = 0.55f;
    float randomSeed = 0.0f;
    float roughness = 1.0f;
    float metallic = 0.0f;
    float ambientOcclusion = 1.0f;
    // 与 WorldDecal 同名的作者数据；渲染器消费前不改变其语义。
    uint32_t responseMask = DefaultDecalResponse();
    float normalStrength = 0.35f;
    // 可选的 T_*.json 贴图资产引用；空字符串表示程序化/默认行为。
    // decal 材质由代码维护，没有也不计划引入 UE Material Graph。
    std::string albedoMap;
    std::string normalMap;
    // ORM：R=roughness，G=metallic，B=ambientOcclusion。
    std::string ormMap;
    uint32_t sortOrder = 0;
    bool visible = true;
};

// Immutable render-facing copy of one World frame. The renderer may keep this
// after the game thread has already advanced to a newer mutable World state.
struct WorldSnapshot
{
    uint64_t worldGeneration = 0;
    uint64_t frameIndex = 0;
    // 逻辑模拟时间（已乘 timeScale，Pause 时为 0）；粒子等逐帧模拟消费它，
    // 而不是渲染器的 wall-clock delta。
    float simulationDeltaSeconds = 1.0f / 60.0f;
    // 全局粒子数据通道的逐帧副本。它与 ParticleSystemSnapshot::userParameters
    // 是不同命名空间：这里没有实例归属，渲染器负责按约定合并 `$Name`。
    std::unordered_map<std::string, float> particleDataChannels;
    CameraSnapshot camera;
    std::vector<MeshDrawSnapshot> meshDraws;
    std::vector<LightSnapshot> lights;
    std::vector<DecalSnapshot> decals;
    std::vector<ParticleSystemSnapshot> particleSystems;
    EnvironmentSnapshot environment;
    int debugViewMode = 0;
};

using WorldSnapshotPtr = std::shared_ptr<const WorldSnapshot>;

// Single-latest snapshot handoff for the optional render thread. The mutex only
// protects the pointer slot; World traversal, RenderScene building, and command
// recording stay outside this mailbox. Publishing a newer snapshot replaces the
// pending one, which prevents a slow renderer from building an ever-growing
// backlog of obsolete frames.
class WorldSnapshotQueue
{
public:
    void Publish(WorldSnapshot snapshot);
    void Publish(WorldSnapshotPtr snapshot);

    std::optional<WorldSnapshotPtr> ConsumeLatest();
    std::optional<WorldSnapshotPtr> PeekLatest() const;

    void Clear() noexcept;
    bool HasPendingSnapshot() const;

private:
    mutable std::mutex mutex;
    WorldSnapshotPtr latestSnapshot;
};

} // namespace VL
