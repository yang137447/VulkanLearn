#include "engine/projectileRuntime.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>

#include <Eigen/Geometry>

#include "debug/drawDebug.h"
#include "engine/diagnosticsSubsystem.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderableObject.h"
#include "sceneNode.h"

namespace VL
{
namespace
{

constexpr float ArrowTipOffset = 30.0f;
constexpr float ArrowStuckSeconds = 2.5f;
constexpr float ArrowSpawnOffset = 45.0f;
constexpr float ArrowMaxLifetimeSeconds = 6.0f;
constexpr std::size_t MaxArrows = 48;

void AppendTriangle(
    std::vector<Vertex>& vertices,
    std::vector<uint32_t>& indices,
    const Eigen::Vector3f& first,
    const Eigen::Vector3f& second,
    const Eigen::Vector3f& third)
{
    Eigen::Vector3f normal = (second - first).cross(third - first);
    if (normal.norm() > 1e-6f) normal.normalize();
    else normal = Eigen::Vector3f::UnitZ();
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    const Eigen::Vector4f color(1.0f, 1.0f, 1.0f, 1.0f);
    vertices.emplace_back(first, normal, color, Eigen::Vector2f(0.0f, 0.0f));
    vertices.emplace_back(second, normal, color, Eigen::Vector2f(1.0f, 0.0f));
    vertices.emplace_back(third, normal, color, Eigen::Vector2f(0.5f, 1.0f));
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
}

void AppendQuad(
    std::vector<Vertex>& vertices,
    std::vector<uint32_t>& indices,
    const Eigen::Vector3f& first,
    const Eigen::Vector3f& second,
    const Eigen::Vector3f& third,
    const Eigen::Vector3f& fourth)
{
    AppendTriangle(vertices, indices, first, second, third);
    AppendTriangle(vertices, indices, first, third, fourth);
}

} // namespace

ProjectileRuntime::ProjectileRuntime(
    RendererBackendVulkan& rendererBackend,
    DiagnosticsSubsystem& diagnostics,
    Physics::FractureImpactWorld& impactWorld)
    : rendererBackend(rendererBackend)
    , diagnostics(diagnostics)
    , impactWorld(impactWorld)
{
    BuildArrowTemplate();
}

ProjectileRuntime::~ProjectileRuntime()
{
    Shutdown();
}

void ProjectileRuntime::BuildArrowTemplate()
{
    // 局部空间：箭头尖在 +Z = 30，尾在 -30；原点位于箭身中点，便于直接使用
    // 投射物刚体位置。命中时把原点沿飞行方向回退 ArrowTipOffset，让箭尖落在命中点。
    arrowVertices.clear();
    arrowIndices.clear();
    const float halfThickness = 1.5f;
    const float tail = -30.0f;
    const float headBase = 22.0f;
    const float tip = 30.0f;
    const float headHalf = 3.0f;

    const Eigen::Vector3f shaftXPositive[4] = {
        {halfThickness, -halfThickness, tail},
        {halfThickness, halfThickness, tail},
        {halfThickness, halfThickness, headBase},
        {halfThickness, -halfThickness, headBase}};
    AppendQuad(arrowVertices, arrowIndices,
        shaftXPositive[0], shaftXPositive[1], shaftXPositive[2], shaftXPositive[3]);
    const Eigen::Vector3f shaftXNegative[4] = {
        {-halfThickness, -halfThickness, headBase},
        {-halfThickness, halfThickness, headBase},
        {-halfThickness, halfThickness, tail},
        {-halfThickness, -halfThickness, tail}};
    AppendQuad(arrowVertices, arrowIndices,
        shaftXNegative[0], shaftXNegative[1], shaftXNegative[2], shaftXNegative[3]);
    const Eigen::Vector3f shaftYPositive[4] = {
        {-halfThickness, halfThickness, tail},
        {-halfThickness, halfThickness, headBase},
        {halfThickness, halfThickness, headBase},
        {halfThickness, halfThickness, tail}};
    AppendQuad(arrowVertices, arrowIndices,
        shaftYPositive[0], shaftYPositive[1], shaftYPositive[2], shaftYPositive[3]);
    const Eigen::Vector3f shaftYNegative[4] = {
        {-halfThickness, -halfThickness, headBase},
        {-halfThickness, -halfThickness, tail},
        {halfThickness, -halfThickness, tail},
        {halfThickness, -halfThickness, headBase}};
    AppendQuad(arrowVertices, arrowIndices,
        shaftYNegative[0], shaftYNegative[1], shaftYNegative[2], shaftYNegative[3]);
    // 尾盖
    const Eigen::Vector3f tailCap[4] = {
        {halfThickness, -halfThickness, tail},
        {-halfThickness, -halfThickness, tail},
        {-halfThickness, halfThickness, tail},
        {halfThickness, halfThickness, tail}};
    AppendQuad(arrowVertices, arrowIndices,
        tailCap[0], tailCap[1], tailCap[2], tailCap[3]);

    // 四棱锥箭头
    const Eigen::Vector3f headFront[4] = {
        {headHalf, -headHalf, headBase},
        {headHalf, headHalf, headBase},
        {-headHalf, headHalf, headBase},
        {-headHalf, -headHalf, headBase}};
    const Eigen::Vector3f headTip(0.0f, 0.0f, tip);
    for (int index = 0; index < 4; ++index)
    {
        AppendTriangle(arrowVertices, arrowIndices,
            headFront[index], headFront[(index + 1) % 4], headTip);
    }
}

void ProjectileRuntime::SynchronizeWorld(World& world)
{
    if (generation == world.GetGeneration()) return;
    Clear(world);
    generation = world.GetGeneration();
    arrowMaterialKey.clear();
    arrowMaterialInstanceKey.clear();
    settings = world.GetProjectileSettings();
    arrowMaterialKey = settings.materialKey;
    arrowMaterialInstanceKey = settings.materialInstanceKey;
    if (settings.IsConfigured())
        diagnostics.ReportInfo(
            "Projectile ready: scene projectile recipe configured");
    else if (world.IsNativeGameplay())
        diagnostics.ReportWarning(
            "Projectile disabled: scene has no projectile recipe");
}

void ProjectileRuntime::Tick(World& world, float deltaSeconds,
    const std::vector<Physics::FractureImpact>& impacts)
{
    if (deltaSeconds <= 0.0f) return;
    if (fireCooldown > 0.0f)
        fireCooldown = std::max(0.0f, fireCooldown - deltaSeconds);
    for (Arrow& arrow : arrows) arrow.age += deltaSeconds;
    for (const Physics::FractureImpact& impact : impacts)
    {
        // 投射物与任意目标/地面接触都可能消费箭矢；没有 projectileId 的
        // 自由碎片互撞事件不属于投射物生命周期。
        if (impact.projectileId == 0) continue;
        for (std::size_t index = 0; index < arrows.size(); ++index)
        {
            Arrow& arrow = arrows[index];
            if (arrow.id != impact.projectileId || arrow.stuck) continue;
            if (impact.targetRole == Physics::FractureBodyRole::Ground)
            {
                // 射向地面/天空的箭不再永久保留；命中地面直接回收可见箭。
                RemoveArrowMesh(world, arrow);
                arrows.erase(arrows.begin() + static_cast<std::ptrdiff_t>(index));
                break;
            }
            arrow.stuck = true;
            arrow.stuckTimer = ArrowStuckSeconds;
            Eigen::Vector3f direction = arrow.lastVelocity;
            if (direction.norm() <= 1e-3f) direction = Eigen::Vector3f::UnitZ();
            direction.normalize();
            const Eigen::Vector3f origin = impact.point - direction * ArrowTipOffset;
            arrow.model = Eigen::Matrix4f::Identity();
            arrow.model.block<3, 3>(0, 0) =
                Eigen::Quaternionf::FromTwoVectors(
                    Eigen::Vector3f::UnitZ(), direction).toRotationMatrix();
            arrow.model.block<3, 1>(0, 3) = origin;
            world.SetMeshObjectModel(arrow.name, arrow.model);
            break;
        }
    }

    for (auto it = arrows.begin(); it != arrows.end();)
    {
        if (it->stuck)
        {
            it->stuckTimer -= deltaSeconds;
            if (it->stuckTimer <= 0.0f)
            {
                RemoveArrowMesh(world, *it);
                it = arrows.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        if (it->age >= ArrowMaxLifetimeSeconds)
        {
            impactWorld.CancelProjectile(it->id);
            RemoveArrowMesh(world, *it);
            it = arrows.erase(it);
            continue;
        }
        if (const auto velocity = impactWorld.GetProjectileVelocity(it->id))
            it->lastVelocity = *velocity;
        const auto pose = impactWorld.GetProjectilePose(it->id);
        if (!pose)
        {
            // 投射物已销毁但没有对应命中事件（例如打到碎片）：直接移除箭头。
            RemoveArrowMesh(world, *it);
            it = arrows.erase(it);
            continue;
        }
        Eigen::Vector3f direction = it->lastVelocity;
        if (direction.norm() <= 1e-3f) direction = Eigen::Vector3f::UnitZ();
        direction.normalize();
        it->model = Eigen::Matrix4f::Identity();
        it->model.block<3, 3>(0, 0) =
            Eigen::Quaternionf::FromTwoVectors(
                Eigen::Vector3f::UnitZ(), direction).toRotationMatrix();
        it->model.block<3, 1>(0, 3) = pose->position;
        world.SetMeshObjectModel(it->name, it->model);
        ++it;
    }

    // 准星：屏幕空间大小的点固定在世界前方，Overlay 深度保证不被目标遮挡。
    const auto& camera = world.GetRenderCamera();
    if (camera)
    {
        const Eigen::Vector3f forward = camera->GetForwardVector().normalized();
        Debug::Style style;
        style.color = {1.0f, 0.92f, 0.55f, 0.95f};
        style.duration = 0.0f;
        style.category = "Gameplay";
        style.owner = "ProjectileRuntime";
        style.publisher = "ProjectileRuntime";
        style.key = "crosshair";
        style.depth = Debug::DepthMode::Overlay;
        world.GetDebugDraw().DrawDebugPoint(
            camera->GetPosition() + forward * 450.0f, 6.0f, style);
    }
}

bool ProjectileRuntime::Fire(World& world, const Camera& camera)
{
    if (fireCooldown > 0.0f) return false;
    if (arrowMaterialKey.empty())
    {
        diagnostics.ReportWarning(
            "Projectile ignored: the scene has no fracture object material to reuse");
        return false;
    }
    Eigen::Vector3f forward = camera.GetForwardVector();
    if (forward.norm() <= 1e-4f) return false;
    forward.normalize();
    if (arrows.size() >= MaxArrows)
    {
        // 保险丝：即使输入/外部命令绕过冷却，也保持可见箭和 Jolt body 有界。
        impactWorld.CancelProjectile(arrows.front().id);
        RemoveArrowMesh(world, arrows.front());
        arrows.erase(arrows.begin());
    }
    const Eigen::Vector3f position = camera.GetPosition() + forward * ArrowSpawnOffset;
    const Eigen::Vector3f velocity =
        forward * settings.speedCmPerSecond;
    uint32_t id = 0;
    try
    {
        id = impactWorld.LaunchProjectile(
            position, velocity, settings.radiusCm, settings.massKg);
    }
    catch (const std::exception& error)
    {
        diagnostics.ReportError(
            std::string("Projectile launch failed: ") + error.what());
        return false;
    }

    Arrow arrow;
    arrow.id = id;
    arrow.name = "Arrow~" + std::to_string(nextArrowId);
    arrow.meshKey = BuildMeshKey(world, nextArrowId);
    ++nextArrowId;
    arrow.lastVelocity = velocity;
    arrow.model = Eigen::Matrix4f::Identity();
    arrow.model.block<3, 3>(0, 0) =
        Eigen::Quaternionf::FromTwoVectors(
            Eigen::Vector3f::UnitZ(), forward).toRotationMatrix();
    arrow.model.block<3, 1>(0, 3) = position;
    AddArrowMesh(world, arrow);
    arrows.push_back(std::move(arrow));
    // 防止输入状态异常时一帧多箭；正常点射不受影响。
    fireCooldown = settings.cooldownSeconds;
    diagnostics.ReportInfo("Arrow fired");
    return true;
}

void ProjectileRuntime::AddArrowMesh(World& world, Arrow& arrow)
{
    auto object = std::make_shared<RenderableObject>(
        arrowVertices, arrowIndices, rendererBackend, arrow.meshKey, true);
    RendererResourceCache::GetInstance().BindRenderableObject(
        arrow.meshKey, std::move(object));

    Eigen::Vector3f minimum = Eigen::Vector3f::Constant(
        std::numeric_limits<float>::max());
    Eigen::Vector3f maximum = Eigen::Vector3f::Constant(
        std::numeric_limits<float>::lowest());
    for (const Vertex& vertex : arrowVertices)
    {
        minimum = minimum.cwiseMin(vertex.position);
        maximum = maximum.cwiseMax(vertex.position);
    }
    if (arrowVertices.empty())
    {
        minimum.setZero();
        maximum.setZero();
    }

    WorldMeshObject mesh;
    mesh.debugName = arrow.name;
    mesh.sceneObjectIdentity = arrow.name;
    mesh.materialSlotName = "Arrow";
    mesh.model = arrow.model;
    mesh.localBoundsMin = minimum;
    mesh.localBoundsMax = maximum;
    mesh.meshKey = arrow.meshKey;
    mesh.materialKey = arrowMaterialKey;
    mesh.materialInstanceKey = arrowMaterialInstanceKey;
    world.AddMeshObject(arrow.name, std::move(mesh));
}

void ProjectileRuntime::RemoveArrowMesh(World& world, const Arrow& arrow)
{
    world.RemoveMeshObject(arrow.name);
    RendererResourceCache& cache = RendererResourceCache::GetInstance();
    std::shared_ptr<RenderableObject> retired;
    const auto* cached = cache.GetRenderableObject(arrow.meshKey);
    if (cached != nullptr && *cached) retired = *cached;
    cache.BindRenderableObject(arrow.meshKey, {});
    if (retired)
    {
        ResourceRetireQueue::GetInstance().RetireShared(
            "ProjectileMesh:" + arrow.meshKey,
            generation,
            ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch(),
            std::move(retired));
    }
}

void ProjectileRuntime::Clear(World& world)
{
    for (const Arrow& arrow : arrows)
    {
        impactWorld.CancelProjectile(arrow.id);
        RemoveArrowMesh(world, arrow);
    }
    arrows.clear();
}

void ProjectileRuntime::Shutdown()
{
    arrows.clear();
    generation = 0;
}

std::string ProjectileRuntime::BuildMeshKey(
    const World& world, uint64_t index) const
{
    return "projectile|" + world.GetScenePath() + "|" +
        std::to_string(world.GetGeneration()) + "|" + std::to_string(index);
}

} // namespace VL
