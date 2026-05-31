#include "controller.h"

#include "sceneObject.h"

Controller::Controller()
{
    
}

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

    Eigen::Vector3f position = sceneObject->GetPosition();
    Eigen::Vector3f rotation = sceneObject->GetRotation();
    Eigen::Vector3f forward = sceneObject->GetForwardVector();
    Eigen::Vector3f right = sceneObject->GetRightVector();
    Eigen::Vector3f up = sceneObject->GetUpVector();
    position += moveFactor.z() * forward * deltaTime * moveSpeed;
    position += moveFactor.x() * right * deltaTime * moveSpeed;
    position += moveFactor.y() * up * deltaTime * moveSpeed;
    
    sceneObject->SetPosition(position);
    //sceneObject->SetRotation(rotation);
    Eigen::Vector3f deltaRot(deltaRotationX, deltaRotationY, 0.0f);
    deltaRot *= deltaTime *mouseRotationSpeed;
    sceneObject->SetDeltaRotation(deltaRot);
}

void Controller::SetSceneObject(std::shared_ptr<SceneNode> sceneObject)
{
    this->sceneObject = sceneObject;
}

void Controller::SetMoveVelocity(float moveSpeed)
{
    this->moveSpeed = moveSpeed;
}

void Controller::SetRotationSpeed(float mouseRotationSpeed)
{
    this->mouseRotationSpeed = mouseRotationSpeed;
}
