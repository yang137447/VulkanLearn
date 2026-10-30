#include "engine/softBodyRuntime.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>

#include <Eigen/Geometry>

#include "debug/drawDebug.h"
#include "engine/diagnosticsSubsystem.h"
#include "audio/audioRuntime.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderableObject.h"
#include "sceneNode.h"
#include "vertexDataStruct.h"

namespace VL
{
namespace
{

constexpr float DebugGroundHeight = -100.0f;

std::vector<Vertex> BuildSoftBodyVertices(const Physics::Softbody::SoftBodyRenderMesh& mesh)
{
    std::vector<Vertex> vertices;
    vertices.reserve(mesh.positions.size());
    for (size_t index = 0; index < mesh.positions.size(); ++index)
    {
        vertices.emplace_back(
            mesh.positions[index],
            mesh.normals[index],
            Eigen::Vector4f(1.0f, 1.0f, 1.0f, 1.0f),
            Eigen::Vector2f::Zero());
    }
    return vertices;
}

void ComputeBounds(
    const Physics::Softbody::SoftBodyRenderMesh& mesh,
    Eigen::Vector3f& minimum,
    Eigen::Vector3f& maximum)
{
    minimum = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    maximum = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
    for (const Eigen::Vector3f& position : mesh.positions)
    {
        minimum = minimum.cwiseMin(position);
        maximum = maximum.cwiseMax(position);
    }
    if (mesh.positions.empty())
    {
        minimum.setZero();
        maximum.setZero();
    }
}

// splitmix64：碎片初速度/角速度需要可复现，不能用时间戳或全局 RNG。
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

// 每个碎片一个确定性的翻滚轴和角速度：碎橡皮片在空中的翻滚比纯平动更可信。
Eigen::Vector3f ShardAngularVelocity(
    uint64_t seed, size_t shardIndex, float energyScale)
{
    const uint64_t base = MixBits(seed ^ (static_cast<uint64_t>(shardIndex) * 0x9E3779B97F4A7C15ull));
    const float x = UnitFromBits(base) * 2.0f - 1.0f;
    const float y = UnitFromBits(base ^ 0x51633E2D0F5A7C11ull) * 2.0f - 1.0f;
    const float z = UnitFromBits(base ^ 0x2F1B3C5D7E9A4B61ull) * 2.0f - 1.0f;
    Eigen::Vector3f axis(x, y, z);
    if (axis.norm() <= 1e-4f) axis = Eigen::Vector3f::UnitY();
    axis.normalize();
    // 2.5–8.0 rad/s 量级：一秒钟大约翻滚半圈到一圈多。
    const float magnitude =
        (2.5f + 5.5f * UnitFromBits(base ^ 0x8BADF00DDEADBEEFull)) * energyScale;
    return axis * magnitude;
}

// 模型矩阵可能带均匀缩放；Jolt 姿态只接受纯旋转。
Eigen::Quaternionf RotationFromModel(const Eigen::Matrix4f& model, float scale)
{
    const Eigen::Matrix3f rotation = model.block<3, 3>(0, 0) / scale;
    Eigen::Quaternionf quaternion(rotation);
    quaternion.normalize();
    return quaternion;
}

} // namespace

SoftBodyRuntime::SoftBodyRuntime(
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

SoftBodyRuntime::~SoftBodyRuntime()
{
    Shutdown();
}

void SoftBodyRuntime::SynchronizeWorld(World& world)
{
    currentWorld = &world;
    if (generation == world.GetGeneration()) return;
    instances.clear();
    airPuffs.clear();
    generation = world.GetGeneration();

    for (const auto& [name, descriptor] : world.GetSoftBodyObjects())
    {
        if (!descriptor.visible) continue;
        try
        {
            InitializeInstance(world, descriptor);
        }
        catch (const std::exception& error)
        {
            diagnostics.ReportError(
                "SoftBody initialize failed: " + name + " error=" + error.what());
        }
    }
}

void SoftBodyRuntime::HandleImpacts(
    World& world, const std::vector<Physics::FractureImpact>& impacts)
{
    currentWorld = &world;
    for (const Physics::FractureImpact& impact : impacts)
    {
        if (impact.materialProfile !=
            Physics::FractureMaterialProfile::Balloon)
        {
            continue;
        }
        const auto found = instances.find(impact.objectName);
        if (found == instances.end()) continue;
        if (impact.type == Physics::FractureImpactType::PaneHit) continue;
        if (impact.type == Physics::FractureImpactType::ShardLanding ||
            impact.type == Physics::FractureImpactType::ShardHit)
        {
            if (audioRuntime)
                audioRuntime->PlayShardLanding(
                    // Jolt 速度单位是 m/s；这里把常见落地速度映射到 0..1 强度。
                    impact.relativeSpeed / 5.0f, impact.point);
            continue;
        }
        if (found->second.burst) continue;
        diagnostics.ReportInfo(
            "SoftBody Jolt impact: " + impact.objectName +
            " profile=" + Physics::FractureMaterialProfileName(
                impact.materialProfile) +
            " speed=" + std::to_string(impact.relativeSpeed));
        // 低于爆裂阈值的命中只扎破一个洞：泄压塌陷，不产生碎片。
        constexpr float BurstSpeedMetersPerSecond = 4.0f;
        if (impact.relativeSpeed < BurstSpeedMetersPerSecond)
        {
            if (!found->second.leaking)
                DeflateInstance(found->second, impact.relativeSpeed, impact.point);
            continue;
        }
        BurstInstance(world, found->second, impact.relativeSpeed, impact.point);
    }
}

void SoftBodyRuntime::Tick(World& world, float deltaSeconds)
{
    if (deltaSeconds <= 0.0f) return;
    currentWorld = &world;
    for (auto& [name, instance] : instances)
    {
        if (instance.shells.empty()) continue;
        if (!instance.burst)
        {
            if (instance.leaking)
            {
                instance.leakAge += deltaSeconds;
                // 破口喷气反冲：随压力下降迅速减弱，同时给一点刚性自旋。
                const float thrustRemaining = std::clamp(
                    1.0f - instance.leakAge / 1.4f, 0.0f, 1.0f);
                if (thrustRemaining > 0.0f)
                    instance.shells[0].ApplyAerodynamicThrust(
                        instance.leakThrustLocal.cast<double>(),
                        950.0 * thrustRemaining * thrustRemaining,
                        instance.leakSpinAxisLocal.cast<double>(),
                        7.0 * thrustRemaining, deltaSeconds);
                // 可见破口随泄压缓慢扩大（0.12 → 0.22 rad）。
                const double holeAngle = 0.12 + 0.10 *
                    std::min(static_cast<double>(instance.leakAge) / 1.2, 1.0);
                instance.shells[0].SetRenderHole(
                    instance.leakHoleDirection.cast<double>(), holeAngle);
                // 前 1.2 秒持续抽气；dt 缩放让结果与帧率无关。
                if (instance.leakAge < 1.2f)
                    instance.shells[0].ApplyDeflateImpulse(
                        Eigen::Vector3d::Zero(),
                        30.0 * deltaSeconds, 8.0 * deltaSeconds,
                        instance.descriptor.seed + 991);
                const float scale = std::max(0.001f,
                    instance.descriptor.model.block<3, 3>(0, 0).col(0).norm());
                const float localGroundY =
                    (DebugGroundHeight - instance.descriptor.model(1, 3)) / scale;
                instance.shells[0].ApplyGroundPlane(localGroundY, 0.05f);
            }
            instance.shells[0].Step(deltaSeconds,
                instance.leaking ? 0.0 : instance.descriptor.pressure);
            UpdateSoftMesh(world, instance.meshObjectNames[0],
                instance.meshKeys[0], instance.shells[0]);
            if (!instance.leaking && instance.shells[0].ShouldBurst())
                BurstInstance(world, instance, 0.0f);
            continue;
        }
        for (size_t index = 0; index < instance.shells.size(); ++index)
        {
            if (index < instance.shardPoseValid.size() &&
                instance.shardPoseValid[index])
            {
                const auto pose = impactWorld.GetShardPose(
                    instance.descriptor.name, instance.meshObjectNames[index]);
                if (pose)
                {
                    Eigen::Matrix4f current = Eigen::Matrix4f::Identity();
                    current.block<3, 3>(0, 0) =
                        pose->rotation.toRotationMatrix();
                    current.block<3, 1>(0, 3) = pose->position;
                    const Eigen::Matrix4f& previous =
                        instance.shardLastPose[index];
                    const Eigen::Matrix4f worldDelta =
                        current * previous.inverse();
                    const Eigen::Matrix4f localDelta =
                        instance.descriptor.model.inverse() * worldDelta *
                        instance.descriptor.model;
                    instance.shells[index].ApplyRigidTransform(localDelta);
                    instance.shardLastPose[index] = current;

                    // 预算回收候选：只有静止够久的软片才会被另一块玻璃/气球挤掉。
                    if (index < instance.shardSettledSeconds.size() &&
                        index < instance.shardBoundingRadius.size() &&
                        index < instance.shardBudgetIds.size())
                    {
                        const auto velocity = impactWorld.GetShardVelocity(
                            instance.descriptor.name,
                            instance.meshObjectNames[index]);
                        if (velocity && velocity->norm() < 8.0f)
                            instance.shardSettledSeconds[index] += deltaSeconds;
                        else
                            instance.shardSettledSeconds[index] = 0.0f;

                        Physics::Fracture::FractureBudgetCandidate candidate;
                        candidate.owner = instance.descriptor.name;
                        candidate.shardId = instance.shardBudgetIds[index];
                        candidate.kind =
                            Physics::Fracture::FractureBudgetKind::BalloonShard;
                        candidate.settledSeconds =
                            instance.shardSettledSeconds[index];
                        candidate.boundingRadius =
                            instance.shardBoundingRadius[index];
                        candidate.cameraDistance = CameraDistance(pose->position);
                        budget.UpdateReclaimable(candidate);
                    }
                }
            }
            instance.shells[index].Step(deltaSeconds, 0.0);
            const float scale = std::max(0.001f,
                instance.descriptor.model.block<3, 3>(0, 0).col(0).norm());
            const float localGroundY =
                (DebugGroundHeight - instance.descriptor.model(1, 3)) / scale;
            instance.shells[index].ApplyGroundPlane(localGroundY, 0.10f);
            UpdateSoftMesh(world, instance.meshObjectNames[index],
                instance.meshKeys[index], instance.shells[index]);
        }
    }
    for (auto it = airPuffs.begin(); it != airPuffs.end();)
    {
        it->elapsed += deltaSeconds;
        if (it->elapsed >= it->duration)
        {
            world.GetDebugDraw().RemoveKey(
                it->owner, "SoftBodyRuntime", "SoftBody", "air");
            it = airPuffs.erase(it);
            continue;
        }
        // 空气扰动只是视觉占位：用扩张的线框球壳表示泄压气团，0.18s 内淡出。
        // 它不进入物理、音频或快照，owner 与气球身份一致以便结束时精确移除。
        const float t = it->elapsed / it->duration;
        Debug::Style style;
        style.color = {0.62f, 0.86f, 1.0f, (1.0f - t) * 0.45f};
        style.thickness = 2.0f;
        style.duration = 0.0f;
        style.category = "SoftBody";
        style.owner = it->owner;
        style.publisher = "SoftBodyRuntime";
        style.key = "air";
        style.depth = Debug::DepthMode::Overlay;
        world.GetDebugDraw().DrawDebugSphere(
            it->center, it->radius * (0.85f + 0.55f * t), style, 20);
        ++it;
    }
}

void SoftBodyRuntime::LaunchImpact(const std::string& balloonName, float speed)
{
    const auto found = instances.find(balloonName);
    if (found == instances.end() || found->second.burst)
    {
        diagnostics.ReportWarning(
            "SoftBody impact ignored: balloon is unknown or already burst: " + balloonName);
        return;
    }
    const WorldSoftBodyObject& descriptor = found->second.descriptor;
    const Eigen::Vector3f center = descriptor.model.block<3, 1>(0, 3);
    const float scale = std::max(0.001f, descriptor.model.block<3, 3>(0, 0).col(0).norm());
    const float radius = static_cast<float>(descriptor.radius) * scale;
    const Eigen::Vector3f direction(0.0f, 0.0f, -1.0f);
    const Eigen::Vector3f spawn = center - direction * (radius + 5.0f);
    try
    {
        impactWorld.LaunchProjectile(
            spawn, direction * std::max(1.0f, speed), radius * 0.2f, 1.0f);
        diagnostics.ReportInfo(
            "SoftBody impact launched: " + balloonName +
            " speed=" + std::to_string(speed));
    }
    catch (const std::exception& error)
    {
        diagnostics.ReportError(
            "SoftBody impact launch failed: " + balloonName + " error=" + error.what());
    }
}

void SoftBodyRuntime::Shutdown()
{
    instances.clear();
    airPuffs.clear();
    currentWorld = nullptr;
    generation = 0;
}

void SoftBodyRuntime::InitializeInstance(
    World& world,
    const WorldSoftBodyObject& descriptor)
{
    Physics::Softbody::RubberShellSettings settings;
    settings.radius = descriptor.radius;
    settings.subdivisions = descriptor.subdivisions;
    settings.burstStrain = descriptor.burstStrain;
    // 完整气球按近中性浮力处理，避免在爆裂前自己掉出画面。
    settings.gravity.setZero();
    Physics::Softbody::RubberShell shell =
        Physics::Softbody::RubberShell::BuildIcosphere(settings);
    Instance instance;
    instance.descriptor = descriptor;
    AddSoftMesh(world, instance, std::move(shell), "intact");
    const Eigen::Vector3f center = descriptor.model.block<3, 1>(0, 3);
    const float scale = std::max(0.001f, descriptor.model.block<3, 3>(0, 0).col(0).norm());
    impactWorld.RegisterSphereProxy(
        descriptor.name, center, static_cast<float>(descriptor.radius) * scale,
        Physics::FractureMaterialProfile::Balloon);
    diagnostics.ReportInfo(
        "SoftBody initialized: " + descriptor.name +
        " vertices=" + std::to_string(instance.shells.front().Particles().size()) +
        " triangles=" + std::to_string(instance.shells.front().Triangles().size()));
    instances.emplace(descriptor.name, std::move(instance));
}

void SoftBodyRuntime::AddSoftMesh(
    World& world,
    Instance& instance,
    Physics::Softbody::RubberShell shell,
    const std::string& suffix)
{
    const Physics::Softbody::SoftBodyRenderMesh renderMesh = shell.BuildRenderMesh();
    const std::vector<Vertex> vertices = BuildSoftBodyVertices(renderMesh);
    const std::string meshKey = BuildMeshKey(world, instance.descriptor.name, suffix);
    auto object = std::make_shared<RenderableObject>(
        vertices, renderMesh.indices, rendererBackend, meshKey, true);
    RendererResourceCache::GetInstance().BindRenderableObject(meshKey, std::move(object));

    WorldMeshObject mesh;
    mesh.debugName = suffix == "intact" ? instance.descriptor.name :
        instance.descriptor.name + "~" + suffix;
    mesh.sceneObjectIdentity = instance.descriptor.name;
    mesh.materialSlotName = "SoftBody";
    mesh.model = instance.descriptor.model;
    mesh.scenePosition = instance.descriptor.scenePosition;
    mesh.sceneRotation = instance.descriptor.sceneRotation;
    mesh.sceneScale = instance.descriptor.sceneScale;
    ComputeBounds(renderMesh, mesh.localBoundsMin, mesh.localBoundsMax);
    mesh.meshKey = meshKey;
    mesh.materialKey = instance.descriptor.materialKey;
    mesh.materialInstanceKey = instance.descriptor.materialInstanceKey;

    const std::string objectName = suffix == "intact" ? instance.descriptor.name :
        instance.descriptor.name + "~" + suffix;
    world.AddMeshObject(objectName, std::move(mesh));
    instance.shells.push_back(std::move(shell));
    instance.meshObjectNames.push_back(objectName);
    instance.meshKeys.push_back(meshKey);
}

void SoftBodyRuntime::UpdateSoftMesh(
    World& world,
    const std::string& meshObjectName,
    const std::string& meshKey,
    const Physics::Softbody::RubberShell& shell)
{
    const Physics::Softbody::SoftBodyRenderMesh renderMesh = shell.BuildRenderMesh();
    const auto* object = RendererResourceCache::GetInstance().GetRenderableObject(meshKey);
    if (object == nullptr || !*object)
    {
        diagnostics.ReportWarning("SoftBody mesh is not resident: " + meshKey);
        return;
    }
    (*object)->UpdateVertices(BuildSoftBodyVertices(renderMesh));
    Eigen::Vector3f minimum, maximum;
    ComputeBounds(renderMesh, minimum, maximum);
    world.SetMeshObjectLocalBounds(meshObjectName, minimum, maximum);
}

void SoftBodyRuntime::DeflateInstance(Instance& instance, float impactSpeed,
    const Eigen::Vector3f& worldImpactPoint)
{
    if (instance.shells.empty() || instance.leaking) return;
    instance.leaking = true;
    instance.leakAge = 0.0f;
    // 破口方向：局部命中点方向的反向 = 气球被喷气推走的方向。
    const Eigen::Vector3f localImpact = (
        instance.descriptor.model.inverse() *
        worldImpactPoint.homogeneous()).head<3>();
    Eigen::Vector3f thrust = Eigen::Vector3f::UnitY();
    if (localImpact.norm() > 1e-4f)
    {
        instance.leakHoleDirection = localImpact.normalized();
        thrust = -instance.leakHoleDirection;
    }
    instance.shells[0].SetRenderHole(
        instance.leakHoleDirection.cast<double>(), 0.12);
    const float wobble = UnitFromBits(
        MixBits(instance.descriptor.seed + 0x2545F491ull)) * 0.5f - 0.25f;
    thrust = (thrust + Eigen::Vector3f(wobble, wobble * 0.6f, wobble * 0.4f)).normalized();
    instance.leakThrustLocal = thrust;
    const uint64_t spinSeed = MixBits(instance.descriptor.seed + 0x9E3779B9ull);
    instance.leakSpinAxisLocal = Eigen::Vector3f(
        UnitFromBits(spinSeed) * 2.0f - 1.0f,
        UnitFromBits(spinSeed ^ 0x5163ull) * 2.0f - 1.0f,
        UnitFromBits(spinSeed ^ 0x2F1Bull) * 2.0f - 1.0f);
    if (instance.leakSpinAxisLocal.norm() <= 1e-4f)
        instance.leakSpinAxisLocal = Eigen::Vector3f::UnitX();
    instance.leakSpinAxisLocal.normalize();
    Physics::Softbody::RubberShell& shell = instance.shells[0];
    // 漏气后气球不再悬浮：开始下落，由地面钳制接住。
    shell.SetGravity(Eigen::Vector3d(0.0, -981.0, 0.0));
    shell.SetDamping(3.0);
    const double speedScale =
        std::clamp(static_cast<double>(impactSpeed) / 4.0, 0.2, 1.0);
    shell.ApplyDeflateImpulse(Eigen::Vector3d::Zero(),
        45.0 * speedScale, 12.0 * speedScale,
        instance.descriptor.seed + 0x51ED270Bull);
    if (audioRuntime)
    {
        audioRuntime->PlayBalloonLeak(1.0f - 0.4f * static_cast<float>(speedScale),
            instance.descriptor.model.block<3, 1>(0, 3));
    }
    diagnostics.ReportInfo(
        "SoftBody leak: " + instance.descriptor.name +
        " speed=" + std::to_string(impactSpeed));
}

void SoftBodyRuntime::BurstInstance(World& world, Instance& instance,
    float impactSpeed, const std::optional<Eigen::Vector3f>& worldImpactPoint)
{
    if (instance.shells.empty()) return;
    // 破口拓扑必须从真实命中点开始：箭射到哪，裂纹就从哪里扩散。
    // 局部空间以气球中心为原点，命中点方向即球面破口方向。
    Eigen::Vector3f localPuncture(0.3f, 0.4f, 0.86f);
    localPuncture.normalize();
    if (worldImpactPoint)
    {
        const Eigen::Vector3f local = (
            instance.descriptor.model.inverse() *
            worldImpactPoint->homogeneous()).head<3>();
        if (local.norm() > 1e-4f) localPuncture = local.normalized();
    }
    Physics::Fracture::SphericalFractureSettings settings;
    settings.seed = instance.descriptor.seed;
    // 先按剩余全局预算裁掉本次爆裂的碎片数；预算不足时先回收其他对象
    // 已静止的旧碎片。Request 是纯查询，回收在决策返回后统一执行。
    Physics::Fracture::FractureBudgetRequest budgetRequest;
    budgetRequest.owner = instance.descriptor.name;
    budgetRequest.kind = Physics::Fracture::FractureBudgetKind::BalloonShard;
    budgetRequest.count = std::min<std::size_t>(
        static_cast<std::size_t>(instance.descriptor.siteCount),
        Physics::Fracture::FractureBudget::BalloonLimit);
    const Physics::Fracture::FractureBudgetDecision budgetDecision =
        budget.Request(budgetRequest);
    if (!budgetDecision.evictions.empty())
        evictionDispatcher.EvictFractureBudgetShards(budgetDecision.evictions);
    const std::size_t grantedShards = std::clamp<std::size_t>(
        budgetDecision.grantedCount,
        Physics::Fracture::FractureBudget::MinimumBalloonShards,
        Physics::Fracture::FractureBudget::BalloonLimit);
    settings.siteCount = static_cast<int>(grantedShards);
    if (budgetDecision.exceededLimit)
    {
        // 全局预算已满且没有可回收软片：保留视觉下限，但把超限量写进日志。
        diagnostics.ReportWarning(
            "SoftBody burst forced minimum fragment count: " +
            instance.descriptor.name +
            " granted=" + std::to_string(grantedShards) +
            " requested=" + std::to_string(budgetRequest.count));
    }
    // 爆裂点附近更强的环状种子密度 + 大角度抖动：碎片是宽度不均的橡皮长条，
    // 并且在远离破口处自然出现分叉，而不是一圈等宽橘子瓣。
    settings.impactBias = 0.88;
    settings.angularJitter = 0.80;
    settings.radius = instance.descriptor.radius;
    settings.thickness = instance.descriptor.thickness;
    settings.impactDirection = localPuncture.cast<double>();
    const Physics::Fracture::SphericalPattern pattern =
        Physics::Fracture::GenerateSphericalBurstPattern(settings);
    Physics::Softbody::RubberTearSettings tear;
    tear.restContraction = instance.descriptor.restContraction;
    std::vector<Physics::Softbody::RubberShell> fragments =
        Physics::Softbody::RubberShell::TearBySphericalPatternExact(
            instance.shells[0], pattern, tear);
    // 破裂后橡胶软片才进入自由落体；完整气球保持悬浮。
    const float energyScale = std::clamp(
        0.75f + std::max(0.0f, impactSpeed) / 12.0f, 0.75f, 1.45f);
    const double radialImpulse = 220.0 * energyScale;
    const double tangentialImpulse = 28.0 + 32.0 * energyScale;
    for (size_t index = 0; index < fragments.size(); ++index)
    {
        Physics::Softbody::RubberShell& fragment = fragments[index];
        fragment.SetGravity(Eigen::Vector3d(0.0, -981.0, 0.0));
        fragment.SetDamping(4.0);
        fragment.ApplyBurstImpulse(
            Eigen::Vector3d::Zero(), radialImpulse, tangentialImpulse,
            instance.descriptor.seed + index * 0x9E3779B9ull,
            localPuncture.cast<double>(), 0.9);
    }
    int neckIndex = -1;
    double lowestCenterY = std::numeric_limits<double>::max();
    for (size_t index = 0; index < fragments.size(); ++index)
    {
        double centerY = 0.0;
        for (const auto& particle : fragments[index].Particles())
            centerY += particle.position.y();
        if (!fragments[index].Particles().empty())
            centerY /= static_cast<double>(fragments[index].Particles().size());
        if (centerY < lowestCenterY)
        {
            lowestCenterY = centerY;
            neckIndex = static_cast<int>(index);
        }
    }
    if (neckIndex >= 0 && lowestCenterY > -0.5 * instance.descriptor.radius)
        neckIndex = -1;
    if (neckIndex >= 0)
    {
        fragments[static_cast<size_t>(neckIndex)].PinNearest(
            Eigen::Vector3d(0.0, -instance.descriptor.radius, 0.0), 3);
    }
    if (fragments.empty())
    {
        diagnostics.ReportWarning(
            "SoftBody burst produced no fragments: " + instance.descriptor.name);
    }
    diagnostics.ReportInfo(
        "SoftBody burst: " + instance.descriptor.name +
        " fragments=" + std::to_string(fragments.size()));
    const Eigen::Vector3f burstCenter =
        instance.descriptor.model.block<3, 1>(0, 3);
    const float scale = std::max(0.001f,
        instance.descriptor.model.block<3, 3>(0, 0).col(0).norm());
    if (audioRuntime) audioRuntime->PlayBalloonBurst(
        std::clamp(energyScale, 0.75f, 1.45f), burstCenter);
    AirPuff puff;
    puff.owner = instance.descriptor.name;
    // 有真实命中点时，气团从破口喷出；压力自爆则从气球中心向外泄压。
    puff.center = worldImpactPoint ? *worldImpactPoint : burstCenter;
    puff.radius = static_cast<float>(instance.descriptor.radius) * scale *
        (worldImpactPoint ? 0.35f : 1.0f);
    airPuffs.push_back(std::move(puff));
    impactWorld.UnregisterSphereProxy(instance.descriptor.name);

    // 先移除完整气球；旧 RenderableObject 会在缓存/GPU epoch  Retirement 后释放。
    if (!instance.meshObjectNames.empty())
    {
        RemoveMeshObject(world, instance.meshObjectNames.front(),
            instance.meshKeys.front());
    }
    instance.shells.clear();
    instance.meshObjectNames.clear();
    instance.meshKeys.clear();
    instance.shardLastPose.clear();
    instance.shardPoseValid.clear();
    instance.shardBudgetIds.clear();
    instance.shardSettledSeconds.clear();
    instance.shardBoundingRadius.clear();
    instance.nextShardBudgetId = 0;
    instance.burst = true;

    const Eigen::Quaternionf balloonRotation =
        RotationFromModel(instance.descriptor.model, scale);
    const size_t fragmentCount = fragments.size();
    for (size_t index = 0; index < fragmentCount; ++index)
    {
        const auto& particles = fragments[index].Particles();
        Eigen::Vector3d localCenter = Eigen::Vector3d::Zero();
        Eigen::Vector3d localVelocity = Eigen::Vector3d::Zero();
        for (const auto& particle : particles)
        {
            localCenter += particle.position;
            localVelocity += particle.velocity;
        }
        if (!particles.empty())
        {
            localCenter /= static_cast<double>(particles.size());
            localVelocity /= static_cast<double>(particles.size());
        }
        double localRadius = 0.0;
        for (const auto& particle : particles)
            localRadius = std::max(localRadius, (particle.position - localCenter).norm());
        const Eigen::Vector3f worldCenter = (
            instance.descriptor.model * Eigen::Vector4f(
                localCenter.cast<float>().x(), localCenter.cast<float>().y(),
                localCenter.cast<float>().z(), 1.0f)).head<3>();
        const Eigen::Vector3f worldVelocity =
            instance.descriptor.model.block<3, 3>(0, 0) *
            localVelocity.cast<float>();
        const std::string shardName =
            instance.descriptor.name + "~shard" + std::to_string(index);
        const bool neck = static_cast<int>(index) == neckIndex;
        Eigen::Matrix4f initialPose = Eigen::Matrix4f::Identity();
        bool hasPose = false;
        if (!neck)
        {
            std::vector<Eigen::Vector3f> hullPoints;
            hullPoints.reserve(particles.size());
            for (const auto& particle : particles)
                hullPoints.push_back(
                    ((particle.position - localCenter).cast<float>()) * scale);
            try
            {
                impactWorld.RegisterShard(instance.descriptor.name, shardName,
                    worldCenter, balloonRotation,
                    worldVelocity,
                    std::max(0.05f, static_cast<float>(localRadius * localRadius) * 0.001f),
                    hullPoints,
                    Physics::FractureMaterialProfile::Balloon,
                    ShardAngularVelocity(
                        instance.descriptor.seed, index, energyScale));
                const auto pose = impactWorld.GetShardPose(
                    instance.descriptor.name, shardName);
                if (pose)
                {
                    initialPose.block<3, 3>(0, 0) =
                        pose->rotation.toRotationMatrix();
                    initialPose.block<3, 1>(0, 3) = pose->position;
                    hasPose = true;
                }
            }
            catch (const std::exception& error)
            {
                diagnostics.ReportWarning(
                    "SoftBody shard proxy failed: " + shardName + " error=" + error.what());
            }
        }
        AddSoftMesh(world, instance, std::move(fragments[index]),
            "shard" + std::to_string(index));
        instance.shardLastPose.push_back(initialPose);
        instance.shardPoseValid.push_back(hasPose);
        instance.shardBudgetIds.push_back(instance.nextShardBudgetId++);
        instance.shardSettledSeconds.push_back(0.0f);
        instance.shardBoundingRadius.push_back(
            static_cast<float>(localRadius) * scale);
    }
    budget.OnShardsCreated(instance.descriptor.name,
        Physics::Fracture::FractureBudgetKind::BalloonShard, fragmentCount);
    if (fragmentCount > budgetDecision.grantedCount)
    {
        diagnostics.ReportWarning(
            "SoftBody burst exceeded fragment budget: " +
            instance.descriptor.name +
            " fragments=" + std::to_string(fragmentCount) +
            " granted=" + std::to_string(budgetDecision.grantedCount));
    }
}

void SoftBodyRuntime::RemoveMeshObject(
    World& world,
    const std::string& meshObjectName,
    const std::string& meshKey)
{
    world.RemoveMeshObject(meshObjectName);
    RendererResourceCache& cache = RendererResourceCache::GetInstance();
    std::shared_ptr<RenderableObject> retired;
    const auto* cached = cache.GetRenderableObject(meshKey);
    if (cached != nullptr && *cached) retired = *cached;
    cache.BindRenderableObject(meshKey, {});
    if (retired)
    {
        // 旧网格可能仍被上一帧已提交的 command buffer 引用；不能在 GT 上直接析构。
        ResourceRetireQueue::GetInstance().RetireShared(
            "SoftBodyMesh:" + meshKey,
            generation,
            ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch(),
            std::move(retired));
    }
}

float SoftBodyRuntime::CameraDistance(const Eigen::Vector3f& worldPosition) const
{
    if (currentWorld == nullptr) return 0.0f;
    const std::shared_ptr<Camera>& camera = currentWorld->GetRenderCamera();
    if (!camera) return 0.0f;
    return (worldPosition - camera->GetPosition()).norm();
}

void SoftBodyRuntime::RemoveShardAt(
    World& world, Instance& instance, size_t shardIndex)
{
    if (shardIndex >= instance.meshObjectNames.size()) return;
    const std::string shardName = instance.meshObjectNames[shardIndex];
    const std::string meshKey = instance.meshKeys[shardIndex];
    if (shardIndex < instance.shardPoseValid.size() &&
        instance.shardPoseValid[shardIndex])
        impactWorld.RemoveShard(instance.descriptor.name, shardName);
    RemoveMeshObject(world, shardName, meshKey);

    // shells 与 mesh 名/key/姿态/budget 记录全部按下标对齐，必须一起删除。
    instance.shells.erase(
        instance.shells.begin() + static_cast<std::ptrdiff_t>(shardIndex));
    instance.meshObjectNames.erase(instance.meshObjectNames.begin() +
        static_cast<std::ptrdiff_t>(shardIndex));
    instance.meshKeys.erase(
        instance.meshKeys.begin() + static_cast<std::ptrdiff_t>(shardIndex));
    if (shardIndex < instance.shardLastPose.size())
        instance.shardLastPose.erase(instance.shardLastPose.begin() +
            static_cast<std::ptrdiff_t>(shardIndex));
    if (shardIndex < instance.shardPoseValid.size())
        instance.shardPoseValid.erase(instance.shardPoseValid.begin() +
            static_cast<std::ptrdiff_t>(shardIndex));
    if (shardIndex < instance.shardBudgetIds.size())
        instance.shardBudgetIds.erase(instance.shardBudgetIds.begin() +
            static_cast<std::ptrdiff_t>(shardIndex));
    if (shardIndex < instance.shardSettledSeconds.size())
        instance.shardSettledSeconds.erase(
            instance.shardSettledSeconds.begin() +
            static_cast<std::ptrdiff_t>(shardIndex));
    if (shardIndex < instance.shardBoundingRadius.size())
        instance.shardBoundingRadius.erase(
            instance.shardBoundingRadius.begin() +
            static_cast<std::ptrdiff_t>(shardIndex));
}

void SoftBodyRuntime::EvictFractureBudgetShards(
    const std::vector<Physics::Fracture::FractureBudgetCandidate>& candidates)
{
    if (currentWorld == nullptr) return;
    for (const Physics::Fracture::FractureBudgetCandidate& candidate : candidates)
    {
        const auto found = instances.find(candidate.owner);
        if (found == instances.end()) continue;
        Instance& instance = found->second;
        if (!instance.burst) continue;

        size_t shardIndex = instance.shardBudgetIds.size();
        for (size_t index = 0; index < instance.shardBudgetIds.size(); ++index)
            if (instance.shardBudgetIds[index] == candidate.shardId)
            {
                shardIndex = index;
                break;
            }
        if (shardIndex >= instance.shardBudgetIds.size()) continue;

        RemoveShardAt(*currentWorld, instance, shardIndex);
        budget.OnShardsRemoved(instance.descriptor.name,
            Physics::Fracture::FractureBudgetKind::BalloonShard);
        budget.UntrackReclaimable(candidate.owner, candidate.shardId);
    }
}

std::string SoftBodyRuntime::BuildMeshKey(
    const World& world,
    const std::string& softBodyName,
    const std::string& suffix) const
{
    return "softbody|" + world.GetScenePath() + "|" + softBodyName + "|" +
        std::to_string(generation) + "|" + suffix;
}

} // namespace VL
