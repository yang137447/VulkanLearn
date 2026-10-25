#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "baseStructs.h"
#include "environmentType.h"
#include "render/camera/cameraExposure.h"
#include "scene/placementBinding.h"

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
    // 与本帧姿态/placement 一起冻结。普通动画使用 GT 帧时钟，蓝图组件
    // 使用脚本倍率时钟；绑定它的发丝必须继承源对象的时钟。
    float simulationDeltaSeconds = 0.0f;
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
    // 放置级网格：mesh.key 已是本实例键，模板键指向仍在缓存里的模板
    // RenderableObject；渲染线程在 resolve 前据此克隆独立对象，并按 placementBinding
    // 的 Kind 解析绑定。普通共享对象两个字段为空。
    std::string placementTemplateMeshKey;
    PlacementBinding placementBinding;
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

// Immutable render-facing copy of one World frame. The renderer may keep this
// after the game thread has already advanced to a newer mutable World state.
struct WorldSnapshot
{
    uint64_t worldGeneration = 0;
    uint64_t frameIndex = 0;
    // 没有骨骼来源的发丝使用场景时间：Editing 正常预览，Play 随倍率，
    // Pause 为零。骨骼绑定发丝改用源 draw 的 simulationDeltaSeconds。
    float hairSimulationDeltaSeconds = 0.0f;
    CameraSnapshot camera;
    std::vector<MeshDrawSnapshot> meshDraws;
    std::vector<LightSnapshot> lights;
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
