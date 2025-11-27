#include <Eigen/Dense>
#include <memory>
#include "SDL3/SDL.h"


class SceneNode;
class Controller
{
public:
    Controller(SDL_Window* window);
    void Update(float deltaTime);
    void SetSceneObject(std::shared_ptr<SceneNode> sceneObject);
    void SetMoveVelocity(float moveSpeed);
    void SetRotationSpeed(float mouseRotationSpeed);

private:
    Controller();
    std::shared_ptr<SceneNode> sceneObject;
    float moveSpeed;
    float mouseRotationSpeed;

    SDL_Window* window;
};
