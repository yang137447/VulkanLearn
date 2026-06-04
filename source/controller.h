#pragma once

#include <memory>

#include "input/inputSubsystem.h"


class SceneNode;
class Controller
{
public:
    Controller() = default;
    void Update(float deltaTime, const VL::InputActionState& input);
    void SetViewTarget(std::shared_ptr<SceneNode> viewTarget);
    void SetMoveVelocity(float moveSpeed);
    void SetRotationSpeed(float mouseRotationSpeed);

private:
    std::shared_ptr<SceneNode> viewTarget;
    float moveSpeed = 0.0f;
    float mouseRotationSpeed = 0.0f;
};
