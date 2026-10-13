#pragma once

#include <cstdint>
#include <Eigen/Core>
#include "ui/uiAction.h"

namespace VL
{
class World;

// GT 独占的预览导航状态；不持有 World，不写模型 transform 或资产文件。
class ModelPreviewCamera
{
public:
    bool Synchronize(World& world, float aspect);
    bool Apply(World& world, const ModelPreviewCameraInput& input, float aspect, float viewportWidth);

private:
    void Frame(World& world, float aspect);
    void Publish(World& world);
    uint64_t generation = 0;
    bool active = false;
    Eigen::Vector3f center = Eigen::Vector3f::Zero();
    Eigen::Vector3f target = Eigen::Vector3f::Zero();
    float radius = 1.0f;
    float distance = 1.0f;
    float minimumDistance = 1.0f;
    float maximumDistance = 1.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
};
}
