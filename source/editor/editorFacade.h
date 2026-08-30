#pragma once

// 这是 editor 与宿主引擎之间的最小值语义边界；它只转发 CommandBus，
// 不持有 World、MaterialInstance、renderer 或 Vulkan handle。

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "editor/command/editorCommandBus.h"

namespace VL
{

class EditorFacade
{
public:
    explicit EditorFacade(std::size_t maxResultCount = 256)
        : commandBus(maxResultCount)
    {
    }

    EditorCommandSubmission Submit(EditorCommandEnvelope command)
    {
        return commandBus.Submit(std::move(command));
    }

    std::vector<EditorCommandEnvelope> Drain(std::size_t maxCount = 0)
    {
        return commandBus.Drain(maxCount);
    }

    bool PublishResult(EditorCommandResult result)
    {
        return commandBus.PublishResult(std::move(result));
    }

    std::optional<EditorCommandResult> GetResult(EditorCommandId commandId) const
    {
        return commandBus.GetResult(commandId);
    }

    void SetDocumentRevision(
        std::string assetPath,
        EditorDocumentRevision revision)
    {
        commandBus.SetDocumentRevision(std::move(assetPath), revision);
    }

    std::optional<EditorDocumentRevision> GetDocumentRevision(
        const std::string& assetPath) const
    {
        return commandBus.GetDocumentRevision(assetPath);
    }

    std::size_t PendingCount() const
    {
        return commandBus.PendingCount();
    }

    EditorCommandBus& CommandBus() noexcept
    {
        return commandBus;
    }

    const EditorCommandBus& CommandBus() const noexcept
    {
        return commandBus;
    }

private:
    EditorCommandBus commandBus;
};

} // namespace VL
