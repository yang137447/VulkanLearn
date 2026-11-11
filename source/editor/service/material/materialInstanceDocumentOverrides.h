#pragma once

// 文件职责：MI 文档的参数/纹理/渲染状态 override 写路径。
// 包含 set/clear/reset/revert/reload；每次写入都先在 proposed 副本上校验，
// 失败时不改变 working 状态。实现位于 materialInstanceDocumentOverrides.cpp；
// 公共 API 仍是 MaterialInstanceDocumentService 的成员，签名与行为不变。

#include "editor/service/material/materialInstanceDocumentInternal.h"

namespace VL::Editor
{

class MaterialInstanceDocumentService;

} // namespace VL::Editor
