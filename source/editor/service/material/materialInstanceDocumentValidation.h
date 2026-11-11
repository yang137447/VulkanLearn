#pragma once

// 文件职责：MI 草稿的托管字段、纹理引用、working 状态与参数范围校验。
// 校验错误码、消息与 JSON 键顺序保持不变。

#include "editor/service/material/materialInstanceDocumentInternal.h"

#include <string_view>

namespace VL::Editor::MaterialDocumentDetail
{

void ValidateEditorParameterRange(
    const Json& descriptor,
    const EditorMaterialParameterValue& value,
    std::string_view parameterName);

} // namespace VL::Editor::MaterialDocumentDetail
