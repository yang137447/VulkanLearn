#pragma once

#include <memory>
#include <string>
#include <vector>

#include <Eigen/Core>

#include "world/worldShowcase.h"

namespace VL
{

class UiSubsystem;
class World;
struct PlatformEvent;

// 布料展示关卡的状态机。只做三件事：材质可见性、模特旋转、相机推拉，
// 然后把结果折成 ShowcaseViewModel 交给 UiSubsystem。
// 非展示关卡（World 没有 showcase 块）时 Active() 为 false，控制器不产生任何副作用。
class ShowcaseController
{
public:
    void AttachUi(UiSubsystem& ui);
    // World 提交后调用；传 nullptr 表示回到非展示关卡。
    void OnWorldChanged(const std::shared_ptr<World>& world);
    // C# 蓝图提交的决策（材质/面板）；应用后清空请求。
    void ApplyExternalState(const ShowcaseStateRequest& request);
    // 展示是否处于"呈现"状态：游戏档常开，编辑器只有 Play 时打开。
    // 关闭时恢复 World 提交时的可见性、TRS 和相机，UI 也随之隐藏。
    void SetActive(bool value);
    void Update();
    // 只收到 UiSubsystem 没有消费的指针事件。
    bool HandlePlatformEvent(const PlatformEvent& event);
    bool HasShowcase() const { return world != nullptr && config.enabled; }
    bool Active() const { return HasShowcase() && activeState; }

private:
    void SelectMaterial(int index);
    void ClosePanel();
    void ApplyMaterialVisibility();
    void ApplyDefaultVisibility();
    void ApplyRotation();
    void ApplyZoom();
    void Publish();
    void ResetRuntimeState();
    void RestoreBase();

    UiSubsystem* ui = nullptr;
    std::shared_ptr<World> world;
    WorldShowcase config{};
    bool activeState = false;
    int selected = 0;
    bool panelOpen = false;
    bool dirty = true;

    std::vector<Eigen::Vector3f> basePositions;
    std::vector<Eigen::Vector3f> baseRotations;
    std::vector<Eigen::Vector3f> baseScales;
    std::vector<bool> baseVisibility;
    Eigen::Vector3f cameraBasePosition = Eigen::Vector3f::Zero();
    Eigen::Vector3f cameraBaseRotation = Eigen::Vector3f::Zero();
    Eigen::Vector3f cameraForward = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    float cameraBaseDistance = 700.0f;
    float yawDegrees = 0.0f;
    float zoom = 1.0f;

    bool dragging = false;
    float lastMouseX = 0.0f;
    float lastMouseY = 0.0f;
};

} // namespace VL
