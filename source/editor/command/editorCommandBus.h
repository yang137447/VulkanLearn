#pragma once

#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor/command/editorCommand.h"

namespace VL
{

enum class EditorCommandAdmission
{
    Queued,
    Duplicate,
    Rejected
};

struct EditorCommandSubmission
{
    EditorCommandId commandId = 0;
    EditorCommandAdmission admission = EditorCommandAdmission::Rejected;
    EditorCommandResult result;
};

// EditorCommandBus 负责 producer 间的值语义排队、结果幂等和 revision 门禁。
// 它不执行资产读写，也不持有 MaterialInstance、Renderer 或 Vulkan 对象。
class EditorCommandBus
{
public:
    explicit EditorCommandBus(std::size_t maxResultCount = 256);

    EditorCommandSubmission Submit(EditorCommandEnvelope command);
    EditorCommandSubmission SubmitInternal(EditorCommandEnvelope command);
    EditorCommandSubmission Queue(EditorCommandEnvelope command);
    std::vector<EditorCommandEnvelope> Drain(std::size_t maxCount = 0);

    bool MarkRunning(EditorCommandId commandId);
    bool PublishResult(EditorCommandResult result);
    std::optional<EditorCommandResult> GetResult(EditorCommandId commandId) const;
    std::optional<EditorCommandResult> GetEditorCommandResult(
        EditorCommandId commandId) const;

    void SetDocumentRevision(
        std::string assetPath,
        EditorDocumentRevision revision);
    std::optional<EditorDocumentRevision> GetDocumentRevision(
        const std::string& assetPath) const;
    std::size_t PendingCount() const;

private:
    EditorCommandSubmission SubmitImpl(
        EditorCommandEnvelope command,
        bool internalCommand);
    EditorCommandResult MakeRejectedResult(
        EditorCommandId commandId,
        EditorErrorCode errorCode,
        std::string message) const;
    void PruneResultsLocked();

    mutable std::mutex mutex;
    EditorCommandId nextCommandId = 1;
    EditorCommandId nextInternalCommandId =
        std::numeric_limits<EditorCommandId>::max();
    std::size_t maxResultCount;
    std::deque<EditorCommandEnvelope> pendingCommands;
    std::unordered_map<EditorCommandId, EditorCommandResult> results;
    std::deque<EditorCommandId> completedResultOrder;
    std::unordered_map<std::string, EditorDocumentRevision> documentRevisions;
};

} // namespace VL
