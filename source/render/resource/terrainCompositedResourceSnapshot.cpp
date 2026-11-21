#include "render/resource/terrainCompositedResourceSnapshot.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace VL
{
TerrainCompositedResourceSnapshot::TerrainCompositedResourceSnapshot(
    uint64_t world, uint64_t value, std::vector<std::string> surfaces)
    : worldGeneration(world), revision(value), expectedRoots(surfaces.begin(), surfaces.end())
{
    if (!worldGeneration || !revision || surfaces.empty() || expectedRoots.size() != surfaces.size() ||
        expectedRoots.count(""))
        throw std::invalid_argument("World candidate needs distinct surfaces and nonzero identity");
}

void TerrainCompositedResourceSnapshot::RecordInputs()
{
    if (phase != TerrainCompositedSnapshotPhase::Prepared)
        throw std::logic_error("Candidate inputs already recorded");
    phase = TerrainCompositedSnapshotPhase::InputsRecorded;
}

void TerrainCompositedResourceSnapshot::SubmitInputs(uint64_t epoch)
{
    if (phase != TerrainCompositedSnapshotPhase::InputsRecorded || !epoch)
        throw std::logic_error("Candidate input submission requires a real epoch");
    inputEpoch = lastUseEpoch = epoch;
    phase = TerrainCompositedSnapshotPhase::InputsSubmitted;
}

void TerrainCompositedResourceSnapshot::AbortInputs()
{
    if (phase == TerrainCompositedSnapshotPhase::InputsRecorded)
        phase = TerrainCompositedSnapshotPhase::Prepared;
}

bool TerrainCompositedResourceSnapshot::InputsCompleted(uint64_t completedEpoch)
{
    if (phase == TerrainCompositedSnapshotPhase::InputsSubmitted && completedEpoch >= inputEpoch)
        phase = TerrainCompositedSnapshotPhase::Producing;
    return phase == TerrainCompositedSnapshotPhase::Producing;
}

bool TerrainCompositedResourceSnapshot::ObserveProduction(uint64_t value,
    const std::string& surfaceId, bool root, uint64_t epoch, uint32_t status, bool accepted)
{
    if (value != revision || !expectedRoots.count(surfaceId) ||
        phase != TerrainCompositedSnapshotPhase::Producing || epoch <= inputEpoch)
        return false;
    lastUseEpoch = std::max(lastUseEpoch, epoch);
    if (status || !accepted)
    {
        Fail(status ? "Candidate GPU status rejected: " + std::to_string(status) :
            "Candidate namespace/token rejected");
        return false;
    }
    if (root)
        completedRoots.insert(surfaceId);
    return true;
}

bool TerrainCompositedResourceSnapshot::ConfirmTables(uint64_t completedEpoch, bool runtimeReady)
{
    if (phase != TerrainCompositedSnapshotPhase::Producing || !runtimeReady ||
        completedRoots != expectedRoots || completedEpoch < lastUseEpoch)
        return false;
    phase = TerrainCompositedSnapshotPhase::Ready;
    return true;
}

void TerrainCompositedResourceSnapshot::Publish()
{
    if (phase != TerrainCompositedSnapshotPhase::Ready)
        throw std::logic_error("Candidate cannot switch before status and page-table fences");
    phase = TerrainCompositedSnapshotPhase::Published;
}

void TerrainCompositedResourceSnapshot::PublishCommittedNoThrow() noexcept
{
    if (phase == TerrainCompositedSnapshotPhase::Ready)
        phase = TerrainCompositedSnapshotPhase::Published;
}

void TerrainCompositedResourceSnapshot::Fail(std::string value)
{
    if (phase == TerrainCompositedSnapshotPhase::Published)
        throw std::logic_error("Published candidate cannot roll back");
    diagnostic = std::move(value);
    phase = TerrainCompositedSnapshotPhase::Failed;
}

void TerrainCompositedResourceSnapshot::Cancel()
{
    if (phase == TerrainCompositedSnapshotPhase::Published)
        throw std::logic_error("Published candidate cannot cancel");
    phase = TerrainCompositedSnapshotPhase::Cancelled;
}
}
