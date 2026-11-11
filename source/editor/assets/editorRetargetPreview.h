#pragma once

#include "editorJsonAssetTypes.h"
#include <nlohmann/json_fwd.hpp>

namespace VL::Editor
{
// 只消费 worker 已读取的 JSON；使用生产解析器和 CompiledRetarget，
// 返回不可变展示值，不读文件、不创建几何或修改活动 World。非法资产抛异常。
EditorUi::EditorRetargetPreview BuildEditorRetargetPreview(
    const nlohmann::json& profile, const nlohmann::json& source,
    const nlohmann::json& target);
}
