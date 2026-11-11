#include "editor/terrain/terrainEditSession.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "commonFunction.h"
#include "terrain/terrainHeightmapWriter.h"

namespace VL
{
namespace Editor
{
namespace
{

constexpr size_t TerrainUndoLimit = 8;
constexpr uint32_t RaycastMaxSteps = 2048;
constexpr float RaycastEpsilon = 1.0e-5f;

bool IsFinite(const Eigen::Vector3f& value)
{
    return std::isfinite(value.x()) &&
        std::isfinite(value.y()) &&
        std::isfinite(value.z());
}

Eigen::Matrix4f BuildModelMatrix(const TerrainCollisionPlacement& placement)
{
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    const Eigen::Matrix3f rotation =
        CommonFunction::RotationToMatrix(placement.rotation).block<3, 3>(0, 0);
    model.block<3, 3>(0, 0) =
        rotation * placement.scale.asDiagonal();
    model.block<3, 1>(0, 3) = placement.position;
    return model;
}

bool IntersectAabb(
    const Eigen::Vector3f& origin,
    const Eigen::Vector3f& direction,
    const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax,
    float& tMin,
    float& tMax)
{
    tMin = 0.0f;
    tMax = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 3; ++axis)
    {
        const float axisOrigin = origin[axis];
        const float axisDirection = direction[axis];
        if (std::abs(axisDirection) < 1.0e-8f)
        {
            if (axisOrigin < boundsMin[axis] || axisOrigin > boundsMax[axis])
                return false;
            continue;
        }
        float first = (boundsMin[axis] - axisOrigin) / axisDirection;
        float second = (boundsMax[axis] - axisOrigin) / axisDirection;
        if (first > second)
            std::swap(first, second);
        tMin = std::max(tMin, first);
        tMax = std::min(tMax, second);
        if (tMin > tMax)
            return false;
    }
    return tMax >= 0.0f;
}

bool EvaluateHeightDifference(
    const TerrainHeightmapEditor& editor,
    const TerrainAssetDesc& desc,
    const Eigen::Vector3f& localOrigin,
    const Eigen::Vector3f& localDirection,
    float distance,
    float& difference,
    Eigen::Vector2f& uv)
{
    const Eigen::Vector3f point = localOrigin + localDirection * distance;
    if (!IsFinite(point))
        return false;
    const float u = point.x() / desc.size.x() + 0.5f;
    const float v = point.z() / desc.size.y() + 0.5f;
    if (u < -RaycastEpsilon || u > 1.0f + RaycastEpsilon ||
        v < -RaycastEpsilon || v > 1.0f + RaycastEpsilon)
    {
        return false;
    }
    uv = Eigen::Vector2f(
        std::clamp(u, 0.0f, 1.0f),
        std::clamp(v, 0.0f, 1.0f));
    difference = point.y() - editor.SampleHeightCm(uv.x(), uv.y());
    return std::isfinite(difference);
}

bool ParsePlacementSceneName(
    const std::string& placementId,
    std::string& sceneName)
{
    static const std::string prefix = "terrain|";
    if (placementId.compare(0, prefix.size(), prefix) != 0)
        return false;
    if (placementId.size() <= prefix.size())
        return false;
    sceneName = placementId.substr(prefix.size());
    return true;
}

} // namespace

bool TerrainEditSession::Create(
    const TerrainCollisionInstance& instance,
    TerrainEditSession& out,
    std::string& error)
{
    error.clear();
    std::string sceneName;
    if (!ParsePlacementSceneName(instance.placementId, sceneName))
    {
        error = "Terrain edit session requires a terrain|<name> placement id";
        return false;
    }
    if (!instance.plan.heightmap)
    {
        error = "Terrain edit session requires a decoded heightmap";
        return false;
    }
    if (!IsFinite(instance.placement.position) ||
        !IsFinite(instance.placement.rotation) ||
        !IsFinite(instance.placement.scale) ||
        instance.placement.scale.x() <= 0.0f ||
        instance.placement.scale.y() <= 0.0f ||
        instance.placement.scale.z() <= 0.0f)
    {
        error = "Terrain edit session requires finite positive placement transform";
        return false;
    }
    if (!std::isfinite(instance.plan.desc.size.x()) ||
        !std::isfinite(instance.plan.desc.size.y()) ||
        instance.plan.desc.size.x() <= 0.0f ||
        instance.plan.desc.size.y() <= 0.0f)
    {
        error = "Terrain edit session requires positive finite terrain size";
        return false;
    }
    if (!std::isfinite(instance.plan.desc.heightScale) ||
        !std::isfinite(instance.plan.desc.heightOffset))
    {
        error = "Terrain edit session requires finite height scale/offset";
        return false;
    }
    if (instance.plan.desc.lodLevels > TerrainMaxLodLevels)
    {
        error = "Terrain edit session lodLevels exceeds the supported maximum";
        return false;
    }

    TerrainBuildPlan plan = instance.plan;
    // 会话的身份以 World/碰撞实例为准；同一 TR 多次摆放不能靠 assetPath 区分。
    plan.placementId = instance.placementId;
    try
    {
        TerrainEditSession candidate;
        candidate.editor_ = std::make_unique<TerrainHeightmapEditor>(plan);
        candidate.desc_ = plan.desc;
        candidate.placement_ = instance.placement;
        candidate.placementId_ = instance.placementId;
        candidate.actorIdentity_ = instance.placementId;
        candidate.heightBoundsDirty_ = true;
        out = std::move(candidate);
    }
    catch (const std::exception& exception)
    {
        error = std::string("Terrain edit session creation failed: ") +
            exception.what();
        return false;
    }
    return true;
}

uint64_t TerrainEditSession::Revision() const noexcept
{
    return editor_ ? editor_->Revision() : 0;
}

bool TerrainEditSession::BeginStroke()
{
    if (!editor_ || strokeActive_)
        return false;
    strokeStartSamples_ = editor_->CaptureHeightSamples();
    strokeChanged_ = false;
    strokeActive_ = true;
    return true;
}

bool TerrainEditSession::ApplyBrush(const TerrainSculptBrush& brush)
{
    if (!editor_ || !strokeActive_)
        return false;
    const TerrainSculptResult result = editor_->ApplyBrush(brush);
    if (!result.applied)
        return false;
    strokeChanged_ = true;
    heightBoundsDirty_ = true;
    return true;
}

bool TerrainEditSession::EndStroke()
{
    if (!editor_ || !strokeActive_)
        return false;
    strokeActive_ = false;
    if (!strokeChanged_)
    {
        strokeStartSamples_.clear();
        return false;
    }
    // Raise 后再 Lower 回到原值不应消耗一条 undo 槽位。
    if (editor_->NormalizedSamples() == strokeStartSamples_)
    {
        strokeStartSamples_.clear();
        strokeChanged_ = false;
        return false;
    }

    undoStack_.push_back(std::move(strokeStartSamples_));
    strokeStartSamples_.clear();
    while (undoStack_.size() > TerrainUndoLimit)
        undoStack_.pop_front();
    redoStack_.clear();
    strokeChanged_ = false;
    return true;
}

void TerrainEditSession::CancelStroke()
{
    if (!editor_ || !strokeActive_)
        return;
    strokeActive_ = false;
    if (strokeChanged_)
    {
        try
        {
            editor_->RestoreHeightSamples(std::move(strokeStartSamples_));
            MarkAllDirty();
            heightBoundsDirty_ = true;
        }
        catch (const std::exception&)
        {
            // 恢复失败意味着会话已处于不一致状态；保留当前样本，交给上层
            // 重建会话，而不是静默继续使用一份混合数据。
        }
    }
    strokeStartSamples_.clear();
    strokeChanged_ = false;
}

bool TerrainEditSession::CanUndo() const noexcept
{
    return editor_ && !strokeActive_ && !undoStack_.empty();
}

bool TerrainEditSession::CanRedo() const noexcept
{
    return editor_ && !strokeActive_ && !redoStack_.empty();
}

bool TerrainEditSession::Undo()
{
    if (!CanUndo())
        return false;
    std::vector<float> previous = std::move(undoStack_.back());
    undoStack_.pop_back();
    std::vector<float> current = editor_->CaptureHeightSamples();
    editor_->RestoreHeightSamples(std::move(previous));
    redoStack_.push_back(std::move(current));
    while (redoStack_.size() > TerrainUndoLimit)
        redoStack_.pop_front();
    MarkAllDirty();
    heightBoundsDirty_ = true;
    return true;
}

bool TerrainEditSession::Redo()
{
    if (!CanRedo())
        return false;
    std::vector<float> next = std::move(redoStack_.back());
    redoStack_.pop_back();
    std::vector<float> current = editor_->CaptureHeightSamples();
    editor_->RestoreHeightSamples(std::move(next));
    undoStack_.push_back(std::move(current));
    while (undoStack_.size() > TerrainUndoLimit)
        undoStack_.pop_front();
    MarkAllDirty();
    heightBoundsDirty_ = true;
    return true;
}

void TerrainEditSession::EnsureHeightBounds() const
{
    if (!heightBoundsDirty_ || !editor_)
        return;
    float minimum = std::numeric_limits<float>::infinity();
    float maximum = -std::numeric_limits<float>::infinity();
    for (float sample : editor_->NormalizedSamples())
    {
        const float height =
            sample * desc_.heightScale + desc_.heightOffset;
        minimum = std::min(minimum, height);
        maximum = std::max(maximum, height);
    }
    heightMinCm_ = minimum;
    heightMaxCm_ = maximum;
    heightBoundsDirty_ = false;
}

TerrainRaycastHit TerrainEditSession::Raycast(
    const Eigen::Vector3f& origin,
    const Eigen::Vector3f& worldDirection) const
{
    TerrainRaycastHit hit;
    if (!editor_ || !IsFinite(origin) || !IsFinite(worldDirection))
        return hit;
    const float directionLength = worldDirection.norm();
    if (!std::isfinite(directionLength) || directionLength < 1.0e-6f)
        return hit;
    const Eigen::Vector3f worldUnitDirection =
        worldDirection / directionLength;

    const Eigen::Matrix4f model = BuildModelMatrix(placement_);
    const float determinant = model.determinant();
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-8f)
        return hit;
    const Eigen::Matrix4f inverse = model.inverse();
    const Eigen::Vector3f localOrigin =
        (inverse * origin.homogeneous()).head<3>();
    const Eigen::Vector3f localDirection =
        inverse.block<3, 3>(0, 0) * worldUnitDirection;
    if (!IsFinite(localOrigin) || !IsFinite(localDirection))
        return hit;

