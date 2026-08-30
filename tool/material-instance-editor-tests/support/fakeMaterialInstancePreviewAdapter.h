#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "editor/preview/materialInstancePreviewAdapter.h"

namespace material_instance_editor_test
{

class FakeMaterialInstancePreviewAdapter final
    : public VL::Editor::Preview::IMaterialInstancePreviewAdapter
{
public:
    using AdapterCommand =
        VL::Editor::Preview::MaterialInstancePreviewAdapterCommand;
    using AdapterResult =
        VL::Editor::Preview::MaterialInstancePreviewAdapterResult;
    using OperationId = VL::Editor::Preview::PreviewOperationId;

    AdapterResult Execute(const AdapterCommand& command) override
    {
        executeCommands.push_back(command);
        AdapterResult result;
        if (!queuedExecuteResults.empty())
        {
            result = queuedExecuteResults.front();
            queuedExecuteResults.erase(queuedExecuteResults.begin());
        }
        else
        {
            result.status =
                VL::Editor::Preview::PreviewAdapterOperationStatus::Completed;
            result.message = "fake preview adapter completed";
            if (command.type ==
                VL::Editor::Preview::MaterialInstancePreviewCommandType::
                    ConnectMaterialInstancePreview)
            {
                result.runtimeResourceGeneration = 1;
            }
        }

        if (result.status ==
                VL::Editor::Preview::PreviewAdapterOperationStatus::Pending &&
            result.operationId == 0)
        {
            result.operationId = nextOperationId++;
        }
        return result;
    }

    AdapterResult Poll(OperationId operationId) override
    {
        pollOperationIds.push_back(operationId);
        AdapterResult result;
        if (!queuedPollResults.empty())
        {
            result = queuedPollResults.front();
            queuedPollResults.erase(queuedPollResults.begin());
        }
        else
        {
            result.status =
                VL::Editor::Preview::PreviewAdapterOperationStatus::Completed;
            result.message = "fake preview adapter poll completed";
        }
        if (result.operationId == 0 &&
            result.status ==
                VL::Editor::Preview::PreviewAdapterOperationStatus::Pending)
        {
            result.operationId = operationId;
        }
        return result;
    }

    void Cancel(OperationId operationId) noexcept override
    {
        canceledOperationIds.push_back(operationId);
    }

    AdapterResult Disconnect(uint64_t bridgeLiveGeneration) override
    {
        disconnectGenerations.push_back(bridgeLiveGeneration);
        AdapterResult result;
        result.status =
            VL::Editor::Preview::PreviewAdapterOperationStatus::Completed;
        result.message = "fake preview adapter disconnected";
        return result;
    }

    void QueueExecuteResult(AdapterResult result)
    {
        queuedExecuteResults.push_back(std::move(result));
    }

    void QueuePollResult(AdapterResult result)
    {
        queuedPollResults.push_back(std::move(result));
    }

    std::vector<AdapterCommand> executeCommands;
    std::vector<OperationId> pollOperationIds;
    std::vector<OperationId> canceledOperationIds;
    std::vector<uint64_t> disconnectGenerations;

private:
    OperationId nextOperationId = 1;
    std::vector<AdapterResult> queuedExecuteResults;
    std::vector<AdapterResult> queuedPollResults;
};

} // namespace material_instance_editor_test
