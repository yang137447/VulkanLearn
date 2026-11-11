#pragma once

// 文件职责：MI 文档的磁盘 JSON 读取、LoadDocumentFromDisk、外部修改检测
// 与原子保存。实现位于 materialInstanceDocumentIO.cpp；公共 API 仍是
// MaterialInstanceDocumentService 的成员，签名与行为不变。

#include "editor/service/material/materialInstanceDocumentInternal.h"

namespace VL::Editor
{

class MaterialInstanceDocumentService;

} // namespace VL::Editor
