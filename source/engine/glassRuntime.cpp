#include "engine/glassRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>

#include <Eigen/Geometry>

#include "audio/audioRuntime.h"
#include "debug/drawDebug.h"
#include "engine/diagnosticsSubsystem.h"
#include "physics/fracture/plateFracturePolicy.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderableObject.h"
#include "sceneNode.h"
#include "vertexDataStruct.h"

namespace VL
{
namespace
{

// 裂纹占位使用独立的 category/publisher，结束时按 key 精确移除。
constexpr const char* CrackCategory = "GlassCrack";
constexpr const char* CrackPublisher = "GlassRuntime";

uint64_t MixBits(uint64_t value)
{
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

float UnitFromBits(uint64_t value)
{
    return static_cast<float>(MixBits(value) >> 40) * (1.0f / 16777216.0f);
}

Eigen::Vector3f RandomAngularVelocity(uint64_t seed, float scale)
{
    const uint64_t base = MixBits(seed);
    Eigen::Vector3f axis(
        UnitFromBits(base) * 2.0f - 1.0f,
        UnitFromBits(base ^ 0x51633E2D0F5A7C11ull) * 2.0f - 1.0f,
        UnitFromBits(base ^ 0x2F1B3C5D7E9A4B61ull) * 2.0f - 1.0f);
    if (axis.norm() <= 1e-4f) axis = Eigen::Vector3f::UnitY();
    axis.normalize();
    return axis * ((3.0f + 4.0f * UnitFromBits(base ^ 0x8BADF00DDEADBEEFull)) * scale);
}

Eigen::Quaternionf RotationFromModel(const Eigen::Matrix4f& model, float scale)
{
    const Eigen::Matrix3f rotation = model.block<3, 3>(0, 0) / scale;
    Eigen::Quaternionf quaternion(rotation);
    quaternion.normalize();
    return quaternion;
}

// 完整破碎的伤害半径：冲击越强越大，并夹在板面最大边的 4%..22%。
// 开裂路径复用同一半径再乘 0.5..0.8，保证“先裂后碎”的空间关系一致。
double ComputeDamageRadius(
    const WorldGlassObject& descriptor, float impactSpeed,
    const Physics::Fracture::DestructibleProfile& profile)
{
    return Physics::Fracture::ComputePlateDamageRadius(
        descriptor.width, descriptor.height, impactSpeed, profile);
}

void ComputeBounds(
    const std::vector<Eigen::Vector3f>& positions,
    Eigen::Vector3f& minimum,
    Eigen::Vector3f& maximum)
{
    minimum = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    maximum = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
    for (const Eigen::Vector3f& position : positions)
    {
        minimum = minimum.cwiseMin(position);
        maximum = maximum.cwiseMax(position);
    }
    if (positions.empty())
    {
        minimum.setZero();
        maximum.setZero();
    }
}

void AppendQuad(
    std::vector<Vertex>& vertices,
    std::vector<uint32_t>& indices,
    const Eigen::Vector3f& first,
    const Eigen::Vector3f& second,
    const Eigen::Vector3f& third,
    const Eigen::Vector3f& fourth,
    const Eigen::Vector3f& normal)
{
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    const Eigen::Vector4f color(1.0f, 1.0f, 1.0f, 1.0f);
    vertices.emplace_back(first, normal, color, Eigen::Vector2f(0.0f, 0.0f));
    vertices.emplace_back(second, normal, color, Eigen::Vector2f(1.0f, 0.0f));
    vertices.emplace_back(third, normal, color, Eigen::Vector2f(1.0f, 1.0f));
    vertices.emplace_back(fourth, normal, color, Eigen::Vector2f(0.0f, 1.0f));
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

// 完整玻璃板的六面盒网格；板面在局部 XY，厚度沿 Z。
void BuildPaneMesh(
    double width, double height, double thickness,
    std::vector<Vertex>& vertices,
    std::vector<uint32_t>& indices)
{
    const float hx = static_cast<float>(width * 0.5);
    const float hy = static_cast<float>(height * 0.5);
    const float hz = static_cast<float>(std::max(thickness * 0.5, 0.01));
    vertices.clear();
    indices.clear();
    vertices.reserve(24);
    indices.reserve(36);

    const Eigen::Vector3f normalPositiveX(1.0f, 0.0f, 0.0f);
    AppendQuad(vertices, indices,
        {hx, -hy, -hz}, {hx, hy, -hz}, {hx, hy, hz}, {hx, -hy, hz},
        normalPositiveX);
    AppendQuad(vertices, indices,
        {-hx, -hy, hz}, {-hx, hy, hz}, {-hx, hy, -hz}, {-hx, -hy, -hz},
        {-1.0f, 0.0f, 0.0f});
    AppendQuad(vertices, indices,
        {-hx, hy, -hz}, {-hx, hy, hz}, {hx, hy, hz}, {hx, hy, -hz},
        {0.0f, 1.0f, 0.0f});
    AppendQuad(vertices, indices,
        {-hx, -hy, hz}, {-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, -hy, hz},
        {0.0f, -1.0f, 0.0f});
    AppendQuad(vertices, indices,
        {-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz},
        {0.0f, 0.0f, 1.0f});
    AppendQuad(vertices, indices,
        {hx, -hy, -hz}, {-hx, -hy, -hz}, {-hx, hy, -hz}, {hx, hy, -hz},
        {0.0f, 0.0f, -1.0f});
}

} // namespace

GlassRuntime::GlassRuntime(
    RendererBackendVulkan& rendererBackend,
    DiagnosticsSubsystem& diagnostics,
    AudioRuntime* audioRuntime,
    Physics::FractureImpactWorld& impactWorld,
    Physics::Fracture::FractureBudget& budget,
    Physics::Fracture::FractureEvictionDispatcher& evictionDispatcher)
    : rendererBackend(rendererBackend)
    , diagnostics(diagnostics)
    , audioRuntime(audioRuntime)
    , impactWorld(impactWorld)
    , budget(budget)
    , evictionDispatcher(evictionDispatcher)
{
}

GlassRuntime::~GlassRuntime()
{
    Shutdown();
}

void GlassRuntime::SynchronizeWorld(World& world)
{
    currentWorld = &world;
    if (generation == world.GetGeneration()) return;
    // World 换代时裂纹状态、debug key 和累计损伤都属于旧世界；先按 key
    // 尝试清理（同 generation 原地替换的场景），再整体丢弃实例。
    for (auto& entry : instances) ClearCrackMarks(world, entry.second);
    instances.clear();
    generation = world.GetGeneration();

    for (const auto& [name, descriptor] : world.GetGlassObjects())
    {
        if (!descriptor.visible) continue;
        try
        {
            InitializeInstance(world, descriptor);
        }
        catch (const std::exception& error)
        {
            diagnostics.ReportError(
                "Glass pane initialize failed: " + name + " error=" + error.what());
        }
    }
}

bool GlassRuntime::ContainsTarget(
    const std::string& name,
    Physics::FractureMaterialProfile material) const
{
    return material == Physics::FractureMaterialProfile::Glass &&
        instances.find(name) != instances.end();
}

void GlassRuntime::Reset(World& world)
{
    currentWorld = &world;
    for (auto& [name, instance] : instances)
    {
        for (const Shard& shard : instance.shards)
        {
            impactWorld.RemoveShard(name, shard.name);
            RemoveMeshObject(world, shard.name, shard.meshKey, true);
        }
        ClearCrackMarks(world, instance);
        if (!instance.intactMeshKey.empty())
            RemoveMeshObject(world, name, instance.intactMeshKey);
        impactWorld.UnregisterPane(name);
    }
    instances.clear();
    generation = 0;
    // 作者场景里的 WorldGlassObject 仍在 World 中；重新走一次初始化即可
    // 恢复整板网格、静态 proxy 和 damage 状态。
    SynchronizeWorld(world);
}

void GlassRuntime::LaunchDebugImpact(
    const std::string& target, float speed)
{
    LaunchImpact(target, speed);
}

void GlassRuntime::HandleImpacts(
    World& world, const std::vector<Physics::FractureImpact>& impacts)
{
    currentWorld = &world;
    for (const Physics::FractureImpact& impact : impacts)
    {
        if (impact.materialProfile != Physics::FractureMaterialProfile::Glass)
            continue;
        const auto found = instances.find(impact.objectName);
        if (found == instances.end()) continue;
        Instance& instance = found->second;
        if (impact.targetRole == Physics::FractureBodyRole::StaticTarget &&
            impact.contact == Physics::FractureContactKind::Impact)
        {
            if (instance.fractured) continue;
            diagnostics.ReportInfo(
                "Glass pane impact: " + impact.objectName +
                " profile=" + Physics::FractureMaterialProfileName(
                    impact.materialProfile) +
                " speed=" + std::to_string(impact.relativeSpeed) +
                " normalSpeed=" + std::to_string(impact.normalSpeed));
            // 开裂/破碎阈值看法向压入分量，避免重力把平射的切向速度算进伤害。
            const float damageSpeed = impact.normalSpeed > 0.0f
                ? impact.normalSpeed : impact.relativeSpeed;
            HandlePaneImpact(
                world, instance, impact.point, damageSpeed);
            continue;
        }
        if (impact.targetRole == Physics::FractureBodyRole::FreeShard)
        {
            if (impact.projectileId != 0)
            {
                const size_t shardIndex =
                    FindShard(instance, impact.shardName);
                if (shardIndex < instance.shards.size())
                {
                    const Shard& hitShard = instance.shards[shardIndex];
                    diagnostics.ReportInfo(
                        "Glass shard hit: " + impact.shardName +
                        " speed=" + std::to_string(impact.relativeSpeed) +
                        " area=" + std::to_string(hitShard.outline.area) +
                        " depth=" + std::to_string(hitShard.depth) +
                        " attached=" + (hitShard.attached ? "true" : "false"));
                }
            }
            const bool split = SplitShard(world, instance, impact.shardName,
                impact.relativeSpeed, impact.projectileId != 0);
            if (audioRuntime)
            {
                ImpactAudioEvent event;
                event.audioProfile = instance.profile.audioProfile;
                event.kind = split ? ImpactAudioKind::Subfracture
                    : ImpactAudioKind::Landing;
                event.position = impact.point;
                event.intensity = std::clamp(
                    impact.relativeSpeed / 4.0f, 0.02f, 1.0f);
                event.sizeScale = 1.0f;
                audioRuntime->PlayImpact(event);
            }
            continue;
        }
        if (impact.targetRole == Physics::FractureBodyRole::AttachedPart &&
            impact.contact == Physics::FractureContactKind::Impact)
        {
            const size_t attachedIndex =
                FindShard(instance, impact.shardName);
            if (attachedIndex < instance.shards.size())
            {
                if (instance.descriptor.subfracture)
                    ShatterAttachedShard(
                        world, instance, impact.shardName, impact.point);
                else
                    // subfracture=false 仍允许整块脱落，但不继续产生新碎片。
                    DropAttachedPlate(world, instance, attachedIndex);
                UpdateSupportIslands(world, instance);
            }
            if (audioRuntime)
            {
                ImpactAudioEvent event;
                event.audioProfile = instance.profile.audioProfile;
                event.kind = ImpactAudioKind::Shatter;
                event.position = impact.point;
                event.intensity = 0.7f;
                audioRuntime->PlayImpact(event);
            }
        }
    }
}

void GlassRuntime::Tick(World& world, float deltaSeconds)
{
    if (deltaSeconds <= 0.0f) return;
    currentWorld = &world;
    CollectShardMeshes();
    for (auto& entry : instances)
        UpdateShardTransforms(world, entry.second, deltaSeconds);
}

void GlassRuntime::LaunchImpact(const std::string& paneName, float speed)
{
    const auto found = instances.find(paneName);
    if (found == instances.end() || found->second.fractured)
    {
        diagnostics.ReportWarning(
            "Glass impact ignored: pane is unknown or already broken: " + paneName);
        return;
    }
    const WorldGlassObject& descriptor = found->second.descriptor;
    const Eigen::Vector3f center = descriptor.model.block<3, 1>(0, 3);
    const float scale = found->second.scale;
    const Eigen::Vector3f normal =
        found->second.rotation * Eigen::Vector3f::UnitZ();
    const float radius = 0.5f * static_cast<float>(
        std::sqrt(descriptor.width * descriptor.width +
                  descriptor.height * descriptor.height)) * scale;
    const Eigen::Vector3f spawn = center + normal * (radius + 25.0f);
    try
    {
        impactWorld.LaunchProjectile(
            spawn, -normal * std::max(1.0f, speed), 3.0f, 0.3f);
        diagnostics.ReportInfo(
            "Glass impact launched: " + paneName +
            " speed=" + std::to_string(speed));
    }
    catch (const std::exception& error)
    {
        diagnostics.ReportError(
            "Glass impact launch failed: " + paneName + " error=" + error.what());
    }
}

void GlassRuntime::Shutdown()
{
    instances.clear();
    freeShardMeshes.clear();
    pendingShardMeshes.clear();
    currentWorld = nullptr;
    generation = 0;
}

void GlassRuntime::InitializeInstance(
    World& world, const WorldGlassObject& descriptor)
{
    Instance instance;
    instance.descriptor = descriptor;
    const auto* profile = descriptor.profileKey.empty()
        ? &Physics::Fracture::DefaultGlassProfile()
        : Physics::Fracture::FindDestructibleProfile(descriptor.profileKey);
    if (profile == nullptr ||
        profile->materialProfile != Physics::FractureMaterialProfile::Glass)
    {
        diagnostics.ReportWarning(
            "Glass profile unavailable or mismatched: " +
            descriptor.profileKey + " object=" + descriptor.name);
        profile = &Physics::Fracture::DefaultGlassProfile();
    }
    instance.profile = *profile;
    instance.scale = std::max(0.001f,
        descriptor.model.block<3, 3>(0, 0).col(0).norm());
    instance.rotation = RotationFromModel(descriptor.model, instance.scale);
    AddIntactMesh(world, instance);

    const Eigen::Vector3f center = descriptor.model.block<3, 1>(0, 3);
    const Eigen::Vector3f halfExtents(
        static_cast<float>(descriptor.width * 0.5) * instance.scale,
        static_cast<float>(descriptor.height * 0.5) * instance.scale,
        static_cast<float>(descriptor.thickness * 0.5) * instance.scale);
    impactWorld.RegisterPane(
        descriptor.name, center, instance.rotation, halfExtents,
        Physics::FractureMaterialProfile::Glass);
    diagnostics.ReportInfo(
        "Glass pane initialized: " + descriptor.name +
        " size=" + std::to_string(descriptor.width) + "x" +
        std::to_string(descriptor.height) +
        " thickness=" + std::to_string(descriptor.thickness));
    instances.emplace(descriptor.name, std::move(instance));
}

void GlassRuntime::AddIntactMesh(World& world, Instance& instance)
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    BuildPaneMesh(instance.descriptor.width, instance.descriptor.height,
        instance.descriptor.thickness, vertices, indices);
    const std::string meshKey =
        BuildMeshKey(world, instance.descriptor.name, "intact");
    auto object = std::make_shared<RenderableObject>(
        vertices, indices, rendererBackend, meshKey, true);
    RendererResourceCache::GetInstance().BindRenderableObject(meshKey, std::move(object));

    WorldMeshObject mesh;
    mesh.debugName = instance.descriptor.name;
    mesh.sceneObjectIdentity = instance.descriptor.name;
    mesh.materialSlotName = "Glass";
    mesh.model = instance.descriptor.model;
    mesh.scenePosition = instance.descriptor.scenePosition;
    mesh.sceneRotation = instance.descriptor.sceneRotation;
    mesh.sceneScale = instance.descriptor.sceneScale;
    mesh.localBoundsMin = Eigen::Vector3f(
        static_cast<float>(-instance.descriptor.width * 0.5),
        static_cast<float>(-instance.descriptor.height * 0.5),
        static_cast<float>(-instance.descriptor.thickness * 0.5));
    mesh.localBoundsMax = Eigen::Vector3f(
        static_cast<float>(instance.descriptor.width * 0.5),
        static_cast<float>(instance.descriptor.height * 0.5),
        static_cast<float>(instance.descriptor.thickness * 0.5));
    mesh.meshKey = meshKey;
    mesh.materialKey = instance.descriptor.materialKey;
    mesh.materialInstanceKey = instance.descriptor.materialInstanceKey;

    world.AddMeshObject(instance.descriptor.name, std::move(mesh));
    instance.intactMeshKey = meshKey;
}

void GlassRuntime::HandlePaneImpact(
    World& world, Instance& instance,
    const Eigen::Vector3f& worldImpactPoint, float impactSpeed)
{
    const Physics::Fracture::GlassHitDecision decision =
        instance.damage.RegisterImpact(impactSpeed);
    if (decision.outcome == Physics::Fracture::GlassHitOutcome::Fracture)
    {
        FractureInstance(world, instance, worldImpactPoint, impactSpeed);
        return;
    }

    DrawCrackPattern(
        world, instance, worldImpactPoint, impactSpeed, decision);
    // 专用开裂 transient：0.14 s 高频 click + 两个短模态，比落地 ping 更像裂纹起始。
    if (audioRuntime)
    {
        ImpactAudioEvent event;
        event.audioProfile = instance.profile.audioProfile;
        event.kind = ImpactAudioKind::Crack;
        event.position = worldImpactPoint;
        event.intensity = 0.22f + 0.35f * decision.normalizedEnergy;
        audioRuntime->PlayImpact(event);
    }
    diagnostics.ReportInfo(
        "Glass pane cracked: " + instance.descriptor.name +
        " speed=" + std::to_string(impactSpeed) +
        " energy=" + std::to_string(decision.normalizedEnergy) +
        " cracks=" + std::to_string(decision.radialCracks) +
        " rings=" + std::to_string(decision.ringCount));
}

void GlassRuntime::DrawCrackPattern(
    World& world, Instance& instance,
    const Eigen::Vector3f& worldImpactPoint, float impactSpeed,
    const Physics::Fracture::GlassHitDecision& decision)
{
    const WorldGlassObject& descriptor = instance.descriptor;
    const double halfWidth = descriptor.width * 0.5;
    const double halfHeight = descriptor.height * 0.5;
    const Physics::Fracture::Polygon2 domain{
        {-halfWidth, -halfHeight},
        {halfWidth, -halfHeight},
        {halfWidth, halfHeight},
        {-halfWidth, halfHeight}};
    const Eigen::Vector3f localImpact = (
        descriptor.model.inverse() * worldImpactPoint.homogeneous()).head<3>();
    const Physics::Fracture::Point2 impact(localImpact.x(), localImpact.y());

    // 开裂只覆盖完整伤害半径的 0.5..0.8；累计能量越高，
    // stopRadius、母裂纹数和环数一起增长，而不是每次都用同一个图案。
    const double fullDamageRadius = ComputeDamageRadius(
        descriptor, impactSpeed, instance.profile);
    const double stopRadius = std::max(
        std::min(descriptor.width, descriptor.height) * 0.015,
        fullDamageRadius * decision.crackRadiusScale);

    // 每次命中使用独立的确定性 seed；冲击点仍然决定裂纹中心，
    // 因此同一时刻、同一命中点重放时图案完全一致。
    const int hitIndex = instance.nextCrackHit++;
    Physics::Fracture::RadialFractureSettings settings;
    settings.seed = descriptor.seed ^
        MixBits(0x6A09E667F3BCC909ull + static_cast<uint64_t>(hitIndex));
    settings.coreRadius = std::min(descriptor.width, descriptor.height) * 0.02;
    settings.radialCracks = decision.radialCracks;
    settings.ringCount = decision.ringCount;
    settings.ringGrowth = 1.45;
    settings.stopRadius = stopRadius;
    settings.angularJitter = 0.42;
    settings.ringJitter = 0.30;
    settings.dicingProbability = 0.55 + 0.25 * decision.normalizedEnergy;

    const Physics::Fracture::PlanarPattern pattern =
        Physics::Fracture::GenerateRadialImpactPattern(
            domain, impact, settings);

    Instance::CrackMark mark;
    mark.localImpact = Eigen::Vector2f(
        static_cast<float>(impact.x()), static_cast<float>(impact.y()));
    mark.stopRadius = static_cast<float>(stopRadius);
    mark.normalizedEnergy = decision.normalizedEnergy;
    mark.radialCracks = decision.radialCracks;
    mark.ringCount = decision.ringCount;
    instance.cracks.push_back(mark);

    const bool hasOverlayMaterial = !descriptor.crackMaterialKey.empty();
    Debug::Style style;
    style.color = {0.86f, 0.95f, 1.0f, 0.75f};
    style.thickness = 1.5f;
    // 裂纹是持续损伤标记，保留到破碎或 World 换代；不使用单帧 duration。
    style.duration = -1.0f;
    style.category = CrackCategory;
    style.owner = descriptor.name;
    style.publisher = CrackPublisher;
    style.depth = Debug::DepthMode::Overlay;

    // 只画拓扑占位，不做材质覆盖层：每个 cell 的边界就是一条裂纹。
    for (size_t shardIndex = 0;
        shardIndex < pattern.shards.size(); ++shardIndex)
    {
        const Physics::Fracture::Polygon2& outline =
            pattern.shards[shardIndex].outline;
        if (outline.size() < 3) continue;
        for (size_t edge = 0; edge < outline.size(); ++edge)
        {
            const Eigen::Vector2d& start = outline[edge];
            const Eigen::Vector2d& end = outline[(edge + 1) % outline.size()];
            // 只保留 stopRadius 内的裂纹：外圈扇区仍覆盖整块板，
            // 但它们的径向边一直延伸到板边缘，不应算作本次局部开裂。
            if ((start - impact).norm() > stopRadius * 1.02 ||
                (end - impact).norm() > stopRadius * 1.02)
                continue;
            instance.crackSegments.emplace_back(
                Eigen::Vector2f(static_cast<float>(start.x()),
                    static_cast<float>(start.y())),
                Eigen::Vector2f(static_cast<float>(end.x()),
                    static_cast<float>(end.y())));
        }
        if (hasOverlayMaterial) continue;
        std::vector<Eigen::Vector3f> points;
        points.reserve(outline.size());
        for (const Eigen::Vector2d& point : outline)
        {
            const Eigen::Vector4f local(
                static_cast<float>(point.x()),
                static_cast<float>(point.y()), 0.0f, 1.0f);
            points.push_back((descriptor.model * local).head<3>());
        }
        style.key = "h" + std::to_string(hitIndex) +
            "c" + std::to_string(shardIndex);
        try
        {
            world.GetDebugDraw().DrawDebugPolyline(points, true, style);
            instance.crackDebugKeys.push_back(style.key);
        }
        catch (const std::exception& error)
        {
            // DrawDebug 有全局几何预算；超出时保留已有裂纹并停止本次提交，
            // 不让一次低能量命中打断 GT 帧。
            diagnostics.ReportWarning(
                "Glass crack overlay budget exceeded: " + descriptor.name +
                " error=" + error.what());
            break;
        }
    }
    if (hasOverlayMaterial) RebuildCrackMesh(world, instance);
}

void GlassRuntime::ClearCrackMarks(World& world, Instance& instance)
{
    for (const std::string& key : instance.crackDebugKeys)
        world.GetDebugDraw().RemoveKey(
            instance.descriptor.name, CrackPublisher, CrackCategory, key);
    instance.crackDebugKeys.clear();
    if (instance.crackMeshAdded)
    {
        RemoveMeshObject(world, instance.descriptor.name + "~cracks",
            instance.crackMeshKey);
        instance.crackMeshAdded = false;
    }
    instance.crackMesh.reset();
    instance.crackSegments.clear();
}

void GlassRuntime::RebuildCrackMesh(World& world, Instance& instance)
{
    const WorldGlassObject& descriptor = instance.descriptor;
    if (descriptor.crackMaterialKey.empty() || instance.crackSegments.empty())
        return;
    const float halfWidth = 0.35f;
    const float zOffset = static_cast<float>(descriptor.thickness * 0.5 + 0.06);
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(instance.crackSegments.size() * 8);
    indices.reserve(instance.crackSegments.size() * 12);
    const Eigen::Vector4f color(1.0f, 1.0f, 1.0f, 1.0f);
    auto addSide = [&](const Eigen::Vector2f& first, const Eigen::Vector2f& second,
        float z, float normalZ)
    {
        Eigen::Vector2f direction = second - first;
        if (direction.norm() <= 1e-5f) return;
        direction.normalize();
        const Eigen::Vector2f normal(-direction.y(), direction.x());
        const Eigen::Vector3f a(first.x() + normal.x() * halfWidth,
            first.y() + normal.y() * halfWidth, z);
        const Eigen::Vector3f b(first.x() - normal.x() * halfWidth,
            first.y() - normal.y() * halfWidth, z);
        const Eigen::Vector3f c(second.x() - normal.x() * halfWidth,
            second.y() - normal.y() * halfWidth, z);
        const Eigen::Vector3f d(second.x() + normal.x() * halfWidth,
            second.y() + normal.y() * halfWidth, z);
        const Eigen::Vector3f normal3(0.0f, 0.0f, normalZ);
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.emplace_back(a, normal3, color, Eigen::Vector2f(0.0f, 0.0f));
        vertices.emplace_back(b, normal3, color, Eigen::Vector2f(1.0f, 0.0f));
        vertices.emplace_back(c, normal3, color, Eigen::Vector2f(1.0f, 1.0f));
        vertices.emplace_back(d, normal3, color, Eigen::Vector2f(0.0f, 1.0f));
        if (normalZ > 0.0f)
        {
            // +Z 面需要从 +Z 看是 CCW；a,b,c,d 的顺序对 +Z 是背向的。
            indices.push_back(base);
            indices.push_back(base + 3);
            indices.push_back(base + 2);
            indices.push_back(base);
            indices.push_back(base + 2);
            indices.push_back(base + 1);
        }
        else
        {
            indices.push_back(base);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        }
    };
    for (const auto& segment : instance.crackSegments)
    {
        // 双面各一层，避免玻璃旋转后裂纹只在背面可见。
        addSide(segment.first, segment.second, zOffset, 1.0f);
        addSide(segment.first, segment.second, -zOffset, -1.0f);
    }
    if (indices.empty()) return;

    const size_t vertexCapacity = std::max<size_t>(256, ((vertices.size() + 63) / 64) * 64);
    const size_t indexCapacity = std::max<size_t>(384, ((indices.size() + 47) / 48) * 48);
    if (!instance.crackMesh ||
        instance.crackMesh->GetVertexCapacity() < vertices.size() ||
        instance.crackMesh->GetIndexCapacity() < indices.size())
    {
        if (instance.crackMeshAdded)
        {
            RemoveMeshObject(world, descriptor.name + "~cracks",
                instance.crackMeshKey);
            instance.crackMeshAdded = false;
        }
        const std::string meshKey =
            BuildMeshKey(world, descriptor.name,
                "cracks" + std::to_string(instance.nextCrackHit));
        instance.crackMesh = std::make_shared<RenderableObject>(
            vertexCapacity, indexCapacity, rendererBackend, "GlassCrackOverlay");
        instance.crackMeshKey = meshKey;
        RendererResourceCache::GetInstance().BindRenderableObject(
            meshKey, instance.crackMesh);
        WorldMeshObject mesh;
        mesh.debugName = descriptor.name + "~cracks";
        mesh.sceneObjectIdentity = descriptor.name;
        mesh.materialSlotName = "GlassCrack";
        mesh.model = descriptor.model;
        mesh.scenePosition = descriptor.scenePosition;
        mesh.sceneRotation = descriptor.sceneRotation;
        mesh.sceneScale = descriptor.sceneScale;
        mesh.materialKey = descriptor.crackMaterialKey;
        mesh.materialInstanceKey = descriptor.crackMaterialInstanceKey;
        mesh.meshKey = meshKey;
        world.AddMeshObject(descriptor.name + "~cracks", std::move(mesh));
        instance.crackMeshAdded = true;
    }
    instance.crackMesh->UpdateDynamicMesh(vertices, indices);
    Eigen::Vector3f minimum = vertices.front().position;
    Eigen::Vector3f maximum = vertices.front().position;
    for (const Vertex& vertex : vertices)
    {
        minimum = minimum.cwiseMin(vertex.position);
        maximum = maximum.cwiseMax(vertex.position);
    }
    world.SetMeshObjectLocalBounds(
        descriptor.name + "~cracks", minimum, maximum);
}

void GlassRuntime::FractureInstance(
    World& world, Instance& instance,
    const Eigen::Vector3f& worldImpactPoint, float impactSpeed)
{
    if (instance.fractured) return;
    // 破碎后玻璃整体消失，裂纹占位和累计损伤不再有意义。
    ClearCrackMarks(world, instance);
    instance.damage.Reset();
    instance.cracks.clear();
    const WorldGlassObject& descriptor = instance.descriptor;
    const double halfWidth = descriptor.width * 0.5;
    const double halfHeight = descriptor.height * 0.5;
    Physics::Fracture::Polygon2 domain{
        {-halfWidth, -halfHeight},
        {halfWidth, -halfHeight},
        {halfWidth, halfHeight},
        {-halfWidth, halfHeight}};
    const Eigen::Vector3f localImpact = (
        descriptor.model.inverse() * worldImpactPoint.homogeneous()).head<3>();
    const Physics::Fracture::Point2 impact(localImpact.x(), localImpact.y());

    const float speedScale = std::clamp(impactSpeed / 8.0f, 0.0f, 1.0f);
    // 玻璃通常只在冲击区碎裂：伤害半径随冲击能量增长，外圈保留为“裂而不掉”的
    // 静态板片。这样低能量命中只破一个洞，而不是整块玻璃消失。
    const double minDimension = std::min(descriptor.width, descriptor.height);
    const double damageRadius =
        ComputeDamageRadius(descriptor, impactSpeed, instance.profile);

    // 玻璃更接近 FEM 的形态：冲击点细碎区 + 放射状主裂纹 + 少量同心环，
    // 而不是各向同性 Voronoi。siteCount 作为碎片预算提示，推导径向裂纹数。
    Physics::Fracture::RadialFractureSettings settings;
    settings.seed = descriptor.seed;
    settings.coreRadius = minDimension * instance.profile.coreRadiusFraction;
    settings.ringGrowth = instance.profile.ringGrowth;
    // 伤害半径处强制收口；半径外的板片只保留裂纹边界、不参与运动。
    settings.stopRadius = damageRadius;
    settings.angularJitter = instance.profile.angularJitter;
    settings.ringJitter = instance.profile.ringJitter;
    settings.dicingProbability = instance.profile.dicingProbability;

    // 全场景预算不足时先缩小初始图案（少一环、少几条母裂纹），再退化为
    // “至少打出一个洞”。Request 是纯查询；回收只在最终图案确定后执行一次。
    int radialCracks = std::clamp(
        descriptor.siteCount / 4,
        instance.profile.radialCracksMin,
        instance.profile.radialCracksMax);
    int ringCount = instance.profile.ringCount;
    Physics::Fracture::PlanarPattern pattern;
    Physics::Fracture::FractureBudgetDecision budgetDecision;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        settings.radialCracks = radialCracks;
        settings.ringCount = ringCount;
        pattern = Physics::Fracture::GenerateRadialImpactPattern(
            domain, impact, settings);

        Physics::Fracture::FractureBudgetRequest request;
        request.owner = descriptor.name;
        request.budgetClass = instance.profile.dynamicBudgetClass;
        request.count = pattern.shards.size();
        budgetDecision = budget.Request(request);
        if (budgetDecision.grantedCount >= pattern.shards.size()) break;

        const int nextRings = std::max(1, ringCount - 1);
        const int nextCracks = std::max(6, radialCracks - 3);
        if (nextRings == ringCount && nextCracks == radialCracks) break;
        ringCount = nextRings;
        radialCracks = nextCracks;
    }
    if (!budgetDecision.evictions.empty())
        evictionDispatcher.EvictFractureBudgetShards(budgetDecision.evictions);

