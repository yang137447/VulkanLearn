#include "engine/runtimeTestHooks.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "commonFunction.h"
#include "engine/diagnosticsSubsystem.h"
#include "engine/testing/runtimeTestFixtures.h"
#include "engine/testing/runtimeValidationServices.h"
#include "render/resource/resourceRetireQueue.h"
#include "shader/build/atomicFile.h"

namespace VL
{
namespace
{

constexpr const char* EyeComputeSourceIdentity =
    "generator/eyeCausticLut.comp";

void RequireEyeComputeCondition(
    bool condition,
    const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void ValidateEyeComputeSnapshot(
    const RuntimeEyeValidationSnapshot& snapshot,
    const std::string& phaseName)
{
    RequireEyeComputeCondition(
        snapshot.captured && snapshot.hasEyeResources,
        phaseName + " did not capture active Eye resources");
    RequireEyeComputeCondition(
        !snapshot.sourceDigest.empty() &&
            !snapshot.artifactGenerationKey.empty(),
        phaseName + " has empty Eye source/artifact identity");
    RequireEyeComputeCondition(
        snapshot.eyeLutTextureIdentity != 0 &&
            snapshot.eyeLutTextureIdentity ==
                snapshot.boundEyeWorldTextureIdentity,
        phaseName + " did not refresh the bound World Eye LUT identity");
    RequireEyeComputeCondition(
        snapshot.forwardEyeLutBinding &&
            snapshot.deferredEyeLutBinding &&
            snapshot.forwardEyeInnerPassPresent &&
            snapshot.forwardEyeCorneaPassPresent,
        phaseName + " lost an Eye LUT descriptor route");
}

RuntimeValidationManualShaderReloadResult ExecuteEyeComputeReload(
    RuntimeValidationServices& validationServices)
{
    // 复用正式 coordinator 的 source -> plan -> candidate -> commit 链路；
    // 虽然适配器名称沿用 graphics 历史命名，batch 会真实包含 Compute participant。
    return validationServices.ExecuteManualGraphicsShaderReload(
        {EyeComputeSourceIdentity},
        false);
}

} // namespace

bool RuntimeTestHooks::BeginEyeComputeReloadTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning(
            "A runtime validation test is already running.");
        return false;
    }

    try
    {
        const std::filesystem::path resourceRoot(resourcePath);
        const std::filesystem::path scenePath =
            resourceRoot / "scenes" / "SC_eye_probe.json";
        const std::filesystem::path sourcePath =
            std::filesystem::path(CommonFunction::GetProjectPath()) /
            "shader" / "glsl" / "generator" / "eyeCausticLut.comp";
        RequireEyeComputeCondition(
            std::filesystem::is_regular_file(scenePath),
            "Eye Compute reload test requires SC_eye_probe.json");
        RequireEyeComputeCondition(
            std::filesystem::is_regular_file(sourcePath),
            "Eye Compute reload test cannot find eyeCausticLut.comp");

        eyeComputeReloadOriginalSource =
            RuntimeTestFixtures::ReadTextFileBytes(sourcePath);
        eyeComputeReloadCompatibleSource =
            RuntimeTestFixtures::ReplaceFirstOccurrence(
                eyeComputeReloadOriginalSource,
                "float frontLightWeight = 0.35 + 0.65 * elevation;",
                "float frontLightWeight = 0.35 + 0.65 * elevation * 0.9995;");
        eyeComputeReloadAbiIncompatibleSource =
            RuntimeTestFixtures::ReplaceFirstOccurrence(
                eyeComputeReloadOriginalSource,
                "layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;",
                "layout(local_size_x = 4, local_size_y = 8, local_size_z = 1) in;");
        RequireEyeComputeCondition(
            eyeComputeReloadCompatibleSource !=
                eyeComputeReloadOriginalSource,
            "Eye Compute compatible edit marker was not found");
        RequireEyeComputeCondition(
            eyeComputeReloadAbiIncompatibleSource !=
                eyeComputeReloadOriginalSource,
            "Eye Compute ABI edit marker was not found");

        eyeComputeReloadScenePath = scenePath.string();
        eyeComputeReloadSourcePath = sourcePath.string();
        eyeComputeReloadBaselineSourceDigest.clear();
        eyeComputeReloadBaselineArtifactGenerationKey.clear();
        eyeComputeReloadCompatibleSourceDigest.clear();
        eyeComputeReloadCompatibleArtifactGenerationKey.clear();
        eyeComputeReloadBaselineLutIdentity = 0;
        eyeComputeReloadCompatibleLutIdentity = 0;
        eyeComputeReloadBaselinePendingRetiredResources = 0;
        eyeComputeReloadMaxPendingRetiredResources = 0;
        eyeComputeReloadRetireDrainFramesRemaining = 0;
        waitingForEyeComputeReloadWorld = false;
        eyeComputeReloadTestPhase =
            EyeComputeReloadTestPhase::WaitWorldLoad;
        eyeComputeReloadTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Eye Compute reload test started: compatible LUT/package commit, "
            "ABI rollback, descriptor refresh and epoch retirement.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create Eye Compute reload test: ") +
            exception.what());
        CleanupEyeComputeReloadTestFixture();
        return false;
    }
}

