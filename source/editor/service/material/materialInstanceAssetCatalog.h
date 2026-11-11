#pragma once

// 文件职责：MI 资源目录与编辑器快照。
// 覆盖已打开文档列表、T_* 纹理探测/枚举、MI_* 资源分页列表、编辑器快照
// 组装与引用上下文读取。实现位于 materialInstanceAssetCatalog.cpp；
// 公共 API 仍是 MaterialInstanceDocumentService 的成员，签名与行为不变。

#include "editor/service/material/materialInstanceDocumentInternal.h"

namespace VL::Editor
{

class MaterialInstanceDocumentService;

} // namespace VL::Editor
