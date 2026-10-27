#pragma once

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
    // 场景全局 wind 对象：方向、强度、阵风参数。阵风开启由“写入任一非零阵风参数”
    // 表达，不额外引入布尔控件；全零时运行期等效关闭。
    WindDirection, WindStrength, WindGustAmplitude, WindGustFrequency, WindGustTurbulence
};

// UI 只提交单个分量的编辑意图；GT 执行端从当前对象读取其它分量，
// 避免同帧连续输入用旧快照覆盖彼此。代数阻止旧关卡事件修改新关卡同名对象。
struct EditorActorPropertyEdit
{
    uint64_t worldGeneration = 0;
    std::string identity;
    EditorActorProperty property = EditorActorProperty::Position;
    uint32_t component = 0;
    float value = 0.0f;
};

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
