#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace VL
{

class World;

enum class EditorActorProperty : uint8_t
{
    Position, Rotation, Scale, Color, Intensity, OuterCone, InnerCone,
    AreaSize, Normal, CameraFov, CameraNear, CameraFar, CastShadows,
    CameraExposureMode, CameraEv100, CameraExposureCompensation,
    CameraAutoMinEv100, CameraAutoMaxEv100, CameraAutoSpeedUp, CameraAutoSpeedDown,
    CameraAutoTargetGray,
    // 完整 TRS 一次提交：position/rotation/scale 三个数组必须同时有效。
    // 追加在末尾，避免已有数值枚举在 UI/命令序列化中重编号。
    Transform,
    // 场景全局 wind 对象：方向、强度、阵风参数。阵风开启由“写入任一非零阵风参数”
    // 表达，不额外引入布尔控件；全零时运行期等效关闭。
    WindDirection, WindStrength, WindGustAmplitude, WindGustFrequency, WindGustTurbulence
};

// UI 提交编辑意图；单分量路径由 GT 从当前对象读取其它分量，避免同帧连续
// 输入互相覆盖。Transform 路径携带完整 TRS，保证一次拖拽只产生一笔 Scene
// 历史。preview 只表示"实时写 live World"，命令执行端不得写入 Scene 草稿。
// 代数阻止旧关卡事件修改新关卡同名对象。
struct EditorActorPropertyEdit
{
    uint64_t worldGeneration = 0;
    std::string identity;
    EditorActorProperty property = EditorActorProperty::Position;
    uint32_t component = 0;
    float value = 0.0f;
    std::array<float, 3> transformPosition{0, 0, 0};
    std::array<float, 3> transformRotation{0, 0, 0};
    std::array<float, 3> transformScale{1, 1, 1};
    // Gizmo 拖动中的每帧预览：World 立即变化，但 Scene 文档与撤销历史保持不动。
    bool preview = false;
};

// Runtime-facing name for the neutral edit DTO; the editor keeps the original
// name as an alias so existing panels and command codecs compile unchanged.
using ActorPropertyEdit = EditorActorPropertyEdit;

// 命令边界校验数值、类型、对象与代数，成功后只修改 GT World，不接触文件或 GPU。
bool ApplyEditorActorProperty(World& world, const EditorActorPropertyEdit& edit,
    std::string& error);

// 曝光属性与 CameraExposure/场景 JSON 共用具名字段；非曝光属性返回 nullptr。
// UI 校验、GT 编辑与稀疏保存使用同一映射，不维护第二套曝光公式或范围。
inline const char* CameraExposureField(EditorActorProperty property)
{
    using P = EditorActorProperty;
    switch (property)
    {
    case P::CameraExposureMode: return "mode";
    case P::CameraEv100: return "ev100";
    case P::CameraExposureCompensation: return "compensation";
    case P::CameraAutoMinEv100: return "autoMinEv100";
    case P::CameraAutoMaxEv100: return "autoMaxEv100";
    case P::CameraAutoSpeedUp: return "autoSpeedUp";
    case P::CameraAutoSpeedDown: return "autoSpeedDown";
    case P::CameraAutoTargetGray: return "autoTargetGray";
    default: return nullptr;
    }
}

// 取景只修改独立观察相机；稳定身份与代数拒绝迟到事件，网格合并所有 section 的世界包围盒。
bool FrameEditorActor(World& world, uint64_t generation, const std::string& identity);

} // namespace VL
