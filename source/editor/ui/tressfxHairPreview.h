#pragma once

#include <array>
#include <cmath>
#include <string_view>

namespace VL::EditorUi
{
// 毛发资产视口只预览无 simulation.skinning 的 tressfx 描述符。带 skinning 的资产
// 需要源角色姿态，由蓝图 TressFX 组件承载；视口既不预览也不静默剥离 binding
// （剥掉会让 collision.capsules[].bone 校验失效并给出误导性画面）。
inline constexpr std::string_view TressfxHairSkinnedAssetMessage =
    "This hair asset declares simulation.skinning. Hair bound to a character is authored "
    "by the Blueprint TressFX component; this viewport previews hair without skinning.";

// 返回挡预览的说明；允许预览时返回空 view。纯函数，供 editor_ui_tests 固定文案与条件。
inline std::string_view TressfxHairPreviewBlockedMessage(bool tressfxAsset, bool hasSkinning)
{
    return tressfxAsset && hasSkinning ? TressfxHairSkinnedAssetMessage : std::string_view{};
}

// 左键拖动只绕资产原点的竖直 Y 轴改变 yaw，忽略垂直位移；不另设发根或包围盒 pivot。
// 角度只属于预览世界，不写 SC/TF，也不参与资产导入身份。
struct TressfxHairRotation
{
    float yaw = 0.0f;

    void Reset()
    {
        yaw = 0.0f;
    }

    void Drag(float deltaX, float /*deltaY*/)
    {
        // 半度/像素：拖过约 720 px 转一圈，既能快速摆头又不至于抖。
        constexpr float DegreesPerPixel = 0.5f;
        yaw = std::fmod(yaw + deltaX * DegreesPerPixel, 360.0f);
        if (yaw < 0.0f) yaw += 360.0f;
    }

    std::array<float, 3> Euler() const { return {0.0f, yaw, 0.0f}; }
};
} // namespace VL::EditorUi
