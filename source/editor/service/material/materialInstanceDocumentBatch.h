#pragma once

// 文件职责：MI 文档的场景引用解析与批量命令执行。
// ExecuteBatch 在隔离的 proposed 副本上按命令序列推导 working 状态，只有
// ValidateWorkingState 通过后才提交；save 命令走原来的原子保存与 digest
// 冲突语义。实现位于 materialInstanceDocumentBatch.cpp；公共 API 仍是
// MaterialInstanceDocumentService 的成员，签名与行为不变。

#include "editor/service/material/materialInstanceDocumentInternal.h"

namespace VL::Editor
{

class MaterialInstanceDocumentService;

} // namespace VL::Editor
