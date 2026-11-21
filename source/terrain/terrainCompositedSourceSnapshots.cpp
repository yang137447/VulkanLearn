#include "terrain/terrainCompositedSourceSnapshots.h"

#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace VL
{
namespace
{
constexpr uint64_t AllocationOverhead = 256;

uint64_t AddBytes(uint64_t left, uint64_t right)
{
    if (right > std::numeric_limits<uint64_t>::max() - left)
        throw std::overflow_error("Terrain pixel source capacity overflow");
    return left + right;
}

uint64_t PaintBytes(const TerrainLayerPaintData& paint)
{
    if (paint.weights.capacity() > std::numeric_limits<uint64_t>::max() / sizeof(float))
        throw std::overflow_error("Terrain paint capacity overflow");
    return AddBytes(AddBytes(sizeof(TerrainLayerPaintData),
        paint.weights.capacity() * sizeof(float)), paint.opacity.capacity());
}

void ValidatePhase(TerrainRvtResourcePhase phase)
{
    if (static_cast<size_t>(phase) >= static_cast<size_t>(TerrainRvtResourcePhase::Count))
        throw std::invalid_argument("Invalid terrain pixel source phase");
}

void Advance(TerrainRvtBudgetLease& lease, TerrainRvtResourcePhase phase)
{
    if (phase <= lease.Phase())
        return;
    try
    {
        lease.SetPhase(phase);
    }
    catch (const std::logic_error&)
    {
        if (lease.Phase() < phase)
            throw;
    }
}

template<typename Value>
struct LeafAllocator
{
    using value_type = Value;
    size_t limit;

    explicit LeafAllocator(size_t bytes) : limit(bytes) {}
    template<typename Other>
    LeafAllocator(const LeafAllocator<Other>& other) : limit(other.limit) {}

    Value* allocate(size_t count)
    {
        if (count > limit / sizeof(Value))
            throw std::bad_alloc();
        return std::allocator<Value>{}.allocate(count);
    }

    void deallocate(Value* allocation, size_t count)
    {
        std::allocator<Value>{}.deallocate(allocation, count);
    }

    template<typename Other>
    bool operator==(const LeafAllocator<Other>& other) const { return limit == other.limit; }
    template<typename Other>
    bool operator!=(const LeafAllocator<Other>& other) const { return !(*this == other); }
};
}

struct TerrainCompositedSourceSnapshots::State
{
    enum class Kind { Height, Paint };

    struct Leaf
    {
        std::shared_ptr<State> registry;
        std::shared_ptr<const void> source;
        std::optional<TerrainRvtBudgetLease> lease;
        std::weak_ptr<Leaf> self;
        Kind kind;
        Leaf* previous = nullptr;
        Leaf* next = nullptr;
        bool registered = false;

        Leaf(std::shared_ptr<State> state, std::shared_ptr<const void> pixels,
            TerrainRvtBudgetLease reservation, Kind type)
            : registry(std::move(state)), source(std::move(pixels)),
              lease(std::move(reservation)), kind(type)
        {
        }

        ~Leaf()
        {
            std::lock_guard<std::recursive_mutex> lock(registry->mutex);
            if (registered)
            {
                if (previous)
                    previous->next = next;
                else
                    registry->head = next;
                if (next)
                    next->previous = previous;
            }
            source.reset();
            lease.reset();
        }
    };

    mutable std::recursive_mutex mutex;
    Leaf* head = nullptr;

    std::shared_ptr<Leaf> Find(const void* identity, Kind kind) const
    {
        auto* entry = head;
        while (entry)
        {
            auto* next = entry->next;
            auto leaf = entry->self.lock();
            if (leaf && leaf->kind == kind && leaf->source.get() == identity)
                return leaf;
            entry = next;
        }
        return {};
    }

    std::shared_ptr<Leaf> RetainSource(const std::shared_ptr<State>& registry,
        TerrainRvtBudgetOwner& owner, std::shared_ptr<const void> source,
        Kind kind, uint64_t payloadBytes, TerrainRvtResourcePhase phase)
    {
        if (!source)
            return {};
        if (auto leaf = Find(source.get(), kind))
            return leaf;
        TerrainRvtCost cost;
        cost.cpuBytes = AddBytes(payloadBytes, sizeof(Leaf) + sizeof(State) +
            sizeof(TerrainCompositedSourceSnapshots) + AllocationOverhead * 3);
        auto lease = owner.TryReserve(cost, TerrainRvtCostClass::Workspace, phase);
        if (!lease)
            throw std::runtime_error("Terrain pixel source exceeds geometry CPU budget");
        auto leaf = std::allocate_shared<Leaf>(
            LeafAllocator<Leaf>(sizeof(Leaf) + AllocationOverhead),
            registry, std::move(source), std::move(*lease), kind);
        leaf->self = leaf;
        leaf->next = head;
        if (head)
            head->previous = leaf.get();
        head = leaf.get();
        leaf->registered = true;
        return leaf;
    }
};

TerrainCompositedSourceSnapshots::TerrainCompositedSourceSnapshots(
    std::shared_ptr<TerrainRvtBudgetOwner> geometryOwner)
    : owner(std::move(geometryOwner)), state(std::make_shared<State>())
{
    if (!owner)
        throw std::invalid_argument("Terrain pixel sources require a geometry budget owner");
}

TerrainBuildPlan TerrainCompositedSourceSnapshots::Retain(
    const TerrainBuildPlan& plan, TerrainRvtResourcePhase phase)
{
    ValidatePhase(phase);
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    TerrainBuildPlan retained = plan;
    auto height = state->RetainSource(state, *owner, plan.heightmap, State::Kind::Height,
        plan.heightmap ? AddBytes(sizeof(HostImage), plan.heightmap->data.capacity()) : 0, phase);
    auto paint = state->RetainSource(state, *owner, plan.layerPaint, State::Kind::Paint,
        plan.layerPaint ? PaintBytes(*plan.layerPaint) : 0, phase);
    if (height)
        retained.heightmap = std::shared_ptr<const HostImage>(height, plan.heightmap.get());
    if (paint)
        retained.layerPaint = std::shared_ptr<const TerrainLayerPaintData>(paint, plan.layerPaint.get());
    if (height)
        Advance(*height->lease, phase);
    if (paint)
        Advance(*paint->lease, phase);
    return retained;
}

void TerrainCompositedSourceSnapshots::Promote(
    const TerrainBuildPlan& plan, TerrainRvtResourcePhase phase)
{
    ValidatePhase(phase);
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    auto height = state->Find(plan.heightmap.get(), State::Kind::Height);
    auto paint = state->Find(plan.layerPaint.get(), State::Kind::Paint);
    if (height)
        Advance(*height->lease, phase);
    if (paint)
        Advance(*paint->lease, phase);
}

size_t TerrainCompositedSourceSnapshots::LiveSources() const
{
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    size_t count = 0;
    for (auto* entry = state->head; entry; entry = entry->next)
        if (!entry->self.expired())
            ++count;
    return count;
}

std::vector<TerrainRvtBudgetPhaseChange> TerrainCompositedSourceSnapshots::PreparePromote(
    const TerrainBuildPlan& plan, TerrainRvtResourcePhase phase) const
{
    ValidatePhase(phase);
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    std::vector<TerrainRvtBudgetPhaseChange> changes;
    changes.reserve(2);
    const auto append = [&](const std::shared_ptr<State::Leaf>& leaf) {
        if (leaf && leaf->lease && phase > leaf->lease->Phase())
            changes.push_back({*leaf->lease, phase});
    };
    append(state->Find(plan.heightmap.get(), State::Kind::Height));
    append(state->Find(plan.layerPaint.get(), State::Kind::Paint));
    return changes;
}
}
