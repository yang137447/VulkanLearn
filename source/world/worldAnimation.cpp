#include "world/world.h"
#include "animation/skeletalPlayback.h"
#include "animation/animationScriptData.h"
#include "script/scriptScene.h"
#include "script/scriptComponents.h"
#include <map>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <set>
#include <algorithm>

namespace VL
{
namespace
{
Debug::Color FootCorrectionColor(float t)
{
    const float red[3] = {1.0f, 0.15f, 0.1f};
    const float green[3] = {0.2f, 1.0f, 0.2f};
    return {red[0] + (green[0] - red[0]) * t,
            red[1] + (green[1] - red[1]) * t,
            red[2] + (green[2] - red[2]) * t, 1};
}

// 足部修正用红→绿分段线表达“pre-IK → post-IK”方向；单条线只有单色，
// 分段插值让纠正量在场景里一眼可读。
void DrawFootCorrectionGradient(Debug::DrawDebug& debug, const Eigen::Vector3f& begin,
    const Eigen::Vector3f& end, Debug::Style style)
{
    constexpr int segments = 8;
    for (int segment = 0; segment < segments; ++segment)
    {
        const float t0 = static_cast<float>(segment) / segments;
        const float t1 = static_cast<float>(segment + 1) / segments;
        style.color = FootCorrectionColor((t0 + t1) * 0.5f);
        debug.DrawDebugLine(begin + (end - begin) * t0, begin + (end - begin) * t1, style);
    }
}

// playMontage 的 targets 只接受“名称 -> 3 或 7 个有限数”的对象；键集合的完整匹配由
// animationScriptData 按 Montage 的 warp target 做最终校验。
//   3 个数：[x,y,z]                              —— 旧 C# 纯位置调用，不提交目标旋转
//   7 个数：[x,y,z, qx,qy,qz,qw]                 —— 位置 + UE 四元数 (x,y,z,w)
// 旋转是目标世界朝向；非单位四元数在此归一化。数值/形状非法一律
// Script.InvalidMontageTargets，不做静默降级。
std::map<std::string, Animation::MotionWarpTarget> ParseMontageTargets(const nlohmann::json& request)
{
    std::map<std::string, Animation::MotionWarpTarget> targets;
    if (!request.contains("targets") || request.at("targets").is_null()) return targets;
    const auto& values = request.at("targets");
    if (!values.is_object() || values.size() > 32)
        throw std::invalid_argument("Script.InvalidMontageTargets");
    for (const auto& entry : values.items())
    {
        if (entry.key().empty() || entry.key().size() > 64 || !entry.value().is_array() ||
            (entry.value().size() != 3 && entry.value().size() != 7))
            throw std::invalid_argument("Script.InvalidMontageTargets");
        std::array<float, 7> numbers{};
        for (size_t index = 0; index < entry.value().size(); ++index)
        {
            if (!entry.value().at(index).is_number())
                throw std::invalid_argument("Script.InvalidMontageTargets");
            numbers[index] = entry.value().at(index).get<float>();
            if (!std::isfinite(numbers[index]))
                throw std::invalid_argument("Script.InvalidMontageTargets");
        }
        Animation::MotionWarpTarget target;
        target.location = Eigen::Vector3f(numbers[0], numbers[1], numbers[2]);
        if (entry.value().size() == 7)
        {
            // Eigen 构造顺序是 (w,x,y,z)，桥接数组是 UE 的 (x,y,z,w)。
            Eigen::Quaternionf rotation(numbers[6], numbers[3], numbers[4], numbers[5]);
            if (!(rotation.squaredNorm() > 1e-8f) || !std::isfinite(rotation.squaredNorm()))
                throw std::invalid_argument("Script.InvalidMontageTargets");
            target.rotation = rotation.normalized();
            target.hasRotation = true;
        }
        targets.emplace(entry.key(), target);
    }
    return targets;
}

// playMontage 的可选入口起始时间（秒、clip-local）：缺省 0 = 从 section 0 起点播放。
// 这里只校验 wire 形状（数字/有限/非负）；是否落在 section 0 的播放范围内由原生
// PlayMontage 按资产判定，两者拒绝码不同（Script.InvalidMontageStartTime vs 带
// montage id/范围的执行层诊断），便于区分调用方字段错误与资产越界。
float ParseMontageStartTime(const nlohmann::json& request)
{
    if (!request.contains("startTime")) return 0;
    const auto& value = request.at("startTime");
    if (!value.is_number())
        throw std::invalid_argument("Script.InvalidMontageStartTime");
    const float startTime = value.get<float>();
    if (!std::isfinite(startTime) || startTime < 0)
        throw std::invalid_argument("Script.InvalidMontageStartTime");
    return startTime;
}
}

void World::TickAnimations(float deltaSeconds)
{
    std::unordered_set<Animation::SkeletalPlayback*> updated;
    for (auto& entry : meshObjects)
    {
        auto& object = entry.second;
        if (!object.animation) continue;
        if (object.sceneObjectIdentity.rfind("__blueprint/", 0) == 0) continue;
        if (updated.insert(object.animation.get()).second)
            object.animation->Tick(deltaSeconds);
        object.skinPose = object.animation->Pose(object.skinSection);
        object.localBoundsMin = object.skinPose->boundsMin;
        object.localBoundsMax = object.skinPose->boundsMax;
    }
}

nlohmann::json World::ScriptAnimationCatalog() const
{
    auto result = nlohmann::json::object();
    std::unordered_map<std::string, const Animation::SkeletalPlayback*> players;
    for (const auto& entry : meshObjects)
        if (entry.second.animation) players.emplace(entry.second.sceneObjectIdentity, entry.second.animation.get());
    if (players.empty()) return result;
    for (const auto& actor : scriptResolvedBlueprints)
        for (const auto& node : actor.value("components", nlohmann::json::array()))
        {
            if (node.at("type") != "mesh") continue;
            const auto identity = ScriptMeshIdentity(actor.at("id"), node);
            const auto found = players.find(identity);
            if (found != players.end())
            {
                auto clips = nlohmann::json::array();
                for (const auto& clip : found->second->Asset().clips) clips.push_back(clip.id);
                result[node.at("modelPath").get<std::string>()] = std::move(clips);
            }
        }
    return result;
}

nlohmann::json World::ScriptAnimationAssets() const
{
    auto result = nlohmann::json::object();
    std::unordered_map<std::string, const Animation::SkeletalPlayback*> players;
    for (const auto& entry : meshObjects)
        if (entry.second.animation) players.emplace(entry.second.sceneObjectIdentity, entry.second.animation.get());
    if (players.empty()) return result;
    // 同一 modelPath 可能被多个组件/模板共享；catalog 按 modelPath 索引，只构造一次完整资产段，
    // 避免把同一份 22 个 sidecar 的曲线键重复序列化到每个组件。
    for (const auto& actor : scriptResolvedBlueprints)
        for (const auto& node : actor.value("components", nlohmann::json::array()))
        {
            if (node.at("type") != "mesh") continue;
            const auto identity = ScriptMeshIdentity(actor.at("id"), node);
            const auto found = players.find(identity);
            if (found == players.end()) continue;
            const std::string modelPath = node.at("modelPath").get<std::string>();
            if (result.contains(modelPath)) continue;
            result[modelPath] = Animation::BuildAnimationScriptAssets(found->second->Asset());
        }
    return result;
}

nlohmann::json World::ScriptAnimationFeedback() const
{
    auto result = nlohmann::json::array();
    std::unordered_map<std::string, const Animation::SkeletalPlayback*> players;
    for (const auto& entry : meshObjects)
        if (entry.second.animation) players.emplace(entry.second.sceneObjectIdentity, entry.second.animation.get());
    if (players.empty()) return result;
    for (const auto& actor : scriptBlueprintStates)
        for (const auto& node : actor.value("components", nlohmann::json::array()))
        {
            if (node.at("type") != "mesh") continue;
            const auto identity = ScriptMeshIdentity(actor.at("id"), node);
            const auto found = players.find(identity);
            if (found != players.end())
            {
                const auto& player = *found->second;
                const auto& clips = player.Asset().clips;
                nlohmann::json feedback{{"owner", actor.at("id")}, {"component", ScriptComponentId(node)},
                    {"clip", clips.empty() ? "" : clips.at(player.SelectedClip()).id},
                    {"time", player.Time()}, {"duration", player.Duration()},
                    {"completed", player.Completed()}, {"playing", player.IsPlaying()}, {"rate", player.Rate()}};
                const auto& rootMotion = player.RootMotionDelta();
                feedback["rootMotion"] = nlohmann::json::array(
                    {rootMotion.x(), rootMotion.y(), rootMotion.z()});
                feedback["rootMotionYaw"] = player.RootMotionYawDelta();
                const auto& total = player.RootMotionTotal();
                feedback["rootMotionTotal"] = nlohmann::json::array(
                    {total.x(), total.y(), total.z()});
                const auto& remaining = player.RootMotionRemaining();
                feedback["rootMotionRemaining"] = nlohmann::json::array(
                    {remaining.x(), remaining.y(), remaining.z()});
                feedback["warp"] = player.MotionWarpState();
                // notify 事件与 Montage 身份都来自唯一播放器；本快照在脚本 tick 之前发布，
                // 因此 C# 看到的是上一动画 tick 的事件（既有反馈时序不变）。
                feedback["notifies"] = Animation::BuildAnimationNotifyFeedback(player.FiredNotifies());
                feedback["montage"] = {{"id", player.MontageId()},
                    {"length", player.MontageLength()},
                    {"range", nlohmann::json::array(
                        {player.PlaybackRangeStart(), player.PlaybackRangeEnd()})}};
                const auto& placement = player.FootPlacement();
                auto feet = nlohmann::json::array();
                for (const auto& foot : placement.feet)
                    feet.push_back({{"grounded", foot.grounded}, {"planted", foot.planted},
                        {"anchor", {foot.anchor.x(), foot.anchor.y(), foot.anchor.z()}},
                        {"preSole", {foot.preSole.x(), foot.preSole.y(), foot.preSole.z()}},
                        {"postSole", {foot.postSole.x(), foot.postSole.y(), foot.postSole.z()}},
                        {"groundPoint", {foot.groundPoint.x(), foot.groundPoint.y(), foot.groundPoint.z()}},
                        {"groundNormal", {foot.groundNormal.x(), foot.groundNormal.y(), foot.groundNormal.z()}},
                        {"probeOrigin", {foot.probeOrigin.x(), foot.probeOrigin.y(), foot.probeOrigin.z()}},
                        {"probeDisplacement", {foot.probeDisplacement.x(), foot.probeDisplacement.y(), foot.probeDisplacement.z()}},
                        {"heelGrounded", foot.heelGrounded},
                        {"preHeel", {foot.preHeel.x(), foot.preHeel.y(), foot.preHeel.z()}},
                        {"postHeel", {foot.postHeel.x(), foot.postHeel.y(), foot.postHeel.z()}},
                        {"heelGroundPoint", {foot.heelGroundPoint.x(), foot.heelGroundPoint.y(), foot.heelGroundPoint.z()}},
                        {"heelGroundNormal", {foot.heelGroundNormal.x(), foot.heelGroundNormal.y(), foot.heelGroundNormal.z()}},
                        {"heelProbeOrigin", {foot.heelProbeOrigin.x(), foot.heelProbeOrigin.y(), foot.heelProbeOrigin.z()}},
                        {"heelProbeDisplacement", {foot.heelProbeDisplacement.x(), foot.heelProbeDisplacement.y(), foot.heelProbeDisplacement.z()}},
                        {"heelSupportIdentity", foot.heelSupportIdentity}, {"contactMode", foot.contactMode},
                        {"targetError", foot.targetError}, {"supportIdentity", foot.supportIdentity},
                        {"soleSpeed", foot.soleSpeed}, {"heelSpeed", foot.heelSpeed},
                        {"soleGap", foot.soleGap}, {"heelGap", foot.heelGap},
                        {"canPlantBall", foot.canPlantBall}, {"canPlantHeel", foot.canPlantHeel}});
                feedback["footPlacement"] = {{"enabled", placement.enabled}, {"active", placement.active},
                    {"pelvisOffset", placement.pelvisOffset}, {"reason", placement.reason},
                    {"lockMode", placement.lockMode}, {"feet", std::move(feet)}};
                const auto& root = player.RootOffset();
                feedback["inertialization"] = player.InertializationEnabled();
                feedback["rootOffset"] = {{"enabled", root.enabled}, {"active", root.active},
                    {"worldTranslation", {root.worldTranslation.x(), root.worldTranslation.y(), root.worldTranslation.z()}},
                    {"yawRadians", root.yawRadians}, {"reason", root.reason}};
                const auto runtime = motionRuntimes.find(identity);
                if (runtime != motionRuntimes.end())
                {
                    const auto& motion = runtime->second;
                    feedback["motion"] = {{"databaseIdentity", motion.database->identity}, {"queryReady", motion.queryReady},
                        {"decision", motion.result.reason}, {"searchCount", motion.searchCount},
                        {"selectedClip", motion.result.selected ? motion.result.selected->clipId : ""},
                        {"selectedTime", motion.result.selected ? motion.result.selected->time : 0}};
                    const auto& speed = motion.speedMatch;
                    feedback["motion"]["speedMatching"] = {{"enabled", speed.enabled}, {"active", speed.active},
                        {"sourceSpeed", speed.sourceSpeed}, {"actualSpeed", speed.actualSpeed},
                        {"targetRate", speed.targetRate}, {"rate", speed.rate},
                        {"residualSpeed", speed.residualSpeed}, {"reason", speed.reason}};
                }
                result.push_back(std::move(feedback));
            }
        }
    return result;
}

void World::ApplyScriptAnimationRequests(const nlohmann::json& actor, const nlohmann::json& requests)
{
    if (actor.value("destroyed", false) || actor.value("failed", false) || !actor.value("enabled", true)) return;
    if (!requests.is_array() || requests.size() > 128)
        throw std::invalid_argument("Script.InvalidAnimationBatch");
    std::map<Animation::SkeletalPlayback*, Animation::SkeletalPlayback> candidates;
    auto runtimeCandidates = motionRuntimes;
    auto databaseCandidates = motionDatabases;
    for (const auto& request : requests)
    {
        const std::string component = request.at("component");
        std::string identity;
        std::string modelPath;
        for (const auto& node : actor.value("components", nlohmann::json::array()))
            if (node.at("type") == "mesh" && ScriptComponentId(node) == component)
            {
                identity = ScriptMeshIdentity(actor.at("id"), node);
                modelPath = node.at("modelPath");
            }
        Animation::SkeletalPlayback* player = nullptr;
        const WorldMeshObject* targetMesh = nullptr;
        for (const auto& entry : meshObjects)
            if (entry.second.sceneObjectIdentity == identity && entry.second.animation)
            { player = entry.second.animation.get(); targetMesh = &entry.second; break; }
        if (!player || candidates.count(player)) throw std::invalid_argument("Script.InvalidAnimationTarget");
        auto candidate = *player;
        if (request.value("mode", std::string()) == "motionMatching")
        {
            const std::string sourcePath = request.at("database");
            if (!request.at("allowedClips").is_array() || request.at("allowedClips").size() > 256 ||
                !request.at("actionLocked").is_boolean() ||
                (request.contains("matchSpeed") && !request.at("matchSpeed").is_boolean()) ||
                (request.contains("footPlacement") && !request.at("footPlacement").is_boolean()) ||
                (request.contains("inertialization") && !request.at("inertialization").is_boolean()) ||
                (request.contains("rootOffset") && !request.at("rootOffset").is_boolean()) ||
                (request.contains("footLock") && (!request.at("footLock").is_string() ||
                    (request.at("footLock") != "Auto" && request.at("footLock") != "Ball" &&
                        request.at("footLock") != "Ankle"))))
                throw std::invalid_argument("Script.InvalidMotionRequest");
            const auto source = std::filesystem::u8path(sourcePath);
            if (sourcePath.empty() || sourcePath.size() > 1024 || source.is_absolute() ||
                source.has_root_name() || sourcePath.find('\\') != std::string::npos ||
                sourcePath.find(':') != std::string::npos || source.lexically_normal().generic_string() != sourcePath ||
                source.extension() != ".json" ||
                (sourcePath.rfind("Common/", 0) != 0 && sourcePath.rfind("Maps/", 0) != 0))
                throw std::invalid_argument("Motion database requires a formal resource path");
            std::vector<std::string> allowed = request.at("allowedClips").get<std::vector<std::string>>();
            std::set<std::string> unique;
            for (const auto& clip : allowed)
                if (!unique.insert(clip).second)
                    throw std::invalid_argument("Motion candidates require unique clip IDs");
            std::string key = sourcePath + "|" + modelPath;
            for (const auto& input : player->Asset().sourceInputs) key += "|" + input.first + ":" + input.second;
            auto& database = databaseCandidates[key];
            if (!database)
            {
                if (motionResourceRoot.empty()) throw std::invalid_argument("Motion resource root is not configured");
                database = Animation::LoadMotionRuntimeDatabase(motionResourceRoot, sourcePath, modelPath, player->Asset());
            }
            for (const auto& id : allowed)
                if (std::find(database->search->Data().clipIds.begin(), database->search->Data().clipIds.end(), id) ==
                    database->search->Data().clipIds.end())
                    throw std::invalid_argument("Motion allowed clip is outside the requested database");
            auto& runtime = runtimeCandidates[identity];
            if (runtime.database != database || runtime.playbackIdentity != player->Pose(0)->playbackIdentity)
            {
                runtime = {};
                runtime.database = database;
                runtime.playbackIdentity = player->Pose(0)->playbackIdentity;
            }
            runtime.allowedClips = std::move(allowed);
            runtime.actionLocked = request.at("actionLocked");
            runtime.matchSpeed = request.value("matchSpeed", false);
            const bool footPlacement = request.value("footPlacement", false);
            if (footPlacement) Animation::FootPlacementSolver::ValidateComponent(targetMesh->model);
            candidate.SetFootPlacementEnabled(footPlacement);
            const std::string footLock = request.value("footLock", std::string("Auto"));
            candidate.SetFootPlacementLockType(footLock == "Ankle" ?
                Animation::FootLockType::PivotAroundAnkle : footLock == "Ball" ?
                    Animation::FootLockType::PivotAroundBall : Animation::FootLockType::Auto);
            candidate.SetInertializationEnabled(request.value("inertialization", false));
            const bool rootOffset = request.value("rootOffset", false);
            if (rootOffset) Animation::FootPlacementSolver::ValidateComponent(targetMesh->model);
            candidate.SetRootOffsetEnabled(rootOffset);
            candidate.ClearMotionWarp();
            if (runtimeCandidates.size() > 8)
                throw std::invalid_argument("Motion matching supports at most eight controlled components");
        }
        else if (request.value("mode", std::string()) == "playMontage")
        {
            // 新 op 只在候选播放器上应用；任何校验失败都不改变当前播放器。
            // 旧 clip 请求仍走下面的分支，字段与语义保持不变。
            if (!request.contains("montage") || !request.at("montage").is_string())
                throw std::invalid_argument("Script.InvalidMontageRequest");
            const std::string montageId = request.at("montage").get<std::string>();
            if (montageId.empty() || montageId.size() > 256)
                throw std::invalid_argument("Script.InvalidMontageRequest");
            const Animation::Montage* montage = player->Asset().FindMontage(montageId);
            if (!montage)
                throw std::invalid_argument("Script.UnknownAnimationMontage: " + montageId);
            const auto targets = ParseMontageTargets(request);
            const bool loop = request.value("loop", false);
            const float rate = request.value("rate", 1.0f);
            const bool inPlace = request.value("inPlace", true);
            const float startTime = ParseMontageStartTime(request);
            candidate.SetFootPlacementEnabled(false);
            candidate.SetInertializationEnabled(false);
            candidate.SetRootOffsetEnabled(false);
            Animation::PlayMontage(candidate, *montage, targets, loop, rate, inPlace, startTime);
            // Montage 播放段不属于 Motion Matching；旧搜索绑定不能继续驱动同一组件。
            runtimeCandidates.erase(identity);
        }
        else
        {
            // clip 请求不再携带 warp 窗口：warp target 由 Montage 资产声明，原生
            // MotionWarpWindow 只在 PlayMontage 内部按资产窗口装配。旧字段显式拒绝，
            // 不能静默忽略，否则调用方会以为自己的 target 生效了。
            if (request.contains("warpTargets"))
                throw std::invalid_argument(
                    "Script.WarpTargetsDefinedByMontage: warp targets are defined by montage assets");
            candidate.SetFootPlacementEnabled(false);
            candidate.Play(request.at("clip"), request.at("loop"), request.at("rate"), request.at("inPlace"));
            // 从 Montage 切回普通 clip 时清掉上一段留下的窗口与 warp 状态。
            candidate.SetMotionWarpWindows({});
            runtimeCandidates.erase(identity);
        }
        candidates.emplace(player, std::move(candidate));
    }
    // 所有目标都能求值后才提交，同一组件的 sections 保留唯一时钟。
    for (auto& candidate : candidates) *candidate.first = std::move(candidate.second);
    motionRuntimes.swap(runtimeCandidates);
    motionDatabases.swap(databaseCandidates);
}

void World::TickScriptAnimations(float deltaSeconds, const Animation::GroundProbe* ground)
{
    std::unordered_map<std::string, const nlohmann::json*> activeOwners;
    std::unordered_set<Animation::SkeletalPlayback*> updated;
    for (const auto& actor : scriptBlueprintStates)
    {
        if (actor.value("failed", false) || actor.value("destroyed", false) || !actor.value("enabled", true)) continue;
        for (const auto& node : actor.value("components", nlohmann::json::array()))
        {
            if (node.at("type") != "mesh") continue;
            const auto identity = ScriptMeshIdentity(actor.at("id"), node);
            activeOwners.emplace(identity, &actor);
        }
    }
    // 组件身份索引避免每帧 Actor × Mesh 全表扫描；sections 仍只推进一次时钟。
    for (auto& entry : meshObjects)
    {
        auto& mesh = entry.second;
        if (!mesh.animation) continue;
        const auto owner = activeOwners.find(mesh.sceneObjectIdentity);
        if (owner == activeOwners.end())
        {
            if (mesh.sceneObjectIdentity.rfind("__blueprint/", 0) != 0) continue;
            if (updated.insert(mesh.animation.get()).second)
            {
                mesh.animation->ResetFootPlacement("owner-inactive");
                mesh.animation->ResetRootOffset("owner-inactive");
            }
            mesh.skinPose = mesh.animation->Pose(mesh.skinSection);
            mesh.localBoundsMin = mesh.skinPose->boundsMin;
            mesh.localBoundsMax = mesh.skinPose->boundsMax;
            continue;
        }
        if (updated.insert(mesh.animation.get()).second)
        {
            const auto& actor = *owner->second;
            const auto runtime = motionRuntimes.find(mesh.sceneObjectIdentity);
            if (runtime == motionRuntimes.end())
            {
                // 原生 MotionWarping 需要当前可视 Mesh、胶囊中心与 Actor yaw；窗口只在
                // GT tick 内消费。脚本 rotation 是角度制，动画侧统一弧度。
                const Eigen::Vector3f actorPosition = ScriptVector(actor, "position");
                constexpr float degreesToRadians = 3.14159265358979323846f / 180.0f;
                const float actorYaw = ScriptVector(actor, "rotation").y() * degreesToRadians;
                mesh.animation->Tick(deltaSeconds, &mesh.model, &actorPosition, actorYaw);
            }
            else if (deltaSeconds > 0)
            {
                auto& track = motionTracks[mesh.sceneObjectIdentity];
                const auto& database = *runtime->second.database;
                if (track.playbackIdentity != mesh.animation->Pose(0)->playbackIdentity ||
                    Animation::FeatureSchemaJson(track.schema) != Animation::FeatureSchemaJson(database.search->Data().schema))
                {
                    track = {};
                    track.playbackIdentity = mesh.animation->Pose(0)->playbackIdentity;
                    track.schema = database.search->Data().schema;
                    track.joints = database.joints;
                    runtime->second.matcher.Reset();
                }
                track.achievedVelocity = ScriptVector(actor, "bodyVelocity");
                track.walkVelocity = ScriptVector(actor, "walkVelocity");
                track.grounded = actor.value("grounded", true);
                Animation::TickMotionRuntime(runtime->second, track, *mesh.animation, mesh.model, deltaSeconds);
            }
            // 视觉根修正先于足部探测；MM 保留独立基础姿态，不能把滞后偏移反馈成下一次查询。
            if (runtime != motionRuntimes.end())
                mesh.animation->ApplyVisualRoot(mesh.model, ScriptVector(actor, "bodyVelocity"), deltaSeconds);
            // 唯一播放器先完成搜索/基础姿态与视觉根，再同步接地；各 section 领取同批不可变结果。
            if (ground)
                mesh.animation->ApplyFootPlacement(mesh.model, actor.value("grounded", false),
                    actor.at("id").get<std::string>(), deltaSeconds, *ground);
            else if (deltaSeconds > 0 && mesh.animation->IsPlaying())
            {
                mesh.animation->ResetFootPlacement("ground-unavailable");
                mesh.animation->ResetRootOffset("ground-unavailable");
            }
        }
        mesh.skinPose = mesh.animation->Pose(mesh.skinSection);
        mesh.localBoundsMin = mesh.skinPose->boundsMin;
        mesh.localBoundsMax = mesh.skinPose->boundsMax;
    }
    RecordScriptMotion(deltaSeconds);
    for (auto it = motionRuntimes.begin(); it != motionRuntimes.end();)
    {
        bool validIdentity = false;
        for (const auto& mesh : meshObjects)
            if (mesh.second.sceneObjectIdentity == it->first && mesh.second.animation &&
                mesh.second.animation->Pose(0)->playbackIdentity == it->second.playbackIdentity)
                validIdentity = true;
        if (!activeOwners.count(it->first) || !validIdentity)
            it = motionRuntimes.erase(it);
        else ++it;
    }
}

void World::ResetScriptAnimations()
{
    motionTracks.clear();
    motionRuntimes.clear();
    std::map<Animation::SkeletalPlayback*, std::shared_ptr<Animation::SkeletalPlayback>> replacements;
    for (auto& entry : meshObjects)
    {
        auto& mesh = entry.second;
        if (!mesh.animation || mesh.sceneObjectIdentity.rfind("__blueprint/", 0) != 0) continue;
        auto& player = replacements[mesh.animation.get()];
        if (!player) player = mesh.animation->NewInstance();
        mesh.animation = player;
        mesh.skinPose = player->Pose(mesh.skinSection);
        mesh.localBoundsMin = mesh.skinPose->boundsMin;
        mesh.localBoundsMax = mesh.skinPose->boundsMax;
    }
}

void World::RecordScriptMotion(float deltaSeconds)
{
    std::map<std::string, const WorldMeshObject*> players;
    for (const auto& entry : meshObjects)
        if (entry.second.animation)
            players.emplace(entry.second.sceneObjectIdentity, &entry.second);
    std::unordered_set<std::string> retained;
    // 最多八个已授权 MM 组件优先取得真实历史，普通调试骨骼再补余量；sections 不重复记录。
    for (int priority = 0; priority < 2; ++priority)
    for (const auto& actor : scriptBlueprintStates)
    {
        if (actor.value("failed", false) || actor.value("destroyed", false) || !actor.value("enabled", true)) continue;
        for (const auto& node : actor.value("components", nlohmann::json::array()))
        {
            if (node.at("type") != "mesh" || retained.size() >= 8) continue;
            const auto identity = ScriptMeshIdentity(actor.at("id"), node);
            if ((motionRuntimes.count(identity) != 0) != (priority == 0)) continue;
            const auto found = players.find(identity);
            if (found == players.end()) continue;
            retained.insert(identity);
            if (deltaSeconds <= 0) continue;
            const auto& mesh = *found->second;
            const auto& player = *mesh.animation;
            auto& track = motionTracks[identity];
            if (track.playbackIdentity != player.Pose(0)->playbackIdentity)
            {
                track = Animation::MotionTrack{};
                track.playbackIdentity = player.Pose(0)->playbackIdentity;
                track.schema = Animation::PreviewFeatureSchema(player.Asset().mesh);
                track.joints = Animation::ResolveFeatureJoints(player.Asset().mesh, track.schema);
            }
            track.time += deltaSeconds;
            Animation::MotionObservation observation;
            observation.time = track.time;
            observation.poseEpoch = player.Pose(0)->discontinuity;
            // 搜索历史读取惯性化后的基础姿态，不包含视觉根偏移与接地反馈。
            observation.root = Animation::HeadingFrame(mesh.model * player.MotionModels().front());
            for (size_t joint : track.joints)
                observation.joints.push_back((mesh.model * player.MotionModels()[joint]).block<3, 1>(0, 3));
            track.history.Record(observation);
            track.achievedVelocity = ScriptVector(actor, "bodyVelocity");
            track.walkVelocity = ScriptVector(actor, "walkVelocity");
            track.grounded = actor.value("grounded", true);
        }
    }
    for (auto it = motionTracks.begin(); it != motionTracks.end();)
        if (!retained.count(it->first)) it = motionTracks.erase(it); else ++it;
}

void World::SetMotionDebugReplayFrame(int frame)
{
    if (frame < 0 || motionDebugHistory.empty())
    {
        motionDebugReplayFrame = -1;
        return;
    }
    motionDebugReplayFrame = std::clamp(frame, 0, static_cast<int>(motionDebugHistory.size()) - 1);
    // 回放只读历史快照；同时冻结被聚焦播放器，避免实时搜索覆盖当前姿态/查询。
    const std::string identity = motionDebugHistory.at(motionDebugReplayFrame).componentIdentity;
    for (auto& entry : meshObjects)
        if (entry.second.sceneObjectIdentity == identity && entry.second.animation)
        {
            entry.second.animation->SetPlaying(false);
            break;
        }
}

std::shared_ptr<const Animation::MotionDebugView> World::CaptureMotionDebug() const
{
    auto view = std::make_shared<Animation::MotionDebugView>();
    view->enabled = motionDebugEnabled;
    if (!motionDebugEnabled) return view;
    if (motionDebugReplayFrame >= 0 && !motionDebugHistory.empty())
    {
        const int frame = std::clamp(motionDebugReplayFrame, 0,
            static_cast<int>(motionDebugHistory.size()) - 1);
        auto replay = std::make_shared<Animation::MotionDebugView>(motionDebugHistory.at(frame));
        replay->replayActive = true;
        replay->replayFrame = frame;
        replay->replayFrames = static_cast<int>(motionDebugHistory.size());
        return replay;
    }
    std::string focus;
    for (const auto& entry : motionTracks)
        if (motionRuntimes.count(entry.first)) { focus = entry.first; break; }
    if (focus.empty() && !motionTracks.empty()) focus = motionTracks.begin()->first;
    for (const auto& entry : motionTracks)
    {
        const auto& track = entry.second;
        Animation::MotionObservation current, previous;
        if (!track.history.Sample(track.time, current)) continue;
        Eigen::Vector3f last = current.root.block<3, 1>(0, 3);
        const Eigen::Vector3f lift(0, 3, 0);
        for (int i = 1; i <= 10; ++i)
        {
            Animation::MotionObservation past;
            if (!track.history.Sample(track.time - i * 0.1, past)) break;
            const Eigen::Vector3f point = past.root.block<3, 1>(0, 3);
            view->lines.push_back({last + lift, point + lift, {0, 0.8f, 1, 1}});
            last = point;
        }
        const auto matchingState = motionRuntimes.find(entry.first);
        if (matchingState != motionRuntimes.end() && matchingState->second.queryReady)
        {
            const auto& matching = matchingState->second;
            last = matching.queryRoot.block<3, 1>(0, 3);
            const Eigen::Vector3f side = matching.queryRoot.block<3, 1>(0, 0) * 8;
            const Eigen::Vector3f ahead = matching.queryRoot.block<3, 1>(0, 2) * 12;
            // 红色轨迹来自最后一次实际搜索；不能用实时意图线解释冻结的查询代价。
            for (const auto& point : matching.queryFutureRoots)
            {
                view->lines.push_back({last + lift, point + lift, {1, 0.2f, 0.25f, 1}});
                view->lines.push_back({point + lift - side, point + lift + side, {1, 0.2f, 0.25f, 1}});
                view->lines.push_back({point + lift, point + lift + ahead, {1, 0.2f, 0.25f, 1}});
                last = point;
            }
        }
        last = current.root.block<3, 1>(0, 3);
        for (int i = 1; i <= 10; ++i)
        {
            const auto future = Animation::PredictCharacterRoot(current.root, track.achievedVelocity,
                track.walkVelocity, track.grounded, i * 0.1f);
            const Eigen::Vector3f point = future.block<3, 1>(0, 3);
            const Eigen::Vector3f side = future.block<3, 1>(0, 0) * 8;
            const Eigen::Vector3f ahead = future.block<3, 1>(0, 2) * 12;
            view->lines.push_back({last + lift, point + lift, {1, 0.65f, 0, 1}});
            view->lines.push_back({point + lift - side, point + lift + side, {1, 0.65f, 0, 1}});
            view->lines.push_back({point + lift, point + lift + ahead, {1, 0.65f, 0, 1}});
            last = point;
        }
        for (const auto& joint : current.joints)
        {
            view->lines.push_back({joint - Eigen::Vector3f(5, 0, 0), joint + Eigen::Vector3f(5, 0, 0), {0.6f, 1, 0.2f, 1}});
            view->lines.push_back({joint - Eigen::Vector3f(0, 5, 0), joint + Eigen::Vector3f(0, 5, 0), {0.6f, 1, 0.2f, 1}});
        }
        // 只有完整历史窗口及相同姿态代际才发布查询，不能把 warm-up 缺样补零伪装有效特征。
        bool ready = track.history.Sample(track.time - track.schema.velocityStep, previous) &&
            previous.poseEpoch == current.poseEpoch && current.poseValid && previous.poseValid;
        std::vector<Eigen::Matrix4f> trajectory;
        for (float offset : track.schema.trajectoryTimes)
        {
            if (offset >= 0)
                trajectory.push_back(Animation::PredictCharacterRoot(current.root, track.achievedVelocity,
                    track.walkVelocity, track.grounded, offset));
            else
            {
                Animation::MotionObservation past;
                if (!track.history.Sample(track.time + offset, past)) ready = false;
                trajectory.push_back(past.root);
            }
        }
        if (entry.first == focus)
        {
            view->summary = "History " + std::to_string(track.history.Size()) +
                " | " + (ready ? "Query ready" : "Warming up") +
                " | Cyan: actual | Amber: live desired (no collision)";
            if (ready)
            {
                std::vector<Eigen::Vector3f> velocities;
                for (size_t i = 0; i < current.joints.size(); ++i)
                    velocities.push_back((current.joints[i] - previous.joints[i]) / track.schema.velocityStep);
                view->channels = Animation::FeatureChannelNames(track.schema);
                view->features = Animation::EncodeMotionFeatures(track.schema, current.root, trajectory, current.joints, velocities);
            }
            const auto runtime = motionRuntimes.find(entry.first);
            if (runtime != motionRuntimes.end())
            {
                const auto& matching = runtime->second;
                view->componentIdentity = entry.first;
                view->databaseIdentity = matching.database->identity;
                view->payloadIdentity = matching.database->payloadIdentity;
                view->queryReady = matching.queryReady;
                view->features = matching.rawQuery;
                view->normalizedQuery = matching.normalizedQuery;
                view->queryRoot = matching.queryRoot;
                view->queryFutureRoots = matching.queryFutureRoots;
                view->queryUsesAchievedVelocity = matching.queryUsesAchievedVelocity;
                view->channels = Animation::FeatureChannelNames(track.schema);
                view->decisionReason = matching.result.reason;
                view->searchCount = matching.searchCount;
                view->allowedFrameCount = matching.result.allowedFrameCount;
                view->policyRejectedCount = matching.result.policyRejectedCount;
                view->selected = matching.result.selected;
                view->continuation = matching.result.continuation;
                view->topCandidates = matching.result.topCandidates;
                for (const auto& mesh : meshObjects)
                    if (mesh.second.sceneObjectIdentity == entry.first && mesh.second.animation)
                    {
                        view->blendAlpha = mesh.second.animation->BlendAlpha();
                        const auto& player = *mesh.second.animation;
                        // Motion 面板的时间轴从当前播放范围起点开始；播放器内部仍用
                        // clip-local 绝对时间驱动 notify/warp 与 section 链。
                        view->playbackTime = player.PlaybackPosition();
                        view->playbackDuration = player.Duration();
                        view->playbackPlaying = player.IsPlaying();
                        const auto& clips = player.Asset().clips;
                        view->playbackClipId = clips.empty() ? "" : clips.at(player.SelectedClip()).id;
                        break;
                    }
                std::ostringstream summary;
                summary << "MM " << matching.database->identity.substr(0, 12) << " | "
                    << (matching.queryReady ? "Query ready" : "Warming up") << " | "
                    << matching.result.reason << " | searches " << matching.searchCount
                    << " | allowed frames " << matching.result.allowedFrameCount
                    << "\nCyan: actual | Amber: live desired (no collision) | Red: frozen MM query"
                    << (matching.queryUsesAchievedVelocity ? " [blocked: actual velocity]" : "");
                if (matching.result.selected)
                    summary << "\nSelected " << matching.result.selected->clipId << " @ "
                        << std::fixed << std::setprecision(3) << matching.result.selected->time
                        << " | blend " << view->blendAlpha;
                for (const auto& top : matching.result.topCandidates)
                    summary << "\n" << top.clipId << " @" << std::fixed << std::setprecision(3) << top.time
                        << " | T " << top.trajectoryCost << " P " << top.poseCost
                        << " policy " << top.policyCost << " = " << top.totalCost
                        << (top.continuing ? " [continue]" : "") << (top.eligible ? "" : " [rejected:" + top.rejection + "]");
                view->summary = summary.str();
            }
        }
    }
    if (view->summary.empty()) view->summary = "No active animated character history";
    if (!view->componentIdentity.empty())
    {
        if (!motionDebugHistory.empty() &&
            motionDebugHistory.back().componentIdentity != view->componentIdentity)
        {
            motionDebugHistory.clear();
            motionDebugReplayFrame = -1;
        }
        const bool duplicate = !motionDebugHistory.empty() &&
            motionDebugHistory.back().searchCount == view->searchCount &&
            motionDebugHistory.back().playbackTime == view->playbackTime &&
            motionDebugHistory.back().decisionReason == view->decisionReason &&
            motionDebugHistory.back().queryReady == view->queryReady;
        if (!duplicate)
        {
            motionDebugHistory.push_back(*view);
            constexpr size_t historyLimit = 600;
            while (motionDebugHistory.size() > historyLimit) motionDebugHistory.pop_front();
        }
        view->replayFrames = static_cast<int>(motionDebugHistory.size());
        view->replayFrame = view->replayFrames - 1;
    }
    return view;
}

std::shared_ptr<const Debug::Snapshot> World::CaptureDebugDraw(const Animation::MotionDebugView& motion)
{
    debugDraw.ClearPublisher("Motion", "Engine.Motion");
    Debug::Style style; style.category = "Motion"; style.owner = "Motion";
    style.publisher = "Engine.Motion"; style.thickness = 2.5f;
    try
    {
        if (motion.enabled)
            for (const auto& line : motion.lines)
            {
                style.color = line.color;
                debugDraw.DrawDebugLine(line.begin, line.end, style);
            }
        // 足部修正独立于运动轨迹开关：FootPlacement 生效时也可见红→绿纠正线。
        std::unordered_set<const Animation::SkeletalPlayback*> shown;
        for (const auto& entry : meshObjects)
        {
            const auto& player = entry.second.animation;
            if (!player || !shown.insert(player.get()).second) continue;
            const auto& placement = player->FootPlacement();
            if (!placement.active) continue;
            // 与求解同帧的世界空间值：红为 pre-IK，绿为最终足底近似点，黄为锁定锚点。
            for (const auto& foot : placement.feet)
            {
                if (foot.probeDisplacement.squaredNorm() == 0) continue;
                style.thickness = 3.0f;
                style.color = {1, 0.15f, 0.1f, 1};
                debugDraw.DrawDebugPoint(foot.preSole, 7, style);
                style.color = {0.2f, 1, 0.2f, 1};
                debugDraw.DrawDebugPoint(foot.postSole, 7, style);
                DrawFootCorrectionGradient(debugDraw, foot.preSole, foot.postSole, style);
                style.color = {0.3f, 0.8f, 1, 1};
                if (foot.probeDisplacement.squaredNorm() > 0)
                    debugDraw.DrawDebugLine(foot.probeOrigin, foot.probeOrigin + foot.probeDisplacement, style);
                if (foot.grounded)
                    debugDraw.DrawDebugLine(foot.groundPoint, foot.groundPoint + foot.groundNormal * 15, style);
                debugDraw.DrawDebugLine(foot.heelProbeOrigin,
                    foot.heelProbeOrigin + foot.heelProbeDisplacement, style);
                if (foot.heelGrounded)
                    debugDraw.DrawDebugLine(foot.heelGroundPoint,
                        foot.heelGroundPoint + foot.heelGroundNormal * 15, style);
                style.color = {1, 0.15f, 0.1f, 1};
                debugDraw.DrawDebugPoint(foot.preHeel, 5, style);
                style.color = {0.2f, 1, 0.2f, 1};
                debugDraw.DrawDebugPoint(foot.postHeel, 5, style);
                DrawFootCorrectionGradient(debugDraw, foot.preHeel, foot.postHeel, style);
                debugDraw.DrawDebugLine(foot.postHeel, foot.postSole, style);
                if (foot.planted)
                {
                    style.color = {1, 0.85f, 0.1f, 1};
                    debugDraw.DrawDebugSphere(foot.anchor, 2, style, 12);
                }
            }
        }
    }
    catch (const std::invalid_argument&)
    {
        // 用户图形已耗尽公共预算时丢弃本组辅助轨迹，不能让调试显示中断正式渲染。
        debugDraw.ClearPublisher("Motion", "Engine.Motion");
    }
    return debugDraw.Freeze();
}

bool World::ControlScriptAnimation(uint64_t expectedGeneration, const std::string& identity,
    int action, float value)
{
    if (expectedGeneration != generation || !std::isfinite(value) ||
        (action != 0 && action != 1) || identity.empty()) return false;
    for (auto& entry : meshObjects)
    {
        if (entry.second.sceneObjectIdentity != identity || !entry.second.animation) continue;
        auto& player = *entry.second.animation;
        if (action == 0)
        {
            if (value < 0 || value > player.Duration()) return false;
            // 显式暂停同时冻结 MM，下一帧搜索不能把 seek 结果覆盖。
            player.SetPlaying(false);
            // UI value 是播放范围内的相对秒数；Seek 仍接收 clip-local 绝对时间。
            // 加法可能在范围末端多出一个 ulp；夹到绝对末端，避免合法的
            // 相对 Duration 在浮点舍入后被 Seek 误判为越界。
            player.Seek(std::min(player.PlaybackRangeStart() + value,
                player.PlaybackRangeEnd()));
        }
        else player.SetPlaying(value != 0);
        return true;
    }
    return false;
}

bool World::ControlAnimation(uint64_t expectedGeneration, const std::string& command, float value)
{
    if (expectedGeneration != generation || !std::isfinite(value)) return false;
    if (command != "play" && command != "seek" && command != "clip") return false;
    // 在改变任何播放器前先验证整批请求，避免不同片长/片段数造成部分应用。
    for (const auto& entry : meshObjects)
    {
        const auto& player = entry.second.animation;
        if (!player) continue;
        if (command == "seek" && (value < 0 || value > player->Duration())) return false;
        if (command == "clip" && (value < 0 || value != std::floor(value) ||
            value >= player->Asset().clips.size())) return false;
    }
    std::unordered_set<Animation::SkeletalPlayback*> updated;
    for (auto& entry : meshObjects)
    {
        auto& player = entry.second.animation;
        if (!player || !updated.insert(player.get()).second) continue;
        if (command == "play") player->SetPlaying(value != 0);
        else if (command == "seek" && value >= 0 && value <= player->Duration())
        {
            player->SetPlaying(false);
            player->Seek(std::min(player->PlaybackRangeStart() + value,
                player->PlaybackRangeEnd()));
        }
        else if (command == "clip" && value >= 0 && value == std::floor(value) &&
            value < player->Asset().clips.size()) player->Select(static_cast<size_t>(value));
        else return false;
    }
    TickAnimations(0);
    return !updated.empty();
}
}
