#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace VL::Physics::Fracture
{

// 碎片预算的类别。attached 板片是玻璃“裂而不掉”的静态部分：计入总量，
// 但不进入可回收候选，因此预算压力不能把它从玻璃上摘掉。
enum class FractureBudgetKind
{
    GlassDynamic,
    GlassAttached,
    BalloonShard
};

// 一次新增碎片的预算请求。count 是净新增片数：调用方先扣掉会被替换的父碎片。
struct FractureBudgetRequest
{
    std::string owner;
    FractureBudgetKind kind = FractureBudgetKind::GlassDynamic;
    std::size_t count = 1;
    // attached 请求在预算不足时退化为“整块掉落”，而不是新增碎片。
    bool allowWholePlateDrop = false;
};

// 一个可回收的旧碎片。Runtime 每帧刷新静止时长、相机距离和包围半径。
struct FractureBudgetCandidate
{
    std::string owner;
    std::uint64_t shardId = 0;
    FractureBudgetKind kind = FractureBudgetKind::GlassDynamic;
    float settledSeconds = 0.0f;
    // 世界空间包围半径与碎片中心到渲染相机的距离，均为厘米。
    float boundingRadius = 0.0f;
    float cameraDistance = 0.0f;
};

struct FractureBudgetDecision
{
    bool granted = false;
    // 允许生成的片数；气球请求可能被裁剪到小于 count。
    std::size_t grantedCount = 0;
    // attached 请求未被满足：调用方应走整块掉落路径，不创建新碎片。
    bool dropWholePlate = false;
    // 视觉下限（气球最少软片）被迫越过总上限时置位。
    bool exceededLimit = false;
    // 调用方必须先回收这些碎片，再创建新碎片。
    std::vector<FractureBudgetCandidate> evictions;
};

// 由 EngineLoop 实现：把回收请求按 owner 转发给拥有该碎片的运行时。
// 预算模块本身不持有 World、网格或刚体，因此可以在纯 CPU 测试里直接构造。
class FractureEvictionDispatcher
{
public:
    virtual ~FractureEvictionDispatcher() = default;
    virtual void EvictFractureBudgetShards(
        const std::vector<FractureBudgetCandidate>& candidates) = 0;
};

// 全场景碎片预算账本与降级策略。只做计数、候选排序和取舍，不创建或销毁碎片，
// 也不读写 World/GPU 资源。EngineLoop 持有唯一实例并注入玻璃/气球运行时；
// World 换代时与运行时状态一起 Reset()。
class FractureBudget
{
public:
    // 全局上限：玻璃动态碎片 + attached 板片 + 气球软片的总和。
    static constexpr std::size_t GlobalLimit = 320;
    // 单块玻璃（动态 + attached）与单个气球的上限，保持原有局部预算语义。
    static constexpr std::size_t GlassPaneLimit = 96;
    static constexpr std::size_t BalloonLimit = 48;
    // 气球爆裂的视觉下限：总预算耗尽时宁可短暂超出上限，也不让气球炸成空。
    static constexpr std::size_t MinimumBalloonShards = 4;
    // 动态碎片落地后静止多久才允许被回收。
    static constexpr float SettledSecondsToReclaim = 1.5f;

    void Reset();

    // 生命周期记账；每次创建/销毁碎片或 attached 板片都必须成对调用。
    void OnShardsCreated(const std::string& owner, FractureBudgetKind kind,
        std::size_t count = 1);
    void OnShardsRemoved(const std::string& owner, FractureBudgetKind kind,
        std::size_t count = 1);
    // 注册或刷新可回收候选；attached 板片不调用这里。
    void UpdateReclaimable(const FractureBudgetCandidate& candidate);
    void UntrackReclaimable(const std::string& owner, std::uint64_t shardId);

    // 纯查询：不修改账本，也不执行回收。调用方拿到 evictions 后必须先回收，
    // 再按 grantedCount 创建碎片，并用 OnShardsCreated 记账。
    FractureBudgetDecision Request(const FractureBudgetRequest& request) const;

    std::size_t TotalShards() const { return totalShards; }
    std::size_t OwnerShards(const std::string& owner) const;
    std::size_t ReclaimableShards() const;

private:
    struct Usage
    {
        std::size_t glassDynamic = 0;
        std::size_t glassAttached = 0;
        std::size_t balloonShards = 0;

        std::size_t Total() const
        {
            return glassDynamic + glassAttached + balloonShards;
        }
    };

    static std::size_t& CounterFor(Usage& usage, FractureBudgetKind kind);
    // 只返回静止时长达到阈值的候选，按“屏幕占比最小优先”排序。
    std::vector<FractureBudgetCandidate> ReclaimableByPriority() const;

    std::unordered_map<std::string, Usage> usageByOwner;
    std::unordered_map<std::string, std::vector<FractureBudgetCandidate>>
        reclaimableByOwner;
    std::size_t totalShards = 0;
};

// 运行时统一入口：先 Request，再按决策执行跨 owner 回收，最后返回决策。
FractureBudgetDecision AcquireFractureBudget(
    FractureBudget& budget,
    FractureEvictionDispatcher& dispatcher,
    const FractureBudgetRequest& request);

} // namespace VL::Physics::Fracture
