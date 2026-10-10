#pragma once

#include <cstdint>
#include <memory>

#include "input/inputSubsystem.h"


class SceneNode;
class Controller
{
public:
    // 世界位置为 cm，移动速度为 cm/s；保持旧默认 10 m/s 的实际移动尺度。
    static constexpr float DefaultMoveSpeed = 1000.0f;
    Controller() = default;
    void Update(float deltaTime, const VL::InputActionState& input);
    void SetViewTarget(
        std::shared_ptr<SceneNode> viewTarget,
        uint64_t worldGeneration = 0) noexcept;
    uint64_t GetBoundWorldGeneration() const noexcept
    {
        return boundWorldGeneration;
    }
    // 调用方按 cm/s 提供速度；Update 的 deltaTime 单位为秒。
    void SetMoveVelocity(float moveSpeed);
    float GetMoveVelocity() const { return moveSpeed; }
    void SetRotationSpeed(float mouseRotationSpeed);

private:
    std::shared_ptr<SceneNode> viewTarget;
    uint64_t boundWorldGeneration = 0;
    float moveSpeed = DefaultMoveSpeed;
    float mouseRotationSpeed = 0.0f;
};