    EnsureHeightBounds();
    if (!std::isfinite(heightMinCm_) || !std::isfinite(heightMaxCm_))
        return hit;
    const Eigen::Vector3f boundsMin(
        -desc_.size.x() * 0.5f,
        heightMinCm_,
        -desc_.size.y() * 0.5f);
    const Eigen::Vector3f boundsMax(
        desc_.size.x() * 0.5f,
        heightMaxCm_,
        desc_.size.y() * 0.5f);
    float tMin = 0.0f;
    float tMax = 0.0f;
    if (!IntersectAabb(
            localOrigin,
            localDirection,
            boundsMin,
            boundsMax,
            tMin,
            tMax))
    {
        return hit;
    }

    float previousDifference = 0.0f;
    Eigen::Vector2f uv = Eigen::Vector2f::Zero();
    if (!EvaluateHeightDifference(
            *editor_,
            desc_,
            localOrigin,
            localDirection,
            tMin,
            previousDifference,
            uv))
    {
        return hit;
    }

    float hitDistance = tMin;
    bool found = previousDifference <= 0.0f;
    if (!found)
    {
        const float minimumCell = std::min(
            desc_.size.x() / static_cast<float>(desc_.gridResolution),
            desc_.size.y() / static_cast<float>(desc_.gridResolution));
        const float minimumScale = std::min(
            placement_.scale.x(),
            std::min(placement_.scale.y(), placement_.scale.z()));
        float step = minimumCell * minimumScale * 0.5f;
        const float range = tMax - tMin;
        if (range > 0.0f)
        {
            step = std::max(
                step,
                range / static_cast<float>(RaycastMaxSteps));
        }
        if (!std::isfinite(step) || step <= 0.0f)
            step = range > 0.0f ? range : 1.0f;

        float previousT = tMin;
        float currentT = tMin;
        for (uint32_t stepIndex = 0;
             stepIndex < RaycastMaxSteps && currentT < tMax;
             ++stepIndex)
        {
            currentT = std::min(previousT + step, tMax);
            float currentDifference = 0.0f;
            Eigen::Vector2f currentUv = Eigen::Vector2f::Zero();
            if (!EvaluateHeightDifference(
                    *editor_,
                    desc_,
                    localOrigin,
                    localDirection,
                    currentT,
                    currentDifference,
                    currentUv))
            {
                return hit;
            }
            if (currentDifference <= 0.0f)
            {
                float low = previousT;
                float high = currentT;
                for (uint32_t iteration = 0; iteration < 24; ++iteration)
                {
                    const float middle = (low + high) * 0.5f;
                    float middleDifference = 0.0f;
                    Eigen::Vector2f middleUv = Eigen::Vector2f::Zero();
                    if (!EvaluateHeightDifference(
                            *editor_,
                            desc_,
                            localOrigin,
                            localDirection,
                            middle,
                            middleDifference,
                            middleUv))
                    {
                        break;
                    }
                    if (middleDifference > 0.0f)
                        low = middle;
                    else
                        high = middle;
                }
                hitDistance = high;
                found = true;
                break;
            }
            previousDifference = currentDifference;
            previousT = currentT;
            if (currentT >= tMax)
                break;
        }
    }