void RuntimeTestHooks::UpdateEyeComputeReloadTest(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!eyeComputeReloadTestActive)
    {
        return;
    }

    if (eyeComputeReloadTestPhase ==
        EyeComputeReloadTestPhase::WaitRetireDrain)
    {
        const size_t pending =
            validationServices.GetPendingRetiredResourceCount();
        eyeComputeReloadMaxPendingRetiredResources = std::max(
            eyeComputeReloadMaxPendingRetiredResources,
            pending);
        if (pending <= eyeComputeReloadBaselinePendingRetiredResources)
        {
            if (eyeComputeReloadMaxPendingRetiredResources <=
                eyeComputeReloadBaselinePendingRetiredResources)
            {
                FailEyeComputeReloadTest(
                    "Eye Compute reload did not observe an epoch-retired "
                    "pipeline/LUT/package.",
                    diagnostics);
                return;
            }
            eyeComputeReloadTestActive = false;
            eyeComputeReloadTestPhase = EyeComputeReloadTestPhase::Idle;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            CleanupEyeComputeReloadTestFixture();
            diagnostics.ReportInfo(
                "Eye Compute reload test succeeded: compatible and restore "
                "commits replaced pipeline/LUT/package, ABI rejection retained "
                "the active package, descriptors refreshed, retirements drained.");
            return;
        }

        --eyeComputeReloadRetireDrainFramesRemaining;
        if (eyeComputeReloadRetireDrainFramesRemaining <= 0)
        {
            FailEyeComputeReloadTest(
                "Eye Compute reload timed out waiting for retired resources "
                "to drain. pending=" + std::to_string(pending),
                diagnostics);
        }
        return;
    }

    try
    {
        switch (eyeComputeReloadTestPhase)
        {
        case EyeComputeReloadTestPhase::ValidateWorld:
        {
            const RuntimeEyeValidationSnapshot snapshot =
                validationServices.CaptureEyeValidationSnapshot();
            ValidateEyeComputeSnapshot(snapshot, "Eye Compute baseline");
            eyeComputeReloadBaselineSourceDigest = snapshot.sourceDigest;
            eyeComputeReloadBaselineArtifactGenerationKey =
                snapshot.artifactGenerationKey;
            eyeComputeReloadBaselineLutIdentity =
                snapshot.eyeLutTextureIdentity;
            eyeComputeReloadBaselinePendingRetiredResources =
                validationServices.GetPendingRetiredResourceCount();
            eyeComputeReloadMaxPendingRetiredResources =
                eyeComputeReloadBaselinePendingRetiredResources;
            eyeComputeReloadTestPhase =
                EyeComputeReloadTestPhase::ApplyCompatible;
            diagnostics.ReportInfo(
                "Eye Compute baseline captured; applying ABI-compatible edit.");
            return;
        }
        case EyeComputeReloadTestPhase::ApplyCompatible:
        {
            WriteTextFileAtomically(
                eyeComputeReloadSourcePath,
                eyeComputeReloadCompatibleSource);
            const RuntimeValidationManualShaderReloadResult operation =
                ExecuteEyeComputeReload(validationServices);
            RequireEyeComputeCondition(
                operation.succeeded && operation.committed &&
                    operation.affectedBuildCount == 1 &&
                    operation.pipelinesCreated == 1 &&
                    operation.pipelinesRetired >= 1,
                "ABI-compatible Eye Compute reload did not commit one "
                "pipeline/resource participant: " + operation.failureMessage);
            eyeComputeReloadMaxPendingRetiredResources = std::max(
                eyeComputeReloadMaxPendingRetiredResources,
                validationServices.GetPendingRetiredResourceCount());
            eyeComputeReloadTestPhase =
                EyeComputeReloadTestPhase::CheckCompatible;
            return;
        }
        case EyeComputeReloadTestPhase::CheckCompatible:
        {
            const RuntimeEyeValidationSnapshot snapshot =
                validationServices.CaptureEyeValidationSnapshot();
            ValidateEyeComputeSnapshot(snapshot, "Eye Compute compatible commit");
            RequireEyeComputeCondition(
                snapshot.artifactGenerationKey !=
                        eyeComputeReloadBaselineArtifactGenerationKey &&
                    snapshot.sourceDigest !=
                        eyeComputeReloadBaselineSourceDigest &&
                    snapshot.eyeLutTextureIdentity !=
                        eyeComputeReloadBaselineLutIdentity,
                "ABI-compatible Eye Compute reload did not replace artifact, "
                "LUT and World-local package identity");
            eyeComputeReloadCompatibleSourceDigest = snapshot.sourceDigest;
            eyeComputeReloadCompatibleArtifactGenerationKey =
                snapshot.artifactGenerationKey;
            eyeComputeReloadCompatibleLutIdentity =
                snapshot.eyeLutTextureIdentity;
            eyeComputeReloadMaxPendingRetiredResources = std::max(
                eyeComputeReloadMaxPendingRetiredResources,
                validationServices.GetPendingRetiredResourceCount());
            eyeComputeReloadTestPhase =
                EyeComputeReloadTestPhase::ApplyAbiRejection;
            diagnostics.ReportInfo(
                "Eye Compute compatible commit passed; applying ABI-incompatible edit.");
            return;
        }
        case EyeComputeReloadTestPhase::ApplyAbiRejection:
        {
            WriteTextFileAtomically(
                eyeComputeReloadSourcePath,
                eyeComputeReloadAbiIncompatibleSource);
            const RuntimeValidationManualShaderReloadResult operation =
                ExecuteEyeComputeReload(validationServices);
            RequireEyeComputeCondition(
                !operation.succeeded,
                "ABI-incompatible Eye Compute edit unexpectedly committed");
            eyeComputeReloadTestPhase =
                EyeComputeReloadTestPhase::CheckAbiRejection;
            return;
        }
        case EyeComputeReloadTestPhase::CheckAbiRejection:
        {
            const RuntimeEyeValidationSnapshot snapshot =
                validationServices.CaptureEyeValidationSnapshot();
            ValidateEyeComputeSnapshot(snapshot, "Eye Compute ABI rejection");
            RequireEyeComputeCondition(
                snapshot.sourceDigest ==
                        eyeComputeReloadCompatibleSourceDigest &&
                    snapshot.artifactGenerationKey ==
                        eyeComputeReloadCompatibleArtifactGenerationKey &&
                    snapshot.eyeLutTextureIdentity ==
                        eyeComputeReloadCompatibleLutIdentity,
                "ABI rejection changed the active Eye pipeline/LUT/package");
            eyeComputeReloadTestPhase =
                EyeComputeReloadTestPhase::ApplyRestore;
            diagnostics.ReportInfo(
                "Eye Compute ABI rejection retained the active package; restoring source.");
            return;
        }
        case EyeComputeReloadTestPhase::ApplyRestore:
        {
            WriteTextFileAtomically(
                eyeComputeReloadSourcePath,
                eyeComputeReloadOriginalSource);
            const RuntimeValidationManualShaderReloadResult operation =
                ExecuteEyeComputeReload(validationServices);
            RequireEyeComputeCondition(
                operation.succeeded && operation.committed &&
                    operation.affectedBuildCount == 1 &&
                    operation.pipelinesCreated == 1 &&
                    operation.pipelinesRetired >= 1,
                "Eye Compute original source restore did not commit: " +
                    operation.failureMessage);
            eyeComputeReloadMaxPendingRetiredResources = std::max(
                eyeComputeReloadMaxPendingRetiredResources,
                validationServices.GetPendingRetiredResourceCount());
            eyeComputeReloadTestPhase =
                EyeComputeReloadTestPhase::CheckRestore;
            return;
        }
        case EyeComputeReloadTestPhase::CheckRestore:
        {
            const RuntimeEyeValidationSnapshot snapshot =
                validationServices.CaptureEyeValidationSnapshot();
            ValidateEyeComputeSnapshot(snapshot, "Eye Compute source restore");
            RequireEyeComputeCondition(
                snapshot.sourceDigest ==
                        eyeComputeReloadBaselineSourceDigest &&
                    snapshot.artifactGenerationKey ==
                        eyeComputeReloadBaselineArtifactGenerationKey &&
                    snapshot.eyeLutTextureIdentity !=
                        eyeComputeReloadCompatibleLutIdentity,
                "Eye Compute restore did not recover source identity with a "
                "new active LUT package");
            eyeComputeReloadMaxPendingRetiredResources = std::max(
                eyeComputeReloadMaxPendingRetiredResources,
                validationServices.GetPendingRetiredResourceCount());
            eyeComputeReloadRetireDrainFramesRemaining =
                RuntimeTestFixtures::RetireDrainFrameBudget;
            eyeComputeReloadTestPhase =
                EyeComputeReloadTestPhase::WaitRetireDrain;
            diagnostics.ReportInfo(
                "Eye Compute source restore passed; waiting for GPU epoch retirement drain.");
            return;
        }
        case EyeComputeReloadTestPhase::Idle:
        case EyeComputeReloadTestPhase::WaitWorldLoad:
        case EyeComputeReloadTestPhase::WaitRetireDrain:
            return;
        }
    }
    catch (const std::exception& exception)
    {
        FailEyeComputeReloadTest(exception.what(), diagnostics);
    }
}

