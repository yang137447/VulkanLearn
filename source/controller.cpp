#include "controller.h"
#include <iostream>

#include "sceneObject.h"

Controller::Controller()
{
    
}

Controller::Controller(SDL_Window* window)
{
    this->window = window;
}

void Controller::Update(float deltaTime)
{
    // 鼠标控制
    SDL_SetWindowRelativeMouseMode(window, true);  // 启用相对鼠标模式
    
    float deltaX, deltaY;
    SDL_GetRelativeMouseState(&deltaX, &deltaY);  // 获取相对移动
    
    float deltaRotationY = -deltaX * mouseRotationSpeed;  // 调整灵敏度
    float deltaRotationX = -deltaY * mouseRotationSpeed;
 
    // 键盘控制
    const bool* state = SDL_GetKeyboardState(NULL);
    Eigen::Vector3f moveFactor(0, 0, 0);
    if(state[SDL_SCANCODE_W])
    {
        moveFactor.z() = 1;
    }
    else if (state[SDL_SCANCODE_S])
    {
        moveFactor.z() = -1;
    }
    if (state[SDL_SCANCODE_A])
    {
        moveFactor.x() = -1;
    }
    else if (state[SDL_SCANCODE_D])
    {
        moveFactor.x() = 1;
    }
    if (state[SDL_SCANCODE_Q])
    {
        moveFactor.y() = -1;
    }
    else if (state[SDL_SCANCODE_E])
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