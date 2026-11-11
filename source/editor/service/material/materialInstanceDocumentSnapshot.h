#pragma once

// 文件职责：MI 文档快照与草稿序列化。
// MakeDocumentSnapshot 负责参数/纹理/渲染状态快照，BuildCandidateJson 与
// BuildEffective* 负责草稿 JSON；这里只读 Document 状态，不改变 dirty/revision。
// 实现位于 materialInstanceDocumentSnapshot.cpp；公共 API 仍是
// MaterialInstanceDocumentService 的成员，签名与行为不变。

#include "editor/service/material/materialInstanceDocumentInternal.h"

namespace VL::Editor
{

class MaterialInstanceDocumentService;

} // namespace VL::Editor
