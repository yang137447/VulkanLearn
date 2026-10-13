#pragma once

#include <cstdint>
#include <string>

namespace VL
{

class World;

enum class EditorActorProperty : uint8_t
{
    Position, Rotation, Scale, Color, Intensity, OuterCone, InnerCone,
    AreaSize, Normal, CameraFov, CameraNear, CameraFar, CastShadows
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

// 取景只修改独立观察相机；稳定身份与代数拒绝迟到事件，网格合并所有 section 的世界包围盒。
bool FrameEditorActor(World& world, uint64_t generation, const std::string& identity);

} // namespace VL
