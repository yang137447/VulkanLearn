#pragma once

#include "terrain/terrainCompositedVT.h"
#include "terrain/terrainRvtBudget.h"

#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace VL
{
class TerrainCompositedProductionPriority
{
    using Coordinates = std::tuple<uint32_t, uint32_t, uint32_t>;
    struct NamespaceState
    {
        std::optional<Coordinates> fine;
        std::optional<Coordinates> coarseSweep;
        uint64_t coarseTurns = 0;
        std::optional<TerrainRvtBudgetLease> metadata;
    };
    struct Cursor
    {
        std::string surfaceId;
        std::optional<TerrainRvtBudgetLease> metadata;
    };

public:
    struct Decision
    {
        size_t index = 0;
        bool root = false;
        bool fine = false;
        bool sweep = false;

    private:
        friend class TerrainCompositedProductionPriority;
        NamespaceState* state = nullptr;
        Cursor next;
    };

    TerrainCompositedProductionPriority(TerrainRvtBudgetOwner& owner, uint32_t poolCapacity)
        : owner(owner), capacity(poolCapacity)
    {
        metadata = Charge(sizeof(*this) + 4096);
    }

    void Prepare(const std::vector<TerrainCompositedVTTaskToken>& requests)
    {
        for (auto iterator = states.begin(); iterator != states.end();)
        {
            const bool present = std::any_of(requests.begin(), requests.end(),
                [&](const auto& token) { return token.key.surfaceId == iterator->first; });
            if (!present)
                iterator = states.erase(iterator);
            else
                ++iterator;
        }
        for (const auto& token : requests)
        {
            if (states.count(token.key.surfaceId))
                continue;
            if (states.size() >= capacity)
                throw std::length_error("Terrain production namespaces exceed physical pool capacity");
            NamespaceState state;
            state.metadata = Charge(sizeof(std::pair<const std::string, NamespaceState>) +
                256 + 2 * (token.key.surfaceId.size() + 1));
            states.emplace(token.key.surfaceId, std::move(state));
        }
    }

    template<class IsRoot>
    std::optional<Decision> Pick(const std::vector<TerrainCompositedVTTaskToken>& requests,
        IsRoot isRoot)
    {
        bool roots = false, coarse = false, fine = false;
        for (const auto& token : requests)
        {
            const bool root = isRoot(token);
            roots = roots || root;
            coarse = coarse || (!root && token.key.mip > 0);
            fine = fine || (!root && token.key.mip == 0);
        }
        if (!roots && !coarse && !fine)
            return {};
        Decision decision;
        decision.root = roots;
        decision.fine = !roots && fine && (!coarse || nonRootTurns % 4 == 3);
        const auto member = [&](const auto& token) {
            if (roots)
                return isRoot(token);
            return !isRoot(token) && (decision.fine ? token.key.mip == 0 : token.key.mip > 0);
        };
        const auto& last = roots ? rootCursor : (decision.fine ? fineCursor : coarseCursor);
        std::string_view first, after;
        for (const auto& token : requests)
            if (member(token))
            {
                const std::string_view name(token.key.surfaceId);
                if (first.empty() || name < first)
                    first = name;
                if (name > last.surfaceId && (after.empty() || name < after))
                    after = name;
            }
        const auto chosen = after.empty() ? first : after;
        const auto found = states.find(chosen);
        if (found == states.end())
            throw std::logic_error("Terrain production priority was not prepared");
        auto& state = found->second;
        decision.sweep = !roots && !decision.fine && state.coarseTurns % 8 == 7;
        const auto bookmark = decision.fine ? state.fine : state.coarseSweep;
        std::optional<size_t> minimum, successor;
        for (size_t index = 0; index < requests.size(); ++index)
        {
            const auto& token = requests[index];
            if (!member(token) || token.key.surfaceId != chosen)
                continue;
            const auto less = [&](size_t left, size_t right) {
                const auto leftKey = Key(requests[left].key);
                const auto rightKey = Key(requests[right].key);
                if (!roots && !decision.fine && !decision.sweep &&
                    std::get<0>(leftKey) != std::get<0>(rightKey))
                    return std::get<0>(leftKey) > std::get<0>(rightKey);
                return leftKey < rightKey;
            };
            if (!minimum || less(index, *minimum))
                minimum = index;
            if ((decision.fine || decision.sweep) && bookmark && Key(token.key) > *bookmark &&
                (!successor || Key(token.key) < Key(requests[*successor].key)))
                successor = index;
        }
        decision.index = successor.value_or(*minimum);
        decision.state = &state;
        decision.next.metadata = Charge(128 + 2 * (chosen.size() + 1));
        decision.next.surfaceId = chosen;
        return decision;
    }

    void CommitSuccessful(const std::vector<TerrainCompositedVTTaskToken>& requests,
        Decision& decision) noexcept
    {
        auto& state = *decision.state;
        auto& cursor = decision.root ? rootCursor : (decision.fine ? fineCursor : coarseCursor);
        cursor.surfaceId.swap(decision.next.surfaceId);
        cursor.metadata.swap(decision.next.metadata);
        if (!decision.root)
        {
            ++nonRootTurns;
            if (decision.fine)
                state.fine = Key(requests[decision.index].key);
            else
            {
                ++state.coarseTurns;
                if (decision.sweep)
                    state.coarseSweep = Key(requests[decision.index].key);
            }
        }
    }

private:
    std::optional<TerrainRvtBudgetLease> Charge(uint64_t bytes)
    {
        auto lease = owner.TryReserve({0, bytes, 0, 0}, TerrainRvtCostClass::Workspace,
            TerrainRvtResourcePhase::Active);
        if (!lease)
            throw std::runtime_error("Terrain production priority exceeds frozen CPU workspace");
        return lease;
    }

    static Coordinates Key(const TerrainCompositedVTPageKey& key) noexcept
    {
        return {key.mip, key.x, key.y};
    }

    TerrainRvtBudgetOwner& owner;
    uint32_t capacity;
    std::optional<TerrainRvtBudgetLease> metadata;
    std::map<std::string, NamespaceState, std::less<>> states;
    Cursor rootCursor, coarseCursor, fineCursor;
    uint64_t nonRootTurns = 0;
};
}
