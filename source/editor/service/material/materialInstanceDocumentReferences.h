#pragma once

// 文件职责：导航来源到 MaterialEditorReferenceSnapshot 的转换与去重判定。
// 原先位于 materialInstanceDocumentService.cpp 的匿名 namespace；因为打开文档
// （facade）、场景解析（batch）与目录快照三个切片都要用，提升为内联实现。
// 行为与此前逐字一致：selector 仍拼接可选 section，去重仍比较全部字段。

#include "editor/service/materialInstanceDocumentService.h"

#include <string>
#include <utility>

namespace VL::Editor::MaterialDocumentDetail
{

inline MaterialEditorReferenceSnapshot MakeReferenceSnapshot(
    const EditorNavigationOrigin& origin,
    std::string materialInstancePath)
{
    MaterialEditorReferenceSnapshot result;
    result.sceneIdentity = origin.sceneIdentity;
    result.objectIdentity = origin.objectIdentity;
    result.selector = origin.slot;
    if (origin.section.has_value())
    {
        result.selector += "[" + std::to_string(*origin.section) + "]";
    }
    result.materialInstancePath = std::move(materialInstancePath);
    return result;
}

inline bool SameReference(
    const MaterialEditorReferenceSnapshot& left,
    const MaterialEditorReferenceSnapshot& right)
{
    return left.sceneIdentity == right.sceneIdentity &&
           left.objectIdentity == right.objectIdentity &&
           left.sourceAssetPath == right.sourceAssetPath &&
           left.selector == right.selector &&
           left.materialInstancePath == right.materialInstancePath;
}

} // namespace VL::Editor::MaterialDocumentDetail
