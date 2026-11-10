#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "hair/tressfxGroupRanges.h"
#include "hair/tressfxGroupBinding.h"
#include "hair/tressfxSurfaceBrush.h"

namespace VL::Hair
{

enum class GroupPaintMode : uint32_t
{
    Add = 0,
    Remove = 1
};

// GPU ghost 只消费这份值语义；host buffer 由 RenderableObject 拥有。
struct TressfxBrushGuideWeight
{
    uint32_t guideIndex = 0;
    float weight = 0.0f;
};

struct TressfxBrushPreviewRequest
{
    std::string objectName;
    uint64_t worldGeneration = 0;
    uint32_t targetGroupId = 0;
    GroupPaintMode mode = GroupPaintMode::Add;
    // 空列表表示清除 ghost；非空列表是一次笔画从开始到现在的完整权重并集。
    std::vector<TressfxBrushGuideWeight> weights;
    // 世界空间笔刷圆环：与 ghost 同一条值语义命令进入 GT/World debug draw。
    // ringVisible=false 表示移除上一帧的固定 key；不依赖 ghost 权重是否非空。
    bool ringVisible = false;
    Eigen::Vector3f ringCenter = Eigen::Vector3f::Zero();
    Eigen::Vector3f ringNormal = Eigen::Vector3f::UnitY();
    float ringRadiusWorld = 0.0f;
};

struct GroupPaintConflict
{
    uint32_t groupId = 0;
    std::string groupName;
    uint32_t guideCount = 0;
};

struct GroupPaintSummary
{
    uint32_t selectedGuides = 0;
    uint32_t eligibleGuides = 0;
    std::vector<GroupPaintConflict> conflicts;
};

struct GroupPaintCommit
{
    bool valid = false;
    GroupPaintMode mode = GroupPaintMode::Add;
    uint32_t targetGroupId = 0;
    std::vector<uint32_t> addGuides;
    std::vector<uint32_t> removeGuides;
    std::vector<TressfxGuideRange> addRanges;
    std::vector<TressfxGuideRange> removeRanges;
    std::vector<GroupPaintConflict> conflicts;
    std::vector<std::string> emptiedGroupNames;
};

// 编辑器分组笔刷的纯业务状态：目标组、Add/Remove、冲突与本次会话撤销。
// 不接触 RmlUi/World/Vulkan；实际资产写回仍由编辑器草稿完成。
class GroupPaintController
{
public:
    // 值语义入口：拷贝一份可写映射，供测试或独立快照使用。
    void BeginSession(uint32_t guideCount, std::vector<uint32_t> guideGroupIds,
        std::vector<std::string> groupNames);
    // 预览会话入口：直接共享已保存资产的 binding，不复制 40k guide 映射。
    // 第一次 CommitStroke/Undo 才做 copy-on-write，hover 的 AnalyzeHits 始终只读。
    void BeginSession(std::shared_ptr<const TressfxGroupBinding> binding);
    void ClearSession();
    bool HasSession() const { return sessionActive_; }
    uint32_t GuideCount() const { return guideCount_; }
    const std::vector<uint32_t>& GuideGroupIds() const { return CurrentIds(); }

    void BeginStroke(uint32_t targetGroupId, GroupPaintMode mode, bool replaceExisting);
    void AddHits(const std::vector<RootHit>& hits);
    void CancelStroke();
    bool IsStrokeActive() const { return strokeActive_; }
    const std::map<uint32_t, float>& StrokeWeights() const { return strokeWeights_; }
    uint32_t TargetGroupId() const { return targetGroupId_; }
    GroupPaintMode Mode() const { return mode_; }
    bool ReplaceExisting() const { return replaceExisting_; }

    // 只分析当前 hits，不改草稿；UI hover 的计数/冲突提示使用它。
    GroupPaintSummary AnalyzeHits(const std::vector<RootHit>& hits,
        uint32_t targetGroupId, GroupPaintMode mode, bool replaceExisting) const;

    // 提交本笔：Add 会跳过其他组（replaceExisting=false）并报告冲突；
    // Remove 只移除目标组内的 guide。空组由调用方根据 emptiedGroupNames 删除。
    std::optional<GroupPaintCommit> CommitStroke();
    bool UndoLastStroke();
    bool CanUndo() const { return !undoStack_.empty(); }
    const std::optional<GroupPaintCommit>& LastCommit() const { return lastCommit_; }

private:
    const std::vector<uint32_t>& CurrentIds() const;
    std::vector<uint32_t>& WritableIds();
    GroupPaintSummary SummarizeHits(const std::vector<RootHit>& hits,
        uint32_t targetGroupId, GroupPaintMode mode, bool replaceExisting) const;
    std::string GroupName(uint32_t groupId) const;
    uint32_t CountGuidesInGroup(uint32_t groupId) const;

    bool sessionActive_ = false;
    uint32_t guideCount_ = 0;
    // binding_ 只读共享；ownedGuideGroupIds_ 仅在本次会话第一次写入时创建。
    std::shared_ptr<const TressfxGroupBinding> binding_;
    std::shared_ptr<std::vector<uint32_t>> ownedGuideGroupIds_;
    std::vector<std::string> groupNames_;

    bool strokeActive_ = false;
    uint32_t targetGroupId_ = 0;
    GroupPaintMode mode_ = GroupPaintMode::Add;
    bool replaceExisting_ = false;
    std::map<uint32_t, float> strokeWeights_;
    std::vector<std::vector<uint32_t>> undoStack_;
    std::optional<GroupPaintCommit> lastCommit_;
};

// GT 执行端复用的值校验；渲染线程不做字段解释。
bool ValidateTressfxBrushPreviewRequest(
    const TressfxBrushPreviewRequest& request, std::string& error);

} // namespace VL::Hair
