#include <Eigen/Dense>
#include <memory>

#include "input/inputSubsystem.h"


class SceneNode;
class Controller
{
public:
    Controller();
    void Update(float deltaTime, const VL::InputActionState& input);
    void SetSceneObject(std::shared_ptr<SceneNode> sceneObject);
    void SetMoveVelocity(float moveSpeed);
    void SetRotationSpeed(float mouseRotationSpeed);

private:
    std::shared_ptr<SceneNode> sceneObject;
    float moveSpeed = 0.0f;
    float mouseRotationSpeed = 0.0f;
};
