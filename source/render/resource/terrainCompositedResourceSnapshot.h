#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace VL
{
enum class TerrainCompositedSnapshotPhase
{
    Prepared, InputsRecorded, InputsSubmitted, Producing, Ready, Published, Failed, Cancelled
};

class TerrainCompositedResourceSnapshot
{
public:
    TerrainCompositedResourceSnapshot(uint64_t worldGeneration, uint64_t revision,
        std::vector<std::string> surfaces);
    void RecordInputs();
    void SubmitInputs(uint64_t epoch);
    void AbortInputs();
    bool InputsCompleted(uint64_t completedEpoch);
    bool ObserveProduction(uint64_t revision, const std::string& surfaceId,
        bool root, uint64_t epoch, uint32_t status, bool accepted);
    bool ConfirmTables(uint64_t completedEpoch, bool runtimeReady);
    void Publish();
    void PublishCommittedNoThrow() noexcept;
    void Fail(std::string diagnostic);
    void Cancel();
    TerrainCompositedSnapshotPhase Phase() const { return phase; }
    uint64_t Revision() const { return revision; }
    uint64_t InputEpoch() const { return inputEpoch; }
    uint64_t LastUseEpoch() const { return lastUseEpoch; }
    size_t ExpectedRootCount() const noexcept { return expectedRoots.size(); }
    size_t CompletedRootCount() const noexcept { return completedRoots.size(); }
    const std::string& Diagnostic() const { return diagnostic; }

private:
    uint64_t worldGeneration;
    uint64_t revision;
    uint64_t inputEpoch = 0;
    uint64_t lastUseEpoch = 0;
    std::set<std::string> expectedRoots;
    std::set<std::string> completedRoots;
    TerrainCompositedSnapshotPhase phase = TerrainCompositedSnapshotPhase::Prepared;
    std::string diagnostic;
};
}
