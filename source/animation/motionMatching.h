#pragma once

#include "motionDatabase.h"
#include <optional>

namespace VL::Animation
{
// GT 从已解析数据库创建冻结搜索资源；预归一化只做一次，不持有 World、播放器或游戏状态。
class MotionSearchDatabase
{
public:
    const MotionDatabase& Data() const { return database; }
    const std::vector<std::vector<float>>& NormalizedFrames() const { return normalizedFrames; }
    const std::vector<float>& Weights() const { return weights; }

private:
    friend std::shared_ptr<const MotionSearchDatabase> FreezeMotionDatabase(MotionDatabase database);
    MotionDatabase database;
    std::vector<std::vector<float>> normalizedFrames;
    std::vector<float> weights;
};

std::shared_ptr<const MotionSearchDatabase> FreezeMotionDatabase(MotionDatabase database);

// 查询必须通过 BindMotionQuery 建立；共享所有权把 schema、统计和归一化帧绑定在同一代资源上。
struct MotionQuery
{
    std::shared_ptr<const MotionSearchDatabase> database;
    std::vector<float> normalized;
};

MotionQuery BindMotionQuery(std::shared_ptr<const MotionSearchDatabase> database,
    const FeatureSchema& schema, const std::vector<float>& rawFeatures);

struct MotionMatchingPolicy
{
    double searchInterval = 0.1;
    double continuationBias = 0.05;
    double minimumImprovement = 0.05;
    double reselectionWindow = 0.15;
    double sameClipMinimumJump = 0.25;
    size_t topK = 5;
    bool actionLocked = false;
};

// 播放时间来自原生播放器。可附准确时刻的 raw 特征；留空时用最近库帧并记录量化误差。
// duration 用于真实完整窗口/循环边界判断，不能把非循环尾端无限钳到最后一帧。
struct MotionContinuation
{
    std::string clipId;
    float time = 0;
    float duration = 0;
    std::vector<float> rawFeatures;
};

// 只读值结果供播放器和冻结调试快照消费；分项之和为 distanceCost，policyCost 为继续偏置。
struct MotionCandidate
{
    size_t memberIndex = 0;
    size_t frameIndex = 0;
    std::string clipId;
    float time = 0;
    double trajectoryCost = 0;
    double poseCost = 0;
    double distanceCost = 0;
    double policyCost = 0;
    double totalCost = 0;
    bool continuing = false;
    bool eligible = true;
    std::string rejection;
    double quantizationSeconds = 0;
};

struct MotionSearchResult
{
    bool searched = false;
    bool switched = false;
    std::string reason = "not-searched";
    std::optional<MotionCandidate> selected;
    std::optional<MotionCandidate> continuation;
    std::vector<MotionCandidate> topCandidates;
    size_t allowedFrameCount = 0;
    size_t policyRejectedCount = 0;
};

// 精确全表搜索。allowedClipIds 是 C# 筛选结果；空集合始终表示无候选，不扩展业务允许范围。
MotionSearchResult SearchMotionDatabase(const MotionQuery& query,
    const std::vector<std::string>& allowedClipIds,
    const std::optional<MotionContinuation>& continuation = std::nullopt,
    const MotionMatchingPolicy& policy = {}, double selectionAge = 0);

// 只维护搜索节流和重选年龄，播放时钟与业务状态机属于调用方。
// deltaSeconds 必须为已缩放的非负时间；0 不搜索、不改变内部状态，Reset 用于 Stop/传送/身份变化。
class MotionMatcher
{
public:
    MotionSearchResult Update(const MotionQuery& query, const std::vector<std::string>& allowedClipIds,
        const std::optional<MotionContinuation>& continuation, double deltaSeconds,
        const MotionMatchingPolicy& policy = {});
    void Reset();

private:
    std::shared_ptr<const MotionSearchDatabase> database;
    std::vector<std::string> allowed;
    double searchAge = 0;
    double selectionAge = 0;
    bool initialized = false;
    MotionSearchResult lastResult;
};
}
