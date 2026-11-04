#include "hair/tressfxGroupPaint.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace VL::Hair
{
namespace
{

constexpr size_t MaxBrushGuidesPerRequest = 200000;
const std::vector<uint32_t> kEmptyGuideGroupIds;

} // namespace

void GroupPaintController::BeginSession(uint32_t guideCount,
    std::vector<uint32_t> guideGroupIds, std::vector<std::string> groupNames)
{
    guideCount_ = guideCount;
    binding_.reset();
    ownedGuideGroupIds_ = std::make_shared<std::vector<uint32_t>>(
        std::move(guideGroupIds));
    ownedGuideGroupIds_->resize(guideCount, 0u);
    for (uint32_t& groupId : *ownedGuideGroupIds_)
        if (groupId >= groupNames.size() + 1) groupId = 0;
    groupNames_ = std::move(groupNames);
    sessionActive_ = true;
    strokeActive_ = false;
    strokeWeights_.clear();
    undoStack_.clear();
    lastCommit_.reset();
}

void GroupPaintController::BeginSession(
    std::shared_ptr<const TressfxGroupBinding> binding)
{
    if (!binding)
    {
        ClearSession();
        return;
    }
    // 只复制少量组名；guideGroupIds 由预览快照共享，hover/笔画分析全部只读。
    guideCount_ = binding->GuideCount();
    groupNames_ = binding->groupNames;
    binding_ = std::move(binding);
    ownedGuideGroupIds_.reset();
    sessionActive_ = true;
    strokeActive_ = false;
    strokeWeights_.clear();
    undoStack_.clear();
    lastCommit_.reset();
}

void GroupPaintController::ClearSession()
{
    sessionActive_ = false;
    guideCount_ = 0;
    binding_.reset();
    ownedGuideGroupIds_.reset();
    groupNames_.clear();
    strokeActive_ = false;
    strokeWeights_.clear();
    undoStack_.clear();
    lastCommit_.reset();
}

const std::vector<uint32_t>& GroupPaintController::CurrentIds() const
{
    if (ownedGuideGroupIds_) return *ownedGuideGroupIds_;
    if (binding_) return binding_->guideGroupIds;
    return kEmptyGuideGroupIds;
}

std::vector<uint32_t>& GroupPaintController::WritableIds()
{
    if (!ownedGuideGroupIds_)
        ownedGuideGroupIds_ = std::make_shared<std::vector<uint32_t>>(CurrentIds());
    return *ownedGuideGroupIds_;
}

void GroupPaintController::BeginStroke(
    uint32_t targetGroupId, GroupPaintMode mode, bool replaceExisting)
{
    if (!sessionActive_ || targetGroupId == 0 ||
        targetGroupId > groupNames_.size()) return;
    targetGroupId_ = targetGroupId;
    mode_ = mode;
    replaceExisting_ = replaceExisting;
    strokeWeights_.clear();
    strokeActive_ = true;
}

void GroupPaintController::AddHits(const std::vector<RootHit>& hits)
{
    if (!strokeActive_) return;
    for (const RootHit& hit : hits)
    {
        if (hit.guideIndex >= guideCount_ || !std::isfinite(hit.weight) ||
            hit.weight <= 0.0f)
            continue;
        float& weight = strokeWeights_[hit.guideIndex];
        weight = std::max(weight, std::min(1.0f, hit.weight));
    }
}

void GroupPaintController::CancelStroke()
{
    strokeActive_ = false;
    strokeWeights_.clear();
}

std::string GroupPaintController::GroupName(uint32_t groupId) const
{
    if (groupId == 0) return "Default";
    if (groupId > groupNames_.size()) return {};
    return groupNames_[groupId - 1];
}

uint32_t GroupPaintController::CountGuidesInGroup(uint32_t groupId) const
{
    const std::vector<uint32_t>& ids = CurrentIds();
    return static_cast<uint32_t>(std::count(
        ids.begin(), ids.end(), groupId));
}

GroupPaintSummary GroupPaintController::SummarizeHits(
    const std::vector<RootHit>& hits, uint32_t targetGroupId,
    GroupPaintMode mode, bool replaceExisting) const
{
    GroupPaintSummary summary;
    std::set<uint32_t> unique;
    for (const RootHit& hit : hits)
        if (hit.guideIndex < guideCount_ && std::isfinite(hit.weight) &&
            hit.weight > 0.0f)
            unique.insert(hit.guideIndex);
    summary.selectedGuides = static_cast<uint32_t>(unique.size());
    if (targetGroupId == 0 || targetGroupId > groupNames_.size()) return summary;

    const std::vector<uint32_t>& ids = CurrentIds();
    std::map<uint32_t, uint32_t> conflicts;
    for (uint32_t guide : unique)
    {
        const uint32_t current = ids[guide];
        if (mode == GroupPaintMode::Remove)
        {
            if (current == targetGroupId) ++summary.eligibleGuides;
            continue;
        }
        if (current == targetGroupId || current == 0 || replaceExisting)
            ++summary.eligibleGuides;
        else
            ++conflicts[current];
    }
    for (const auto& [groupId, count] : conflicts)
        summary.conflicts.push_back({groupId, GroupName(groupId), count});
    return summary;
}

GroupPaintSummary GroupPaintController::AnalyzeHits(
    const std::vector<RootHit>& hits, uint32_t targetGroupId,
    GroupPaintMode mode, bool replaceExisting) const
{
    return SummarizeHits(hits, targetGroupId, mode, replaceExisting);
}

std::optional<GroupPaintCommit> GroupPaintController::CommitStroke()
{
    if (!strokeActive_ || !sessionActive_ || strokeWeights_.empty())
    {
        strokeActive_ = false;
        strokeWeights_.clear();
        return std::nullopt;
    }

    std::vector<RootHit> hits;
    hits.reserve(strokeWeights_.size());
    for (const auto& [guide, weight] : strokeWeights_)
        hits.push_back({guide, weight});
    GroupPaintSummary summary = SummarizeHits(
        hits, targetGroupId_, mode_, replaceExisting_);

    GroupPaintCommit commit;
    commit.valid = true;
    commit.mode = mode_;
    commit.targetGroupId = targetGroupId_;
    commit.conflicts = summary.conflicts;
    // 只有提交时才复制整份映射；hover/AnalyzeHits 仍共享 binding。
    const std::vector<uint32_t> before = CurrentIds();
    std::vector<uint32_t>& ids = WritableIds();

    if (mode_ == GroupPaintMode::Add)
    {
        for (const RootHit& hit : hits)
        {
            const uint32_t guide = hit.guideIndex;
            const uint32_t current = ids[guide];
            if (current == targetGroupId_ || current == 0 || replaceExisting_)
            {
                ids[guide] = targetGroupId_;
                commit.addGuides.push_back(guide);
            }
        }
    }
    else
    {
        for (const RootHit& hit : hits)
        {
            const uint32_t guide = hit.guideIndex;
            if (ids[guide] == targetGroupId_)
            {
                ids[guide] = 0;
                commit.removeGuides.push_back(guide);
            }
        }
    }
    std::sort(commit.addGuides.begin(), commit.addGuides.end());
    std::sort(commit.removeGuides.begin(), commit.removeGuides.end());
    commit.addRanges = CompressTressfxGuides(commit.addGuides);
    commit.removeRanges = CompressTressfxGuides(commit.removeGuides);

    // 提交后变空的组必须删除；Default 0 不是可删除的命名组。
    for (uint32_t groupId = 1; groupId <= groupNames_.size(); ++groupId)
    {
        if (groupId == targetGroupId_) continue;
        if (CountGuidesInGroup(groupId) == 0 &&
            std::count(before.begin(), before.end(), groupId) != 0)
            commit.emptiedGroupNames.push_back(GroupName(groupId));
    }
    if (mode_ == GroupPaintMode::Remove && CountGuidesInGroup(targetGroupId_) == 0)
        commit.emptiedGroupNames.push_back(GroupName(targetGroupId_));

    // 控制器自带 undo 只服务纯逻辑测试/调试；生产 UI 用 Groups 草稿快照。
    // 40k guide 每份约 160 KB，限制份数避免长时间会话无界增长。
    if (undoStack_.size() >= 16) undoStack_.erase(undoStack_.begin());
    undoStack_.push_back(before);
    lastCommit_ = commit;
    strokeActive_ = false;
    strokeWeights_.clear();
    return commit;
}

bool GroupPaintController::UndoLastStroke()
{
    if (undoStack_.empty()) return false;
    ownedGuideGroupIds_ = std::make_shared<std::vector<uint32_t>>(
        std::move(undoStack_.back()));
    undoStack_.pop_back();
    lastCommit_.reset();
    strokeActive_ = false;
    strokeWeights_.clear();
    return true;
}

bool ValidateTressfxBrushPreviewRequest(
    const TressfxBrushPreviewRequest& request, std::string& error)
{
    error.clear();
    // 圆环清除命令只针对 World 的 debug draw，不涉及具体渲染对象；
    // 因此 objectName 只在需要写 ghost 或绘制圆环时必填。
    if (request.objectName.empty() &&
        (!request.weights.empty() || request.ringVisible))
    {
        error = "TressFX brush preview requires a render object name";
        return false;
    }
    if (request.worldGeneration == 0)
    {
        error = "TressFX brush preview requires a World generation";
        return false;
    }
    if (request.weights.size() > MaxBrushGuidesPerRequest)
    {
        error = "TressFX brush preview guide list exceeds the request budget";
        return false;
    }
    std::set<uint32_t> seen;
    for (const TressfxBrushGuideWeight& weight : request.weights)
    {
        if (!std::isfinite(weight.weight) || weight.weight < 0.0f ||
            weight.weight > 1.0f)
        {
            error = "TressFX brush preview weight must be finite and within [0,1]";
            return false;
        }
        if (!seen.insert(weight.guideIndex).second)
        {
            error = "TressFX brush preview contains a duplicate guide";
            return false;
        }
    }
    if (request.ringVisible)
    {
        if (!request.ringCenter.allFinite() || !request.ringNormal.allFinite() ||
            !std::isfinite(request.ringRadiusWorld) || request.ringRadiusWorld <= 0.0f ||
            request.ringRadiusWorld > 1.0e6f)
        {
            error = "TressFX brush ring requires a finite center, normal and radius in (0, 1e6]";
            return false;
        }
        if (request.ringNormal.norm() <= 1.0e-6f)
        {
            error = "TressFX brush ring normal must be non-zero";
            return false;
        }
    }
    return true;
}

} // namespace VL::Hair
