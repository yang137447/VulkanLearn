#include "engine/modelPreviewCamera.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include "sceneNode.h"
#include "world/world.h"

namespace VL
{
bool ModelPreviewCamera::Synchronize(World& world, float aspect)
{
    if (generation == world.GetGeneration()) return false;
    generation = world.GetGeneration();
    active = false;
    if (std::filesystem::path(world.GetScenePath()).filename() != "SC_model_preview.json" || !world.GetCamera())
        return false;

    Eigen::Vector3f lower = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    Eigen::Vector3f upper = Eigen::Vector3f::Constant(std::numeric_limits<float>::lowest());
    for (const auto& entry : world.GetMeshObjects())
    {
        const auto& object = entry.second;
        if (object.sceneObjectIdentity != "preview_model" && object.sceneObjectIdentity != "preview_material" &&
            object.sceneObjectIdentity.rfind("__blueprint/~preview.", 0) != 0) continue;
        active = true;
        // 合并所有 section 的变换后包围盒；环境背板不参与模型取景。
        for (float x : {object.localBoundsMin.x(), object.localBoundsMax.x()})
            for (float y : {object.localBoundsMin.y(), object.localBoundsMax.y()})
                for (float z : {object.localBoundsMin.z(), object.localBoundsMax.z()})
                {
                    const Eigen::Vector3f corner = (object.model * Eigen::Vector4f(x, y, z, 1.0f)).head<3>();
                    lower = lower.cwiseMin(corner);
                    upper = upper.cwiseMax(corner);
                }
    }
    if (!active)
        for (const auto& actor : world.GetScriptBlueprints())
            if (actor.at("id").get<std::string>().rfind("~preview.", 0) == 0)
            {
                // 空 Actor/纯相机组件仍可导航；无网格时给原点一个厘米级观察范围。
                active = true;
                lower = Eigen::Vector3f::Constant(-50);
                upper = Eigen::Vector3f::Constant(50);
                break;
            }
    if (!active) return false;
    // 固定平面只适合正面取景；环绕预览隐藏它，保留场景原有 HDRI 照明和背景。
    world.SetMeshSceneObjectVisible("neutral_backdrop", false);
    center = (lower + upper) * 0.5f;
    // 点状/退化模型仍需要可操作的厘米级导航范围。
    radius = std::max((upper - lower).norm() * 0.5f, 1.0f);
    Frame(world, aspect);
    return true;
}

void ModelPreviewCamera::Frame(World& world, float aspect)
{
    const float horizontalHalfAngle = world.GetCamera()->GetHFOV() * 0.00872664626f;
    const float verticalHalfAngle = std::atan(std::tan(horizontalHalfAngle) / aspect);
    distance = 1.1f * radius / std::sin(std::min(horizontalHalfAngle, verticalHalfAngle));
    minimumDistance = radius * 0.02f;
    maximumDistance = distance * 20.0f;
    target = center;
    yaw = pitch = 0.0f;
    // 导航距离与裁剪范围随模型尺度变化，不沿用固定预览场景的 3000 cm 上限。
    world.GetCamera()->SetClip(radius * 0.001f, maximumDistance + radius * 2.0f);
    Publish(world);
}

bool ModelPreviewCamera::Apply(World& world, const ModelPreviewCameraInput& input, float aspect, float viewportWidth)
{
    if (!active || generation != world.GetGeneration() || input.worldGeneration != generation ||
        !std::isfinite(input.x) || !std::isfinite(input.y)) return false;
    switch (input.mode)
    {
    case ModelPreviewNavigation::Orbit:
        yaw = std::remainder(yaw - input.x * 0.006f, 6.28318530718f);
        pitch = std::clamp(pitch + input.y * 0.006f, -1.553343f, 1.553343f);
        break;
    case ModelPreviewNavigation::Pan:
    {
        const auto& camera = world.GetCamera();
        const float unitsPerPixel = 2.0f * distance * std::tan(camera->GetHFOV() * 0.00872664626f) / viewportWidth;
        target += unitsPerPixel * (-input.x * camera->GetRightVector() + input.y * camera->GetUpVector());
        break;
    }
    case ModelPreviewNavigation::Zoom:
        // RmlUi 的正滚轮值表示向下滚；指数缩放使远近距离都有一致的操作比例。
        distance = std::clamp(distance * std::exp(std::clamp(input.y * 0.15f, -20.0f, 20.0f)),
            minimumDistance, maximumDistance);
        break;
    case ModelPreviewNavigation::Frame:
        Frame(world, aspect);
        return true;
    }
    Publish(world);
    return true;
}

void ModelPreviewCamera::Publish(World& world)
{
    const Eigen::Vector3f offset(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
        std::cos(yaw) * std::cos(pitch));
    world.GetCamera()->SetCamera(target + distance * offset, target, Eigen::Vector3f::UnitY());
}
}
