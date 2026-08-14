#pragma once

// File responsibility: Prepares an old live Vulkan resource for epoch
// retirement without arming its destruction until the live-owner swap begins.

#include <functional>
#include <memory>
#include <utility>

namespace VL
{

class PreparedRetiredResourcePackage
{
public:
    PreparedRetiredResourcePackage() = default;

    explicit PreparedRetiredResourcePackage(
        std::function<void()> destroy)
    {
        std::shared_ptr<State> preparedState(
            new State{false, std::move(destroy)},
            [](State* state)
            {
                if (state->armed && state->destroy)
                {
                    state->destroy();
                }
                delete state;
            });
        resource = std::shared_ptr<void>(
            preparedState,
            static_cast<void*>(preparedState.get()));
        state = std::move(preparedState);
    }

    PreparedRetiredResourcePackage(
        PreparedRetiredResourcePackage&&) noexcept = default;
    PreparedRetiredResourcePackage& operator=(
        PreparedRetiredResourcePackage&&) noexcept = default;

    PreparedRetiredResourcePackage(
        const PreparedRetiredResourcePackage&) = delete;
    PreparedRetiredResourcePackage& operator=(
        const PreparedRetiredResourcePackage&) = delete;

    explicit operator bool() const noexcept
    {
        return static_cast<bool>(resource);
    }

    std::shared_ptr<void> TakeResource() noexcept
    {
        return std::move(resource);
    }

    void Activate() noexcept
    {
        if (state)
        {
            state->armed = true;
        }
    }

private:
    struct State
    {
        bool armed = false;
        std::function<void()> destroy;
    };

    std::shared_ptr<State> state;
    std::shared_ptr<void> resource;
};

inline PreparedRetiredResourcePackage
MakePreparedRetiredResourcePackage(std::function<void()> destroy)
{
    return PreparedRetiredResourcePackage(std::move(destroy));
}

} // namespace VL
