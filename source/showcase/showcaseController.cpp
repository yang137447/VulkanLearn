#include "showcase/showcaseController.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "platform/platformEvent.h"
#include "sceneNode.h"
#include "showcase/showcaseViewModel.h"
#include "ui/uiSubsystem.h"
#include "world/world.h"

namespace VL
{
namespace
{
constexpr float kDragDegreesPerPixel = 0.35f;
constexpr float kMinZoom = 0.42f;   // 约 300 cm
constexpr float kMaxZoom = 1.65f;   // 约 1200 cm
constexpr float kZoomPerWheelStep = 0.12f;
} // namespace

void ShowcaseController::AttachUi(UiSubsystem& uiSubsystem)
{
    ui = &uiSubsystem;
    ui->SetShowcaseCallbacks(
        [this](int index) { SelectMaterial(index); },
        [this] { ClosePanel(); });
}

void ShowcaseController::OnWorldChanged(const std::shared_ptr<World>& newWorld)
{
    world = newWorld;
    config = world != nullptr ? world->GetShowcase() : WorldShowcase{};
    ResetRuntimeState();
    if (!config.enabled)
    {
        Publish();
        return;
    }

    selected = static_cast<int>(
        std::min(config.defaultIndex, config.materials.size() - 1));
    basePositions.clear();
    baseRotations.clear();
    baseScales.clear();
    baseVisibility.clear();
    basePositions.reserve(config.materials.size());
    baseRotations.reserve(config.materials.size());
    baseScales.reserve(config.materials.size());
    baseVisibility.reserve(config.materials.size());
    for (const ShowcaseMaterialVariant& material : config.materials)
    {
        Eigen::Vector3f position = Eigen::Vector3f::Zero();
        Eigen::Vector3f rotation = Eigen::Vector3f::Zero();
        Eigen::Vector3f scale = Eigen::Vector3f::Ones();
        world->GetMeshSceneObjectTransform(
            material.objectName, position, rotation, scale);
        basePositions.push_back(position);
        baseRotations.push_back(rotation);
        baseScales.push_back(scale);
        baseVisibility.push_back(
            world->IsMeshSceneObjectVisible(material.objectName));
    }

    if (const std::shared_ptr<Camera>& camera = world->GetCamera())
    {
        cameraBasePosition = camera->GetPosition();
        cameraBaseRotation = camera->GetRotation();
        cameraForward = camera->GetForwardVector();
        const Eigen::Vector3f pivot(
            config.rotatePivot[0], config.rotatePivot[1], config.rotatePivot[2]);
        cameraBaseDistance = std::max(
            1.0f, (cameraBasePosition - pivot).norm());
    }

    // 可见性始终应用：编辑器里也只显示默认变体，避免四个重合网格互相闪烁。
    // 旋转/相机只在呈现态应用，编辑态保持作者机位。
    ApplyDefaultVisibility();
    if (Active())
    {
        ApplyRotation();
        ApplyZoom();
    }
    dirty = true;
    Publish();
}

void ShowcaseController::ResetRuntimeState()
{
    selected = 0;
    panelOpen = false;
    yawDegrees = 0.0f;
    zoom = 1.0f;
    dragging = false;
    lastMouseX = 0.0f;
    lastMouseY = 0.0f;
    basePositions.clear();
    baseRotations.clear();
    baseScales.clear();
    baseVisibility.clear();
    dirty = true;
}

void ShowcaseController::SetActive(bool value)
{
    if (activeState == value)
    {
        return;
    }
    activeState = value;
    if (!HasShowcase())
    {
        dirty = true;
        Publish();
        return;
    }

    if (activeState)
    {
        // 每次进入呈现状态都从关卡默认值开始，避免上一次 Play 的残留。
        ResetRuntimeState();
        selected = static_cast<int>(
            std::min(config.defaultIndex, config.materials.size() - 1));
        ApplyMaterialVisibility();
        ApplyRotation();
        ApplyZoom();
    }
    else
    {
        // 恢复作者 TRS/相机，但仍然只显示默认变体。
        RestoreBase();
        ResetRuntimeState();
        selected = static_cast<int>(
            std::min(config.defaultIndex, config.materials.size() - 1));
        ApplyDefaultVisibility();
    }
    dirty = true;
    Publish();
}

void ShowcaseController::RestoreBase()
{
    if (!HasShowcase())
    {
        return;
    }
    for (std::size_t index = 0; index < config.materials.size(); ++index)
    {
        const std::string& objectName = config.materials[index].objectName;
        world->SetMeshSceneObjectVisible(
            objectName,
            index < baseVisibility.size() ? baseVisibility[index] : true);
        if (index < basePositions.size() && index < baseRotations.size())
        {
            world->SetMeshSceneObjectTransform(
                objectName,
                basePositions[index],
                baseRotations[index],
                index < baseScales.size() ? baseScales[index]
                                          : Eigen::Vector3f::Ones());
        }
    }
    if (const std::shared_ptr<Camera>& camera = world->GetCamera())
    {
        camera->SetCamera(cameraBasePosition, cameraBaseRotation);
    }
}

void ShowcaseController::Update()
{
    if (dirty)
    {
        Publish();
    }
}

void ShowcaseController::ApplyExternalState(const ShowcaseStateRequest& request)
{
    if (!Active() || config.materials.empty())
    {
        return;
    }
    if (!request.materialId.empty())
    {
        for (std::size_t index = 0; index < config.materials.size(); ++index)
        {
            if (config.materials[index].id != request.materialId) continue;
            selected = static_cast<int>(index);
            ApplyMaterialVisibility();
            break;
        }
    }
    panelOpen = request.panelOpen;
    ApplyZoom();
    dirty = true;
}

void ShowcaseController::SelectMaterial(int index)
{
    if (!Active() || config.materials.empty())
    {
        return;
    }
    selected = std::clamp(index, 0, static_cast<int>(config.materials.size()) - 1);
    panelOpen = true;
    ApplyMaterialVisibility();
    ApplyZoom();
    dirty = true;
}

void ShowcaseController::ClosePanel()
{
    if (!panelOpen)
    {
        return;
    }
    panelOpen = false;
    ApplyZoom();
    dirty = true;
}

void ShowcaseController::ApplyMaterialVisibility()
{
    if (!Active())
    {
        return;
    }
    for (int i = 0; i < static_cast<int>(config.materials.size()); ++i)
    {
        world->SetMeshSceneObjectVisible(
            config.materials[static_cast<std::size_t>(i)].objectName,
            i == selected);
    }
}

void ShowcaseController::ApplyDefaultVisibility()
{
    // 编辑态/加载后只保留关卡默认变体，避免 4 个重合网格同时参与绘制和投影。
    // 它不依赖 active：预览态由 ApplyMaterialVisibility 接管选中项。
    if (!HasShowcase())
    {
        return;
    }
    const std::size_t defaultIndex = std::min(
        config.defaultIndex, config.materials.size() - 1);
    for (std::size_t i = 0; i < config.materials.size(); ++i)
    {
        world->SetMeshSceneObjectVisible(
            config.materials[i].objectName, i == defaultIndex);
    }
}

void ShowcaseController::ApplyRotation()
{
    if (!Active())
    {
        return;
    }
    const float radians = yawDegrees * 3.14159265358979f / 180.0f;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const Eigen::Vector3f pivot(
        config.rotatePivot[0], config.rotatePivot[1], config.rotatePivot[2]);
    for (std::size_t i = 0; i < config.materials.size() && i < basePositions.size(); ++i)
    {
        const Eigen::Vector3f offset = basePositions[i] - pivot;
        const Eigen::Vector3f rotated(
            offset.x() * cosine + offset.z() * sine,
            offset.y(),
            -offset.x() * sine + offset.z() * cosine);
        Eigen::Vector3f rotation = baseRotations[i];
        rotation.y() += yawDegrees;
        world->SetMeshSceneObjectTransform(
            config.materials[i].objectName,
            pivot + rotated,
            rotation,
            i < baseScales.size() ? baseScales[i] : Eigen::Vector3f::Ones());
    }
}

void ShowcaseController::ApplyZoom()
{
    if (!Active())
    {
        return;
    }
    const std::shared_ptr<Camera>& camera = world->GetCamera();
    if (!camera)
    {
        return;
    }
    Eigen::Vector3f position =
        cameraBasePosition + cameraForward * (cameraBaseDistance * (1.0f - zoom));
    if (!panelOpen)
    {
        const Eigen::Vector3f pivot(
            config.rotatePivot[0], config.rotatePivot[1], config.rotatePivot[2]);
        const Eigen::Vector3f right = camera->GetRightVector();
        position += right * (pivot - position).dot(right);
    }
    camera->SetCamera(position, camera->GetRotation());
}

bool ShowcaseController::HandlePlatformEvent(const PlatformEvent& event)
{
    if (!Active())
    {
        return false;
    }
    switch (event.type)
    {
    case PlatformEventType::MouseButtonDown:
        if (event.mouseButton == PlatformMouseButton::Left)
        {
            dragging = true;
            lastMouseX = event.mouseX;
            lastMouseY = event.mouseY;
            return true;
        }
        return false;
    case PlatformEventType::MouseButtonUp:
        if (event.mouseButton == PlatformMouseButton::Left && dragging)
        {
            dragging = false;
            return true;
        }
        return false;
    case PlatformEventType::MouseMove:
        if (!dragging)
        {
            return false;
        }
        {
            const float delta = event.mouseDeltaX != 0.0f
                ? event.mouseDeltaX
                : event.mouseX - lastMouseX;
            lastMouseX = event.mouseX;
            lastMouseY = event.mouseY;
            yawDegrees = std::fmod(
                yawDegrees + delta * kDragDegreesPerPixel, 360.0f);
            ApplyRotation();
            dirty = true;
        }
        return true;
    case PlatformEventType::MouseWheel:
        if (event.wheelY == 0.0f)
        {
            return false;
        }
        zoom = std::clamp(
            zoom * (1.0f - event.wheelY * kZoomPerWheelStep),
            kMinZoom,
            kMaxZoom);
        ApplyZoom();
        dirty = true;
        return true;
    case PlatformEventType::MouseLeave:
        dragging = false;
        return false;
    default:
        return false;
    }
}

void ShowcaseController::Publish()
{
    ShowcaseViewModel model;
    model.visible = Active();
    model.panelOpen = model.visible && panelOpen;
    if (model.visible && !config.materials.empty())
    {
        const ShowcaseMaterialVariant& material =
            config.materials[static_cast<std::size_t>(selected)];
        model.material = selected;
        model.name = material.label;
        model.tag = material.tag;
        model.sheenHex = material.sheenHex;
        model.baseHex = "#F2EDE4";
    }
    // 面板展示的是材质参数，不是视角；拖动 yaw 不写进这一行。
    model.rotation = "0°";
    if (ui != nullptr)
    {
        ui->SetShowcaseModel(model);
    }
    dirty = false;
}

} // namespace VL