void RuntimeTestHooks::FailEyeComputeReloadTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    eyeComputeReloadTestActive = false;
    waitingForEyeComputeReloadWorld = false;
    eyeComputeReloadTestPhase = EyeComputeReloadTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupEyeComputeReloadTestFixture();
    diagnostics.ReportError("Eye Compute reload test failed: " + message);
}

void RuntimeTestHooks::CleanupEyeComputeReloadTestFixture() noexcept
{
    if (!eyeComputeReloadSourcePath.empty() &&
        !eyeComputeReloadOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                eyeComputeReloadSourcePath,
                eyeComputeReloadOriginalSource);
        }
        catch (...)
        {
        }
    }
    eyeComputeReloadScenePath.clear();
    eyeComputeReloadSourcePath.clear();
    eyeComputeReloadOriginalSource.clear();
    eyeComputeReloadCompatibleSource.clear();
    eyeComputeReloadAbiIncompatibleSource.clear();
    eyeComputeReloadBaselineSourceDigest.clear();
    eyeComputeReloadBaselineArtifactGenerationKey.clear();
    eyeComputeReloadCompatibleSourceDigest.clear();
    eyeComputeReloadCompatibleArtifactGenerationKey.clear();
    eyeComputeReloadBaselineLutIdentity = 0;
    eyeComputeReloadCompatibleLutIdentity = 0;
    eyeComputeReloadBaselinePendingRetiredResources = 0;
    eyeComputeReloadMaxPendingRetiredResources = 0;
    eyeComputeReloadRetireDrainFramesRemaining = 0;
    waitingForEyeComputeReloadWorld = false;
    eyeComputeReloadTestPhase = EyeComputeReloadTestPhase::Idle;
}

} // namespace VL
