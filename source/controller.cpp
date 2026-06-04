#include "controller.h"

#include <Eigen/Dense>

#include "sceneNode.h"

void Controller::Update(float deltaTime, const VL::InputActionState& input)
{
    float deltaRotationY = -input.mouseDeltaX * mouseRotationSpeed;
    float deltaRotationX = -input.mouseDeltaY * mouseRotationSpeed;
 
    Eigen::Vector3f moveFactor(0, 0, 0);
    if(input.moveForward)
    {
        moveFactor.z() = 1;
    }
    else if (input.moveBackward)
    {
        moveFactor.z() = -1;
    }
    if (input.moveLeft)
    {
        moveFactor.x() = -1;
    }
    else if (input.moveRight)
    {
        moveFactor.x() = 1;
    }
    if (input.moveDown)
    {
        moveFactor.y() = -1;
    }
    else if (input.moveUp)
    {
        moveFactor.y() = 1;
    }
    moveFactor.normalize();

    Eigen::Vector3f position = viewTarget->GetPosition();
    Eigen::Vector3f forward = viewTarget->GetForwardVector();
    Eigen::Vector3f right = viewTarget->GetRightVector();
    Eigen::Vector3f up = viewTarget->GetUpVector();
    position += moveFactor.z() * forward * deltaTime * moveSpeed;
    position += moveFactor.x() * right * deltaTime * moveSpeed;
    position += moveFactor.y() * up * deltaTime * moveSpeed;
    
    viewTarget->SetPosition(position);
    Eigen::Vector3f deltaRot(deltaRotationX, deltaRotationY, 0.0f);
    deltaRot *= deltaTime *mouseRotationSpeed;
    viewTarget->SetDeltaRotation(deltaRot);
}

void Controller::SetViewTarget(std::shared_ptr<SceneNode> viewTarget)
{
    this->viewTarget = viewTarget;
}

void Controller::SetMoveVelocity(float moveSpeed)
{
    this->moveSpeed = moveSpeed;
}

void Controller::SetRotationSpeed(float mouseRotationSpeed)
{
    this->mouseRotationSpeed = mouseRotationSpeed;
}
