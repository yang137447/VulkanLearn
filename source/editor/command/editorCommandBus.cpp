#include "editor/command/editorCommandBus.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace VL
{

EditorCommandBus::EditorCommandBus(std::size_t maxResultCountValue)
    : maxResultCount(maxResultCountValue == 0 ? 1 : maxResultCountValue)
{
}

EditorCommandResult EditorCommandBus::MakeRejectedResult(
    EditorCommandId commandId,
    EditorErrorCode errorCode,
    std::string message) const
{
    EditorCommandResult result;
    result.commandId = commandId;
    result.status = EditorCommandStatus::Rejected;
    result.errorCode = errorCode;
    result.message = std::move(message);
    return result;
}

EditorCommandSubmission EditorCommandBus::Submit(EditorCommandEnvelope command)
{
    return SubmitImpl(std::move(command), false);
}

EditorCommandSubmission EditorCommandBus::SubmitInternal(
    EditorCommandEnvelope command)
{
    return SubmitImpl(std::move(command), true);
}

EditorCommandSubmission EditorCommandBus::SubmitImpl(
    EditorCommandEnvelope command,
    bool internalCommand)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (command.commandId == 0)
    {
        if (internalCommand)
        {
            command.commandId = nextInternalCommandId--;
            if (nextInternalCommandId == 0)
            {
                nextInternalCommandId =
                    std::numeric_limits<EditorCommandId>::max();
            }
        }
        else
        {
            command.commandId = nextCommandId++;
            if (nextCommandId == 0) nextCommandId = 1;
        }
    }
    else if (!internalCommand && command.commandId >= nextCommandId &&
        command.commandId < std::numeric_limits<EditorCommandId>::max())
    {
        nextCommandId = command.commandId + 1;
    }

    const auto existing = results.find(command.commandId);
    if (existing != results.end())
        return {command.commandId, EditorCommandAdmission::Duplicate, existing->second};

    if (const auto error = ValidateEditorCommandEnvelope(command))
    {
        EditorCommandResult result = MakeRejectedResult(command.commandId, *error, std::string(GetEditorErrorCodeName(*error)));
        results.emplace(command.commandId, result);
        completedResultOrder.push_back(command.commandId);
        PruneResultsLocked();
        return {command.commandId, EditorCommandAdmission::Rejected, std::move(result)};
    }

    if (command.expectedDocumentRevision.has_value())
    {
        const auto path = GetEditorCommandAssetPath(command);
        if (path.has_value())
        {
            const auto revision = documentRevisions.find(*path);
            if (revision != documentRevisions.end() && revision->second != *command.expectedDocumentRevision)
            {
                EditorCommandResult result = MakeRejectedResult(command.commandId, EditorErrorCode::StaleDocumentRevision, "expected document revision does not match the bus revision");
                result.documentRevision = revision->second;
                results.emplace(command.commandId, result);
                completedResultOrder.push_back(command.commandId);
                PruneResultsLocked();
                return {command.commandId, EditorCommandAdmission::Rejected, std::move(result)};
            }
        }
    }

    EditorCommandResult accepted;
    accepted.commandId = command.commandId;
    accepted.status = EditorCommandStatus::Accepted;
    accepted.message = "command accepted";
    results.emplace(command.commandId, accepted);
    pendingCommands.push_back(std::move(command));
    return {accepted.commandId, EditorCommandAdmission::Queued, std::move(accepted)};
}

EditorCommandSubmission EditorCommandBus::Queue(EditorCommandEnvelope command)
{
    return Submit(std::move(command));
}

std::vector<EditorCommandEnvelope> EditorCommandBus::Drain(std::size_t maxCount)
{
    std::lock_guard<std::mutex> lock(mutex);
    const std::size_t count = maxCount == 0 ? pendingCommands.size() : std::min(maxCount, pendingCommands.size());
    std::vector<EditorCommandEnvelope> commands;
    commands.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        commands.push_back(std::move(pendingCommands.front()));
        pendingCommands.pop_front();
        auto result = results.find(commands.back().commandId);
        if (result != results.end() && result->second.status == EditorCommandStatus::Accepted)
        {
            result->second.status = EditorCommandStatus::Running;
            result->second.message = "command running";
        }
    }
    return commands;
}

bool EditorCommandBus::MarkRunning(EditorCommandId commandId)
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto result = results.find(commandId);
    if (result == results.end() || IsFinalEditorCommandStatus(result->second.status)) return false;
    result->second.status = EditorCommandStatus::Running;
    result->second.message = "command running";
    return true;
}

bool EditorCommandBus::PublishResult(EditorCommandResult result)
{
    if (result.commandId == 0 || result.protocolVersion != kEditorCommandProtocolVersion || !IsFinalEditorCommandStatus(result.status)) return false;
    std::lock_guard<std::mutex> lock(mutex);
    const auto existing = results.find(result.commandId);
    if (existing == results.end() || IsFinalEditorCommandStatus(existing->second.status)) return false;
    existing->second = std::move(result);
    completedResultOrder.push_back(existing->first);
    PruneResultsLocked();
    return true;
}

std::optional<EditorCommandResult> EditorCommandBus::GetResult(EditorCommandId commandId) const
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto result = results.find(commandId);
    if (result == results.end()) return std::nullopt;
    return result->second;
}

std::optional<EditorCommandResult> EditorCommandBus::GetEditorCommandResult(EditorCommandId commandId) const
{
    return GetResult(commandId);
}

void EditorCommandBus::SetDocumentRevision(std::string assetPath, EditorDocumentRevision revision)
{
    if (assetPath.empty()) return;
    std::lock_guard<std::mutex> lock(mutex);
    documentRevisions[std::move(assetPath)] = revision;
}

std::optional<EditorDocumentRevision> EditorCommandBus::GetDocumentRevision(const std::string& assetPath) const
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto revision = documentRevisions.find(assetPath);
    if (revision == documentRevisions.end()) return std::nullopt;
    return revision->second;
}

std::size_t EditorCommandBus::PendingCount() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return pendingCommands.size();
}

void EditorCommandBus::PruneResultsLocked()
{
    while (completedResultOrder.size() > maxResultCount)
    {
        const EditorCommandId commandId = completedResultOrder.front();
        completedResultOrder.pop_front();
        const auto result = results.find(commandId);
        if (result != results.end() && IsFinalEditorCommandStatus(result->second.status)) results.erase(result);
    }
}

} // namespace VL