    if (!found)
        return hit;
    if (!EvaluateHeightDifference(
            *editor_,
            desc_,
            localOrigin,
            localDirection,
            hitDistance,
            previousDifference,
            uv))
    {
        return hit;
    }

    hit.valid = true;
    hit.uv = uv;
    hit.distanceCm = hitDistance;
    hit.world = origin + worldUnitDirection * hitDistance;
    return hit;
}

Eigen::Vector3f TerrainEditSession::WorldPoint(float u, float v) const
{
    if (!editor_)
        throw std::runtime_error("Terrain edit session is not initialized");
    if (!std::isfinite(u) || !std::isfinite(v))
        throw std::invalid_argument("Terrain world point UV must be finite");
    const float clampedU = std::clamp(u, 0.0f, 1.0f);
    const float clampedV = std::clamp(v, 0.0f, 1.0f);
    const Eigen::Vector3f local(
        (clampedU - 0.5f) * desc_.size.x(),
        editor_->SampleHeightCm(clampedU, clampedV),
        (clampedV - 0.5f) * desc_.size.y());
    return (BuildModelMatrix(placement_) * local.homogeneous()).head<3>();
}

TerrainMeshUpdate TerrainEditSession::BuildUpdate() const
{
    TerrainMeshUpdate update;
    if (!editor_)
        return update;
    update.placementId = placementId_;
    update.actorIdentity = actorIdentity_;
    update.assetPath = desc_.assetPath;
    update.desc = desc_;
    update.heightmap = std::make_shared<const HostImage>(
        editor_->ExportNormalizedHeightmap());

    std::string sceneName;
    if (!ParsePlacementSceneName(placementId_, sceneName))
        return update;

    TerrainBuildPlan editedPlan;
    editedPlan.desc = desc_;
    editedPlan.placementId = placementId_;
    editedPlan.heightmap = update.heightmap;

    const uint32_t lodLevelCount =
        desc_.lodLevels == 0 ? 1u : desc_.lodLevels;
    const bool multiLod = lodLevelCount > 1;
    for (uint32_t lodLevel = 0; lodLevel < lodLevelCount; ++lodLevel)
    {
        const std::vector<TerrainMeshChunk> meshes =
            editor_->BuildDirtyChunkMeshes(lodLevel);
        for (const TerrainMeshChunk& mesh : meshes)
        {
            TerrainChunkMeshUpdate chunk;
            chunk.chunkX = mesh.chunkX;
            chunk.chunkZ = mesh.chunkZ;
            chunk.lodLevel = lodLevel;
            chunk.resourceKey = TerrainChunkResourceKey(
                editedPlan,
                mesh.chunkX,
                mesh.chunkZ,
                lodLevel);
            const std::string chunkBaseName =
                sceneName + "::chunk_" +
                std::to_string(mesh.chunkX) + "_" +
                std::to_string(mesh.chunkZ);
            chunk.objectName = multiLod
                ? chunkBaseName + "::lod" + std::to_string(lodLevel)
                : chunkBaseName;
            chunk.vertices = mesh.vertices;
            chunk.indices = mesh.indices;
            chunk.boundsMin = mesh.boundsMin;
            chunk.boundsMax = mesh.boundsMax;
            update.chunks.push_back(std::move(chunk));
        }
    }
    return update;
}

void TerrainEditSession::ResetDirty()
{
    if (editor_)
        editor_->ResetDirty();
}

bool TerrainEditSession::SaveHeightmap(
    const std::filesystem::path& path,
    std::string& error) const
{
    if (!editor_)
    {
        error = "Terrain edit session is not initialized";
        return false;
    }
    return TerrainHeightmapWriter::Write(
        path, editor_->ExportNormalizedHeightmap(), error);
}

void TerrainEditSession::MarkAllDirty()
{
    if (editor_)
        editor_->MarkAllDirty();
}

} // namespace Editor
} // namespace VL
