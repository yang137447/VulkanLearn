#include <memory>

#include <gtest/gtest.h>

#include "controller.h"
#include "sceneNode.h"

namespace
{

TEST(ControllerMovement, DefaultSpeedIsOneThousandCentimetersPerSecond)
{
    Controller controller;
    EXPECT_FLOAT_EQ(controller.GetMoveVelocity(), 1000.0f);
}

TEST(ControllerMovement, DefaultForwardMovementTravelsTenMetersInOneSecond)
{
    auto target = std::make_shared<SceneNode>();
    Controller controller;
    controller.SetViewTarget(target);
    VL::InputActionState input;
    input.moveForward = true;

    controller.Update(1.0f, input);

    EXPECT_TRUE(target->GetPosition().isApprox(Eigen::Vector3f(0, 0, -1000)));
}

TEST(ControllerMovement, AuthoredSpeedAndTimeAreAppliedWithoutAnotherUnitConversion)
{
    auto target = std::make_shared<SceneNode>();
    Controller controller;
    controller.SetViewTarget(target);
    controller.SetMoveVelocity(500.0f);
    VL::InputActionState input;
    input.moveRight = true;

    controller.Update(0.25f, input);

    EXPECT_FLOAT_EQ(target->GetPosition().x(), 125.0f);
}

TEST(ControllerMovement, DiagonalInputDoesNotIncreaseMovementSpeed)
{
    auto target = std::make_shared<SceneNode>();
    Controller controller;
    controller.SetViewTarget(target);
    VL::InputActionState input;
    input.moveForward = true;
    input.moveRight = true;
    input.moveUp = true;

    controller.Update(1.0f, input);

    EXPECT_NEAR(target->GetPosition().norm(), 1000.0f, 1.0e-3f);
}

TEST(ControllerMovement, DifferentFrameRatesProduceTheSameOneSecondDisplacement)
{
    auto slowTarget = std::make_shared<SceneNode>();
    auto fastTarget = std::make_shared<SceneNode>();
    Controller slowController;
    Controller fastController;
    slowController.SetViewTarget(slowTarget);
    fastController.SetViewTarget(fastTarget);
    VL::InputActionState input;
    input.moveForward = true;

    for (int frame = 0; frame < 30; ++frame)
        slowController.Update(1.0f / 30.0f, input);
    for (int frame = 0; frame < 120; ++frame)
        fastController.Update(1.0f / 120.0f, input);

    EXPECT_NEAR(slowTarget->GetPosition().z(), -1000.0f, 1.0e-2f);
    EXPECT_NEAR(fastTarget->GetPosition().z(), -1000.0f, 1.0e-2f);
    EXPECT_NEAR(slowTarget->GetPosition().z(), fastTarget->GetPosition().z(), 1.0e-2f);
}

TEST(ControllerMovement, MovementSpeedDoesNotChangeMouseRotation)
{
    auto slowTarget = std::make_shared<SceneNode>();
    auto fastTarget = std::make_shared<SceneNode>();
    Controller slowController;
    Controller fastController;
    slowController.SetViewTarget(slowTarget);
    fastController.SetViewTarget(fastTarget);
    slowController.SetMoveVelocity(50.0f);
    fastController.SetMoveVelocity(10000.0f);
    slowController.SetRotationSpeed(0.1f);
    fastController.SetRotationSpeed(0.1f);
    VL::InputActionState input;
    input.mouseDeltaX = 10.0f;
    input.mouseDeltaY = 20.0f;

    slowController.Update(1.0f / 30.0f, input);
    fastController.Update(1.0f / 120.0f, input);

    EXPECT_GT(slowTarget->GetRotation().norm(), 0.0f);
    EXPECT_TRUE(slowTarget->GetRotation().isApprox(fastTarget->GetRotation()));
}

} // namespace
