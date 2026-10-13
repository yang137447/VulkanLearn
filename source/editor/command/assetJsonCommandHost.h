#pragma once

#include <string_view>
#include "editor/command/editorCommandBus.h"
#include "editor/service/assetCommandExecutor.h"

namespace VL::Editor
{
// headless JSONL 会话拥有唯一文档与 bus；复用 Runtime 的 executor，不实现第二套写盘逻辑。
class AssetJsonCommandHost
{
public:
    explicit AssetJsonCommandHost(const std::filesystem::path& root)
        : scenes(root), executor(root, scenes) {}
    nlohmann::json Process(std::string_view request);
private:
    SceneDocumentService scenes;
    AssetCommandExecutor executor;
    EditorCommandBus bus;
};
} // namespace VL::Editor
