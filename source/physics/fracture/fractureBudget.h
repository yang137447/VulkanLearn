#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace VL::Physics::Fracture
{

// 一个预算类由 profile 选择。ownerGroup 让“玻璃动态片 + attached 片”共享
// 单板额度，而不需要 runtime 再写材料分支。
struct FractureBudgetClass
{
    std::string key;
    std::string ownerGroup;
    std::size_t perOwnerLimit = 0;
    // >0 时允许为了视觉下限短暂越过全局上限。
    std::size_t floorCount = 0;
    bool reclaimable = true;
};

struct FractureBudgetRequest
{
    std::string owner;
    std::string budgetClass;
    std::size_t count = 1;
    // attached 请求在预算不足时退化为“整块掉落”。
    bool allowWholePlateDrop = false;
    // 正在被切分/转换的父碎片不能被回收；由调用方传入稳定 shard id。
    std::vector<std::uint64_t> excludeShardIds;
};

struct FractureBudgetCandidate
{
    std::string owner;
    std::string budgetClass;
    std::uint64_t shardId = 0;
    float settledSeconds = 0.0f;
    // 世界空间包围半径与碎片中心到渲染相机的距离，均为厘米。
    float boundingRadius = 0.0f;
    float cameraDistance = 0.0f;
};

struct FractureBudgetDecision
{
    bool granted = false;
    std::size_t grantedCount = 0;
    bool dropWholePlate = false;
    bool exceededLimit = false;
    std::vector<FractureBudgetCandidate> evictions;
};

// 由 EngineLoop/registry 实现：把回收请求按 owner 转发给拥有碎片的运行时。
class FractureEvictionDispatcher
{
public:
    virtual ~FractureEvictionDispatcher() = default;
    virtual void EvictFractureBudgetShards(
        const std::vector<FractureBudgetCandidate>& candidates) = 0;
};

// 全场景碎片预算账本。只做计数、候选排序和取舍，不创建/销毁碎片。
class FractureBudget
{
public:
    static constexpr std::size_t GlobalLimit = 320;
    static constexpr std::size_t GlassPaneLimit = 96;
    static constexpr std::size_t BalloonLimit = 48;
    static constexpr std::size_t MinimumBalloonShards = 4;
    static constexpr float SettledSecondsToReclaim = 1.5f;

    FractureBudget();

    // 可注册新的 profile 预算类；未知类不会被静默记到玻璃账上。
    void RegisterClass(const FractureBudgetClass& budgetClass);
    void Reset();

    void OnShardsCreated(const std::string& owner,
        const std::string& budgetClass, std::size_t count = 1);
    void OnShardsRemoved(const std::string& owner,
        const std::string& budgetClass, std::size_t count = 1);
    void UpdateReclaimable(const FractureBudgetCandidate& candidate);
    void UntrackReclaimable(const std::string& owner, std::uint64_t shardId);

    FractureBudgetDecision Request(const FractureBudgetRequest& request) const;

    std::size_t TotalShards() const { return totalShards; }
    std::size_t OwnerShards(const std::string& owner) const;
    std::size_t ReclaimableShards() const;

private:
    struct Usage
    {
        std::unordered_map<std::string, std::size_t> byClass;
        std::size_t Total() const;
    };
    struct RegisteredClass
    {
        std::string ownerGroup;
        std::size_t perOwnerLimit = 0;
        std::size_t floorCount = 0;
        bool reclaimable = true;
    };

    const RegisteredClass& ClassFor(const std::string& key) const;
    std::size_t OwnerGroupShards(
        const std::string& owner, const std::string& group) const;
    std::vector<FractureBudgetCandidate> ReclaimableByPriority() const;

    std::unordered_map<std::string, RegisteredClass> classes;
    std::unordered_map<std::string, Usage> usageByOwner;
    std::unordered_map<std::string, std::vector<FractureBudgetCandidate>>
        reclaimableByOwner;
    std::size_t totalShards = 0;
};

FractureBudgetDecision AcquireFractureBudget(
    FractureBudget& budget,
    FractureEvictionDispatcher& dispatcher,
    const FractureBudgetRequest& request);

} // namespace VL::Physics::Fracture
