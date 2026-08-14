#pragma once

#include <cstdint>
#include <memory>

#include "input/inputSubsystem.h"


class SceneNode;
class Controller
{
public:
    Controller() = default;
    void Update(float deltaTime, const VL::InputActionState& input);
    void SetViewTarget(
        std::shared_ptr<SceneNode> viewTarget,
        uint64_t worldGeneration = 0) noexcept;
    uint64_t GetBoundWorldGeneration() const noexcept
    {
        return boundWorldGeneration;
    }
    void SetMoveVelocity(float moveSpeed);
    void SetRotationSpeed(float mouseRotationSpeed);

private:
    std::shared_ptr<SceneNode> viewTarget;
    uint64_t boundWorldGeneration = 0;
    float moveSpeed = 0.0f;
    float mouseRotationSpeed = 0.0f;
};