    // 先移除完整板；它的 GPU 资源走延迟退休，旧 command buffer 仍可能引用。
    RemoveMeshObject(world, descriptor.name, instance.intactMeshKey);
    impactWorld.UnregisterPane(descriptor.name);
    instance.fractured = true;

    int dynamicCount = 0;
    int attachedCount = 0;
    for (size_t index = 0; index < pattern.shards.size(); ++index)
    {
        const Physics::Fracture::PlanarShard& shard = pattern.shards[index];
        // 以碎片质心判断：伤害半径内是被打碎的洞，圈外的大板保留在原地，
        // 只表现为裂纹边界（“裂而不掉”）。
        // 外圈两到三环保留：内层 attached 只靠相邻板片连通到边框，
        // 打掉支撑它的外圈板片后就会掉落（支撑岛）。
        const bool attached = Physics::Fracture::IsPlateShardAttached(
            (shard.centroid - impact).norm(), damageRadius,
            instance.profile);
        if (attached) ++attachedCount;
        else ++dynamicCount;
        Physics::Fracture::Polygon2 localOutline = shard.outline;
        for (Eigen::Vector2d& point : localOutline)
            point -= shard.centroid;
        Physics::Fracture::PlanarShard localShard;
        localShard.outline = std::move(localOutline);
        localShard.area = shard.area;
        localShard.patternIndex = shard.patternIndex;

        const Eigen::Vector2d delta = shard.centroid - impact;
        Eigen::Vector3f localDirection(
            static_cast<float>(delta.x()), static_cast<float>(delta.y()), 0.0f);
        if (localDirection.norm() > 1e-4f) localDirection.normalize();
        // 玻璃碎片以“掉落 + 少量外翻”为主，不像气球那样被气压整片冲开。
        const float radialSpeed = 60.0f + 90.0f * speedScale;
        const float outOfPlane =
            (UnitFromBits(MixBits(descriptor.seed + index * 0x9E3779B9ull)) * 2.0f - 1.0f)
            * (25.0f + 35.0f * speedScale);
        const Eigen::Vector3f velocity =
            instance.rotation * (localDirection * radialSpeed +
                Eigen::Vector3f(0.0f, 0.0f, outOfPlane));
        SpawnShard(world, instance, localShard,
            Eigen::Vector3f(static_cast<float>(shard.centroid.x()),
                static_cast<float>(shard.centroid.y()), 0.0f),
            descriptor.model, instance.rotation, velocity,
            RandomAngularVelocity(descriptor.seed + index, 1.0f),
            0, attached);
    }
    if (audioRuntime)
    {
        ImpactAudioEvent event;
        event.audioProfile = instance.profile.audioProfile;
        event.kind = ImpactAudioKind::Shatter;
        event.position = worldImpactPoint;
        event.intensity = 0.9f + 0.3f * speedScale;
        audioRuntime->PlayImpact(event);
    }
    diagnostics.ReportInfo(
        "Glass pane shattered: " + descriptor.name +
        " fragments=" + std::to_string(pattern.shards.size()) +
        " dynamic=" + std::to_string(dynamicCount) +
        " attached=" + std::to_string(attachedCount) +
        " damageRadius=" + std::to_string(damageRadius) +
        " budgetGranted=" + std::to_string(budgetDecision.grantedCount) +
        " budgetEvicted=" + std::to_string(budgetDecision.evictions.size()));
    if (pattern.shards.size() > budgetDecision.grantedCount)
    {
        // 视觉下限：即使全局预算已经耗尽，初次命中也必须打出一个洞。
        diagnostics.ReportWarning(
            "Glass fracture exceeded fragment budget: " + descriptor.name +
            " needed=" + std::to_string(pattern.shards.size()) +
            " granted=" + std::to_string(budgetDecision.grantedCount));
    }
    // 初次碎裂也可能留下不接触外框、只被动态洞包围的 attached 板片；
    // 立即检查一次支撑岛，避免它们继续停在原板面高度看起来“卡在空中”。
    UpdateSupportIslands(world, instance);
}

void GlassRuntime::SpawnShard(
    World& world, Instance& instance,
    const Physics::Fracture::PlanarShard& outlineLocal,
    const Eigen::Vector3f& localCenter,
    const Eigen::Matrix4f& parentModel,
    const Eigen::Quaternionf& orientation,
    const Eigen::Vector3f& velocity,
    const Eigen::Vector3f& angularVelocity,
    int depth,
    bool attached)
{
    Physics::Fracture::ExtrusionSettings extrusion;
    extrusion.thickness = instance.descriptor.thickness;
    extrusion.centerZ = 0.0;
    const Physics::Fracture::FragmentMesh fragment =
        Physics::Fracture::ExtrudePlanarShard(outlineLocal, extrusion);

    const Eigen::Matrix4f worldModel =
        parentModel * Eigen::Affine3f(Eigen::Translation3f(localCenter)).matrix();
    const Eigen::Vector3f worldCenter = worldModel.block<3, 1>(0, 3);
    const std::uint64_t budgetId =
        static_cast<std::uint64_t>(instance.nextShardId);
    const std::string shardName =
        instance.descriptor.name + "~frag" + std::to_string(instance.nextShardId);
    const std::string meshKey =
        BuildMeshKey(world, instance.descriptor.name,
            "frag" + std::to_string(instance.nextShardId));
    ++instance.nextShardId;

    std::vector<Vertex> vertices;
    vertices.reserve(fragment.positions.size());
    Eigen::Vector3f minimum;
    Eigen::Vector3f maximum;
    ComputeBounds(fragment.positions, minimum, maximum);
    const Eigen::Vector3f size = (maximum - minimum).cwiseMax(Eigen::Vector3f(1e-4f, 1e-4f, 1e-4f));
    for (size_t index = 0; index < fragment.positions.size(); ++index)
    {
        const Eigen::Vector3f& position = fragment.positions[index];
        const Eigen::Vector2f uv(
            (position.x() - minimum.x()) / size.x(),
            (position.y() - minimum.y()) / size.y());
        vertices.emplace_back(position, fragment.normals[index],
            Eigen::Vector4f(1.0f, 1.0f, 1.0f, 1.0f), uv);
    }
    std::shared_ptr<RenderableObject> object =
        AcquireShardMesh(vertices.size(), fragment.indices.size());
    object->UpdateDynamicMesh(vertices, fragment.indices);
    RendererResourceCache::GetInstance().BindRenderableObject(meshKey, object);

    WorldMeshObject mesh;
    mesh.debugName = shardName;
    mesh.sceneObjectIdentity = instance.descriptor.name;
    mesh.materialSlotName = "Glass";
    mesh.model = worldModel;
    mesh.scenePosition = instance.descriptor.scenePosition;
    mesh.sceneRotation = instance.descriptor.sceneRotation;
    mesh.sceneScale = instance.descriptor.sceneScale;
    mesh.localBoundsMin = minimum;
    mesh.localBoundsMax = maximum;
    mesh.meshKey = meshKey;
    mesh.materialKey = instance.descriptor.materialKey;
    mesh.materialInstanceKey = instance.descriptor.materialInstanceKey;
    world.AddMeshObject(shardName, std::move(mesh));

    std::vector<Eigen::Vector3f> hullPoints;
    hullPoints.reserve(fragment.convexHullPoints.size());
    for (const Eigen::Vector3f& point : fragment.convexHullPoints)
        hullPoints.push_back(point * instance.scale);
    const double volumeCm3 =
        Physics::Fracture::SignedArea(outlineLocal.outline) *
        instance.descriptor.thickness;
    const float mass = std::max(0.01f,
        static_cast<float>(volumeCm3 * instance.profile.densityKgPerCm3 *
            instance.scale * instance.scale * instance.scale));
    bool hasBody = true;
    try
    {
        impactWorld.RegisterShard(instance.descriptor.name, shardName,
            worldCenter, orientation, velocity, mass, hullPoints,
            Physics::FractureMaterialProfile::Glass,
            static_cast<float>(instance.profile.friction),
            static_cast<float>(instance.profile.restitution),
            angularVelocity, !attached);
    }
    catch (const std::exception& error)
    {
        diagnostics.ReportWarning(
            "Glass shard proxy failed: " + shardName + " error=" + error.what());
        // body 创建失败时不要把网格留在 World 里：它会停在原地、占预算，
        // 并且永远不会进入 Transform/回收更新。直接退回 mesh 池并放弃该片。
        RemoveMeshObject(world, shardName, meshKey, true);
        return;
    }

    Shard shard;
    shard.name = shardName;
    shard.outline = outlineLocal;
    shard.worldArea =
        outlineLocal.area * instance.scale * instance.scale;
    shard.thickness = instance.descriptor.thickness;
    shard.depth = depth;
    shard.hasBody = hasBody;
    shard.attached = attached;
    shard.budgetId = budgetId;
    double localRadius = 0.0;
    for (const Eigen::Vector2d& point : outlineLocal.outline)
        localRadius = std::max(localRadius, point.norm());
    shard.boundingRadius =
        static_cast<float>(localRadius) * instance.scale;
    shard.paneOutline = outlineLocal.outline;
    for (Eigen::Vector2d& point : shard.paneOutline)
        point += Eigen::Vector2d(localCenter.x(), localCenter.y());
    shard.meshKey = meshKey;
    instance.shards.push_back(std::move(shard));

    // 记账与可回收候选：attached 板片只计数，动态碎片才会进入回收候选。
    const std::string& budgetClass = attached
        ? instance.profile.attachedBudgetClass
        : instance.profile.dynamicBudgetClass;
    budget.OnShardsCreated(instance.descriptor.name, budgetClass);
    if (!attached)
    {
        Physics::Fracture::FractureBudgetCandidate candidate;
        candidate.owner = instance.descriptor.name;
        candidate.shardId = budgetId;
        candidate.budgetClass = instance.profile.dynamicBudgetClass;
        candidate.boundingRadius =
            instance.shards.back().boundingRadius;
        candidate.cameraDistance = CameraDistance(worldCenter);
        budget.UpdateReclaimable(candidate);
    }
}

void GlassRuntime::ShatterAttachedShard(
    World& world, Instance& instance, const std::string& shardName,
    const Eigen::Vector3f& worldImpactPoint)
{
    const size_t shardIndex = FindShard(instance, shardName);
    if (shardIndex >= instance.shards.size()) return;
    const Shard& parent = instance.shards[shardIndex];
    if (!parent.attached) return;
    // 预算回收会 erase instance.shards；先把父碎片数据拷出来，避免悬空引用。
    Physics::Fracture::PlanarShard input = parent.outline;
    input.centroid.setZero();
    const std::string parentName = parent.name;
    const std::string parentMeshKey = parent.meshKey;
    const std::uint64_t parentBudgetId = parent.budgetId;
    const uint64_t seed = instance.descriptor.seed ^
        MixBits(parentBudgetId + 0x9E3779B1ull);

    // 后续命中必须沿用第一次的径向裂纹形态：以新的命中点为极点，
    // 在被打中的板片多边形内重新生成，而不是随机直线切三片。
    const auto meshFound = world.GetMeshObjects().find(parentName);
    if (meshFound == world.GetMeshObjects().end()) return;
    const Eigen::Matrix4f parentModel = meshFound->second.model;
    const Eigen::Vector3f localHit = (
        parentModel.inverse() * worldImpactPoint.homogeneous()).head<3>();
    const double extent = std::sqrt(std::max(parent.outline.area, 1e-6));
    Physics::Fracture::RadialFractureSettings crackSettings;
    crackSettings.seed = seed;
    crackSettings.radialCracks = std::clamp(
        static_cast<int>(extent / 6.0),
        instance.profile.attachedRadialCracksMin,
        instance.profile.attachedRadialCracksMax);
    crackSettings.ringCount = instance.profile.attachedRingCount;
    crackSettings.coreRadius = std::max(
        1.0, extent * instance.profile.attachedCoreRadiusFraction);
    crackSettings.ringGrowth = 1.7;
    crackSettings.angularJitter = 0.55;
    crackSettings.ringJitter = 0.30;
    crackSettings.dicingProbability = 0.45;
    const Physics::Fracture::PlanarPattern pattern =
        Physics::Fracture::GenerateRadialImpactPattern(input.outline,
            Eigen::Vector2d(localHit.x(), localHit.y()), crackSettings);
    if (pattern.shards.size() < 2) return;

    // 切碎会净增 children-1 片；预算不足时不再切碎，整块转成动态掉落，
    // 保证“只要还有 attached 板片就一定打得动”。
    Physics::Fracture::FractureBudgetRequest request;
    request.owner = instance.descriptor.name;
    request.budgetClass = instance.profile.attachedBudgetClass;
    request.count = pattern.shards.size() - 1;
    request.allowWholePlateDrop = true;
    const Physics::Fracture::FractureBudgetDecision decision =
        AcquireFractureBudget(budget, evictionDispatcher, request);
    if (decision.dropWholePlate ||
        decision.grantedCount < pattern.shards.size() - 1)
    {
        const size_t index = FindShard(instance, parentName);
        if (index < instance.shards.size())
            DropAttachedPlate(world, instance, index);
        return;
    }

    const Eigen::Quaternionf parentRotation =
        RotationFromModel(parentModel, instance.scale);

    for (size_t index = 0; index < pattern.shards.size(); ++index)
    {
        const Physics::Fracture::PlanarShard& child = pattern.shards[index];
        Physics::Fracture::Polygon2 localOutline = child.outline;
        for (Eigen::Vector2d& point : localOutline)
            point -= child.centroid;
        Physics::Fracture::PlanarShard localShard;
        localShard.outline = std::move(localOutline);
        localShard.area = child.area;
        localShard.patternIndex = child.patternIndex;

        const Eigen::Vector3f localCenter(
            static_cast<float>(child.centroid.x()),
            static_cast<float>(child.centroid.y()), 0.0f);
        Eigen::Vector3f outward = parentModel.block<3, 3>(0, 0) * localCenter;
        if (outward.norm() > 1e-4f) outward = outward.normalized() * 60.0f;
        else outward.setZero();
        SpawnShard(world, instance, localShard, localCenter, parentModel,
            parentRotation, outward,
            RandomAngularVelocity(seed + index, 1.0f), 0, false);
    }
    impactWorld.RemoveShard(instance.descriptor.name, parentName);
    RemoveMeshObject(world, parentName, parentMeshKey, true);
    for (size_t index = 0; index < instance.shards.size(); ++index)
        if (instance.shards[index].name == parentName)
        {
            instance.shards.erase(
                instance.shards.begin() + static_cast<std::ptrdiff_t>(index));
            break;
        }
    budget.OnShardsRemoved(
        instance.descriptor.name,
        instance.profile.attachedBudgetClass);
    diagnostics.ReportInfo(
        "Glass attached plate shattered: " + parentName +
        " children=" + std::to_string(pattern.shards.size()));
}

void GlassRuntime::DropAttachedPlate(
    World& world, Instance& instance, size_t shardIndex)
{
    if (shardIndex >= instance.shards.size()) return;
    Shard& shard = instance.shards[shardIndex];
    if (!shard.attached) return;
    const auto meshFound = world.GetMeshObjects().find(shard.name);
    if (meshFound == world.GetMeshObjects().end()) return;
    const Eigen::Matrix4f model = meshFound->second.model;
    const Eigen::Vector3f center = model.block<3, 1>(0, 3);
    const Eigen::Quaternionf rotation = RotationFromModel(model, instance.scale);
    Physics::Fracture::ExtrusionSettings extrusion;
    extrusion.thickness = instance.descriptor.thickness;
    const Physics::Fracture::FragmentMesh fragment =
        Physics::Fracture::ExtrudePlanarShard(shard.outline, extrusion);
    std::vector<Eigen::Vector3f> hull;
    hull.reserve(fragment.convexHullPoints.size());
    for (const Eigen::Vector3f& point : fragment.convexHullPoints)
        hull.push_back(point * instance.scale);
    const double volume = Physics::Fracture::SignedArea(shard.outline.outline) *
        instance.descriptor.thickness;
    const float mass = std::max(0.01f,
        static_cast<float>(volume * instance.profile.densityKgPerCm3 *
            instance.scale * instance.scale * instance.scale));
    impactWorld.RemoveShard(instance.descriptor.name, shard.name);
    try
    {
        impactWorld.RegisterShard(instance.descriptor.name, shard.name, center,
            rotation, Eigen::Vector3f::Zero(), mass, hull,
            Physics::FractureMaterialProfile::Glass,
            static_cast<float>(instance.profile.friction),
            static_cast<float>(instance.profile.restitution),
            RandomAngularVelocity(MixBits(shardIndex + 77), 0.8f), true);
    }
    catch (const std::exception& error)
    {
        shard.hasBody = false;
        diagnostics.ReportWarning(
            "Glass support island drop failed: " + shard.name +
            " error=" + error.what());
        return;
    }
    shard.attached = false;
    shard.hasBody = true;
    // attached → 动态碎片：总量不变，但从此进入可回收候选。
    budget.OnShardsRemoved(
        instance.descriptor.name,
        instance.profile.attachedBudgetClass);
    budget.OnShardsCreated(
        instance.descriptor.name,
        instance.profile.dynamicBudgetClass);
    Physics::Fracture::FractureBudgetCandidate candidate;
    candidate.owner = instance.descriptor.name;
    candidate.shardId = shard.budgetId;
    candidate.budgetClass = instance.profile.dynamicBudgetClass;
    candidate.settledSeconds = 0.0f;
    candidate.boundingRadius = shard.boundingRadius;
    candidate.cameraDistance = CameraDistance(center);
    budget.UpdateReclaimable(candidate);
    shard.settledTime = 0.0f;
    shard.age = 0.0f;
    diagnostics.ReportInfo("Glass support island dropped: " + shard.name);
}

void GlassRuntime::UpdateSupportIslands(World& world, Instance& instance)
{
    std::vector<size_t> plates;
    for (size_t index = 0; index < instance.shards.size(); ++index)
        if (instance.shards[index].attached) plates.push_back(index);
    if (plates.empty()) return;
    std::vector<Physics::Fracture::Polygon2> outlines;
    std::vector<double> worldAreas;
    outlines.reserve(plates.size());
    worldAreas.reserve(plates.size());
    for (size_t index : plates)
    {
        const Shard& plate = instance.shards[index];
        outlines.push_back(plate.paneOutline);
        worldAreas.push_back(plate.worldArea > 0.0
            ? plate.worldArea
            : plate.outline.area * instance.scale * instance.scale);
    }
    const std::vector<size_t> dropped =
        Physics::Fracture::FindUnsupportedPlateIndices(
            outlines, worldAreas, instance.descriptor.width,
            instance.descriptor.height, instance.scale, instance.profile);
    for (auto it = dropped.rbegin(); it != dropped.rend(); ++it)
        DropAttachedPlate(world, instance, plates[*it]);
    if (!dropped.empty())
    {
        diagnostics.ReportInfo(
            "Glass support islands dropped: " +
            std::to_string(dropped.size()) + " plates");
    }
}

bool GlassRuntime::SplitShard(
    World& world, Instance& instance, const std::string& shardName,
    float impactSpeed, bool projectileImpact)
{
    if (!instance.descriptor.subfracture) return false;
    const size_t shardIndex = FindShard(instance, shardName);
    if (shardIndex >= instance.shards.size()) return false;
    const Shard& parent = instance.shards[shardIndex];
    if (parent.attached) return false;
    const double shardArea = parent.worldArea > 0.0
        ? parent.worldArea
        : parent.outline.area * instance.scale * instance.scale;
    const Physics::Fracture::SubfractureDecision subfracture =
        Physics::Fracture::EvaluateSubfracture(
            shardArea, parent.depth, impactSpeed, projectileImpact,
            instance.profile);
    if (!subfracture.split) return false;

    // 预算回收可能 erase instance.shards；先拷贝父碎片数据，再申请预算。
    Physics::Fracture::PlanarShard input = parent.outline;
    input.centroid.setZero();
    const std::string parentName = parent.name;
    const std::string parentMeshKey = parent.meshKey;
    const std::uint64_t parentBudgetId = parent.budgetId;
    const int parentDepth = parent.depth;
    const auto meshFound = world.GetMeshObjects().find(parentName);
    if (meshFound == world.GetMeshObjects().end()) return false;
    const Eigen::Matrix4f parentModel = meshFound->second.model;
    const uint64_t splitSeed = instance.descriptor.seed ^
        MixBits(parentBudgetId + 0x51ED270Bull);
    // 二次碎裂用直线弦切成尖片，保持玻璃的锐利轮廓；小块只切一刀，
    // 避免地上碎片被切成看不清的粉尘。
    const Physics::Fracture::PlanarPattern pattern =
        Physics::Fracture::SplitPlanarShardByCracks(
            input, subfracture.crackCount, splitSeed);
    if (pattern.shards.size() < 2) return false;

    Physics::Fracture::FractureBudgetRequest request;
    request.owner = instance.descriptor.name;
    request.budgetClass = instance.profile.dynamicBudgetClass;
    request.count = pattern.shards.size() - 1;
    request.excludeShardIds = {parentBudgetId};
    // 先查询：预算不足时直接放弃切分，不执行无意义的回收。
    const Physics::Fracture::FractureBudgetDecision decision =
        AcquireFractureBudget(budget, evictionDispatcher, request);
    if (!decision.granted ||
        decision.grantedCount < pattern.shards.size() - 1)
        return false;

    const Eigen::Vector3f parentVelocity =
        impactWorld.GetShardVelocity(instance.descriptor.name, parentName)
            .value_or(Eigen::Vector3f::Zero());
    const int childDepth = parentDepth + 1;
    const Eigen::Quaternionf parentRotation =
        RotationFromModel(parentModel, instance.scale);

    for (size_t index = 0; index < pattern.shards.size(); ++index)
    {
        const Physics::Fracture::PlanarShard& child = pattern.shards[index];
        Physics::Fracture::Polygon2 localOutline = child.outline;
        for (Eigen::Vector2d& point : localOutline)
            point -= child.centroid;
        Physics::Fracture::PlanarShard localShard;
        localShard.outline = std::move(localOutline);
        localShard.area = child.area;
        localShard.patternIndex = child.patternIndex;

        const Eigen::Vector3f localCenter(
            static_cast<float>(child.centroid.x()),
            static_cast<float>(child.centroid.y()), 0.0f);
        Eigen::Vector3f outward = parentModel.block<3, 3>(0, 0) * localCenter;
        if (outward.norm() > 1e-4f) outward = outward.normalized() * 45.0f;
        else outward.setZero();
        SpawnShard(world, instance, localShard, localCenter, parentModel,
            parentRotation, parentVelocity + outward,
            RandomAngularVelocity(splitSeed + index, 1.2f),
            childDepth);
    }
    impactWorld.RemoveShard(instance.descriptor.name, parentName);
    RemoveMeshObject(world, parentName, parentMeshKey, true);
    for (size_t index = 0; index < instance.shards.size(); ++index)
        if (instance.shards[index].name == parentName)
        {
            instance.shards.erase(
                instance.shards.begin() + static_cast<std::ptrdiff_t>(index));
            break;
        }
    budget.OnShardsRemoved(
        instance.descriptor.name,
        instance.profile.dynamicBudgetClass);
    budget.UntrackReclaimable(instance.descriptor.name, parentBudgetId);
    diagnostics.ReportInfo(
        "Glass shard split: " + parentName +
        " children=" + std::to_string(pattern.shards.size()));
    return true;
}

void GlassRuntime::UpdateShardTransforms(
    World& world, Instance& instance, float deltaSeconds)
{
    for (Shard& shard : instance.shards)
    {
        if (!shard.hasBody || shard.attached) continue;
        shard.age += deltaSeconds;
        const auto velocity = impactWorld.GetShardVelocity(
            instance.descriptor.name, shard.name);
        if (velocity && velocity->norm() < 8.0f) shard.settledTime += deltaSeconds;
        else shard.settledTime = 0.0f;
        const auto pose = impactWorld.GetShardPose(instance.descriptor.name, shard.name);
        if (!pose) continue;
        // 回收优先级需要最新的静止时长、相机距离和包围半径；
        // 屏幕占比只是半径/距离的近似，不引入真正的投影计算。
        Physics::Fracture::FractureBudgetCandidate candidate;
        candidate.owner = instance.descriptor.name;
        candidate.shardId = shard.budgetId;
        candidate.budgetClass = instance.profile.dynamicBudgetClass;
        candidate.settledSeconds = shard.settledTime;
        candidate.boundingRadius = shard.boundingRadius;
        candidate.cameraDistance = CameraDistance(pose->position);
        budget.UpdateReclaimable(candidate);
        Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
        model.block<3, 3>(0, 0) =
            pose->rotation.toRotationMatrix() * instance.scale;
        model.block<3, 1>(0, 3) = pose->position;
        world.SetMeshObjectModel(shard.name, model);
    }
}

void GlassRuntime::RemoveMeshObject(
    World& world, const std::string& meshObjectName,
    const std::string& meshKey, bool returnToShardPool)
{
    world.RemoveMeshObject(meshObjectName);
    RendererResourceCache& cache = RendererResourceCache::GetInstance();
    std::shared_ptr<RenderableObject> retired;
    const auto* cached = cache.GetRenderableObject(meshKey);
    if (cached != nullptr && *cached) retired = *cached;
    cache.BindRenderableObject(meshKey, {});
    if (retired)
    {
        if (returnToShardPool)
        {
            // 只有调用方明确声明“这是碎片池 mesh”时才复用；不再用容量反推类型。
            pendingShardMeshes.push_back({
                ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch(),
                std::move(retired)});
            return;
        }
        // 旧网格可能仍被上一帧已提交的 command buffer 引用；不能在 GT 上直接析构。
        ResourceRetireQueue::GetInstance().RetireShared(
            "GlassMesh:" + meshKey,
            generation,
            ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch(),
            std::move(retired));
    }
}

std::shared_ptr<RenderableObject> GlassRuntime::AcquireShardMesh(
    size_t vertexCount, size_t indexCount)
{
    for (auto iterator = freeShardMeshes.begin();
        iterator != freeShardMeshes.end(); ++iterator)
    {
        if ((*iterator)->GetVertexCapacity() >= vertexCount &&
            (*iterator)->GetIndexCapacity() >= indexCount)
        {
            std::shared_ptr<RenderableObject> mesh = std::move(*iterator);
            freeShardMeshes.erase(iterator);
            return mesh;
        }
    }
    // 按 64 顶点 / 48 索引分档，避免每个碎片都新建一组 buffer。
    const size_t vertexCapacity = std::max<size_t>(64, ((vertexCount + 63) / 64) * 64);
    const size_t indexCapacity = std::max<size_t>(96, ((indexCount + 47) / 48) * 48);
    return std::make_shared<RenderableObject>(
        vertexCapacity, indexCapacity, rendererBackend, "GlassShardPool");
}

void GlassRuntime::CollectShardMeshes()
{
    const uint64_t completed =
        ResourceRetireQueue::GetInstance().GetLastCompletedEpoch();
    for (auto iterator = pendingShardMeshes.begin();
        iterator != pendingShardMeshes.end();)
    {
        if (iterator->epoch <= completed)
        {
            freeShardMeshes.push_back(std::move(iterator->mesh));
            iterator = pendingShardMeshes.erase(iterator);
        }
        else ++iterator;
    }
}

std::string GlassRuntime::BuildMeshKey(
    const World& world, const std::string& paneName,
    const std::string& suffix) const
{
    return "glass|" + world.GetScenePath() + "|" + paneName + "|" +
        std::to_string(world.GetGeneration()) + "|" + suffix;
}

size_t GlassRuntime::FindShard(
    const Instance& instance, const std::string& shardName) const
{
    for (size_t index = 0; index < instance.shards.size(); ++index)
        if (instance.shards[index].name == shardName) return index;
    return instance.shards.size();
}

float GlassRuntime::CameraDistance(const Eigen::Vector3f& worldPosition) const
{
    if (currentWorld == nullptr) return 0.0f;
    const std::shared_ptr<Camera>& camera = currentWorld->GetRenderCamera();
    if (!camera) return 0.0f;
    return (worldPosition - camera->GetPosition()).norm();
}

void GlassRuntime::EvictFractureBudgetShards(
    const std::vector<Physics::Fracture::FractureBudgetCandidate>& candidates)
{
    if (currentWorld == nullptr) return;
    for (const Physics::Fracture::FractureBudgetCandidate& candidate : candidates)
    {
        const auto found = instances.find(candidate.owner);
        if (found == instances.end()) continue;
        Instance& instance = found->second;

        size_t shardIndex = instance.shards.size();
        for (size_t index = 0; index < instance.shards.size(); ++index)
            if (instance.shards[index].budgetId == candidate.shardId)
            {
                shardIndex = index;
                break;
            }
        if (shardIndex >= instance.shards.size()) continue;
        const Shard& shard = instance.shards[shardIndex];
        // attached 板片永远不是候选；这里再挡一次，避免预算候选污染结构。
        if (shard.attached) continue;

        const std::string shardName = shard.name;
        const std::string shardKey = shard.meshKey;
        impactWorld.RemoveShard(instance.descriptor.name, shardName);
        RemoveMeshObject(*currentWorld, shardName, shardKey, true);
        instance.shards.erase(
            instance.shards.begin() + static_cast<std::ptrdiff_t>(shardIndex));
        budget.OnShardsRemoved(
            instance.descriptor.name, candidate.budgetClass);
        budget.UntrackReclaimable(candidate.owner, candidate.shardId);
    }
}

} // namespace VL
