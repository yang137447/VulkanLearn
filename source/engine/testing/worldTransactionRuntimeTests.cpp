#include "engine/runtimeTestHooks.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "commonFunction.h"
#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeCommandExecutor.h"
#include "engine/testing/runtimeTestFixtures.h"
#include "engine/testing/runtimeValidationServices.h"
#include "materialInstance.h"
#include "material/generator/materialParameterIncludeGenerator.h"
#include "render/resource/resourceRetireQueue.h"
#include "shader/build/atomicFile.h"
#include "world/world.h"

namespace VL
{
using namespace RuntimeTestFixtures;
std::string
RuntimeTestHooks::CaptureWorldGraphRuntimeFingerprint(
    RuntimeValidationServices& validationServices,
    const std::string& primaryDefinitionPath,
    const std::string& batchDefinitionPath,
    bool includeFrameLifecycleDiagnostics,
    std::string* details) const
{
    return validationServices.CaptureWorldGraphRuntimeFingerprint(
        primaryDefinitionPath,
        batchDefinitionPath,
        includeFrameLifecycleDiagnostics,
        details);
}

bool RuntimeTestHooks::BeginWorldReloadStress(
    std::string scenePath,
    int reloadCount,
    const DiagnosticsSubsystem& diagnostics)
{
    if (scenePath.empty())
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning("World reload stress ignored because scene path is empty.");
        return false;
    }

    if (reloadCount <= 0)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning("World reload stress ignored because reload count must be positive.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    worldReloadStressScenePath = std::move(scenePath);
    totalWorldReloads = reloadCount;
    remainingWorldReloads = reloadCount;
    completedWorldReloads = 0;
    retireDrainFramesRemaining = 0;
    maxPendingRetiredResources = 0;
    waitingForWorldReloadResult = false;
    waitingForRetireDrain = false;
    worldReloadStressActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "World reload stress started: scene=" +
        worldReloadStressScenePath +
        ", count=" +
        std::to_string(totalWorldReloads));
    return true;
}

bool RuntimeTestHooks::BeginWorldReloadFailureRollbackTest(
    std::string scenePath,
    const DiagnosticsSubsystem& diagnostics,
    std::string expectedErrorCode)
{
    if (scenePath.empty())
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportWarning("World reload failure rollback test ignored because scene path is empty.");
        return false;
    }

    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning("A runtime validation test is already running.");
        return false;
    }

    failureRollbackScenePath = std::move(scenePath);
    failureRollbackExpectedErrorCode = std::move(expectedErrorCode);
    waitingForFailureRollbackResult = false;
    failureRollbackTestActive = true;
    runtimeTestStatus = RuntimeTestStatus::Running;

    diagnostics.ReportInfo(
        "World reload failure rollback test started: scene=" +
        failureRollbackScenePath);
    return true;
}

bool RuntimeTestHooks::BeginGeneratedMaterialFailureRollbackTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    try
    {
        generatedFailureFixtureDirectory.clear();
        const std::filesystem::path scenePath = CreateGeneratedMaterialFailureScene(resourcePath);
        generatedFailureFixtureDirectory = scenePath.parent_path().string();
        cleanupGeneratedFailureFixture = true;
        const bool started = BeginWorldReloadFailureRollbackTest(
            scenePath.string(),
            diagnostics,
            "Material.LoadFailed");
        if (!started)
        {
            CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
            cleanupGeneratedFailureFixture = false;
            generatedFailureFixtureDirectory.clear();
        }
        return started;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create generated material failure rollback fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
        cleanupGeneratedFailureFixture = false;
        generatedFailureFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginGeneratedMeshFailureRollbackTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    try
    {
        generatedFailureFixtureDirectory.clear();
        const std::filesystem::path scenePath = CreateGeneratedMeshFailureScene(resourcePath);
        generatedFailureFixtureDirectory = scenePath.parent_path().string();
        cleanupGeneratedFailureFixture = true;
        const bool started = BeginWorldReloadFailureRollbackTest(
            scenePath.string(),
            diagnostics,
            "Mesh.LoadFailed");
        if (!started)
        {
            CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
            cleanupGeneratedFailureFixture = false;
            generatedFailureFixtureDirectory.clear();
        }
        return started;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create generated mesh failure rollback fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
        cleanupGeneratedFailureFixture = false;
        generatedFailureFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginGeneratedTextureFailureRollbackTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    try
    {
        generatedFailureFixtureDirectory.clear();
        const std::filesystem::path scenePath = CreateGeneratedTextureFailureScene(resourcePath);
        generatedFailureFixtureDirectory = scenePath.parent_path().string();
        cleanupGeneratedFailureFixture = true;
        const bool started = BeginWorldReloadFailureRollbackTest(
            scenePath.string(),
            diagnostics,
            "Texture.LoadFailed");
        if (!started)
        {
            CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
            cleanupGeneratedFailureFixture = false;
            generatedFailureFixtureDirectory.clear();
        }
        return started;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create generated texture failure rollback fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
        cleanupGeneratedFailureFixture = false;
        generatedFailureFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginGeneratedHighLightReloadStress(
    const std::string& resourcePath,
    int reloadCount,
    const DiagnosticsSubsystem& diagnostics)
{
    try
    {
        generatedReloadStressFixtureDirectory.clear();
        const std::filesystem::path scenePath = CreateGeneratedHighLightStressScene(resourcePath);
        generatedReloadStressFixtureDirectory = scenePath.parent_path().string();
        cleanupGeneratedReloadStressFixture = true;

        const bool started = BeginWorldReloadStress(scenePath.string(), reloadCount, diagnostics);
        if (!started)
        {
            CleanupGeneratedRuntimeFixtureIfNeeded(
                cleanupGeneratedReloadStressFixture,
                generatedReloadStressFixtureDirectory,
                diagnostics);
        }
        return started;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create generated high-light reload stress fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixtureIfNeeded(
            cleanupGeneratedReloadStressFixture,
            generatedReloadStressFixtureDirectory,
            diagnostics);
        return false;
    }
}

bool RuntimeTestHooks::BeginWorldGraphTransactionTest(
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
        const std::filesystem::path sourcePath =
            std::filesystem::path(
                CommonFunction::GetProjectPath()) /
            "shader" / "glsl" / "runtimeTest" /
            "M_shaderReloadTest.json";
        const std::filesystem::path batchSourcePath =
            sourcePath.parent_path() /
            "M_shaderReloadBatchTest.json";
        const std::filesystem::path scenePath =
            CreateShaderReloadTestScene(resourcePath);

        worldGraphTransactionTestSourcePath =
            sourcePath.string();
        worldGraphTransactionTestBatchSourcePath =
            batchSourcePath.string();
        worldGraphTransactionTestOriginalSource =
            ReadTextFileBytes(sourcePath);
        nlohmann::json candidateJson =
            nlohmann::json::parse(
                worldGraphTransactionTestOriginalSource);
        candidateJson["parameters"][
            "u_worldGraphTransactionCandidate"] = {
                {"type", "float"},
                {"default", 0.8125}};
        worldGraphTransactionTestCandidateSource =
            candidateJson.dump(2) + "\n";
        worldGraphTransactionTestScenePath =
            scenePath.string();
        worldGraphTransactionTestHighLightScenePath.clear();
        worldGraphTransactionTestFixtureDirectory =
            scenePath.parent_path().string();
        worldGraphTransactionTestMonitorSuspended = false;
        worldGraphTransactionTestFramesUntilNextPhase = 0;
        worldGraphTransactionTestRetireDrainFramesRemaining = 0;
        worldGraphTransactionTestMaxPendingRetiredResources = 0;
        worldGraphTransactionTestNextBatchId = 1;
        worldGraphTransactionTestGenerationBeforeSuccess = 0;
        worldGraphTransactionTestBackendCountsBeforeSuccess = {};
        worldGraphTransactionTestImageResourceNamesBeforeSuccess.clear();
        worldGraphTransactionTestOldWorld.reset();
        worldGraphTransactionTestOldWorldPackage.reset();
        worldGraphTransactionTestOldGraphPackage.reset();
        worldGraphTransactionTestGraphReloadPackage.reset();
        worldGraphTransactionTestOldLightBuffer.reset();
        worldGraphTransactionTestOldMaterial.reset();
        worldGraphTransactionTestOldMaterialInstance.reset();
        worldGraphTransactionTestOldObjectResources.reset();
        worldGraphTransactionTestOldTexture.reset();
        worldGraphTransactionTestResourcesExpectedToExpire.clear();
        waitingForWorldGraphTransactionTestWorld = false;
        worldGraphTransactionTestPhase =
            WorldGraphTransactionTestPhase::WaitWorldLoad;
        worldGraphTransactionTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "World/graph transaction runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create World/graph transaction runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            worldGraphTransactionTestFixtureDirectory,
            diagnostics);
        worldGraphTransactionTestFixtureDirectory.clear();
        return false;
    }
}

void RuntimeTestHooks::FailWorldGraphTransactionTest(
    RuntimeValidationServices* validationServices,
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!worldGraphTransactionTestSourcePath.empty() &&
        !worldGraphTransactionTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
        worldGraphTransactionTestSourcePath,
        worldGraphTransactionTestOriginalSource);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    worldGraphTransactionTestSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    if (worldGraphTransactionTestMonitorSuspended &&
        validationServices)
    {
        validationServices->RefreshShaderMonitorBaseline({
            "runtimeTest/M_shaderReloadTest.json"});
        validationServices->SetShaderMonitorScanSuspended(false);
        worldGraphTransactionTestMonitorSuspended = false;
    }
    worldGraphTransactionTestSourcePath.clear();
    worldGraphTransactionTestOriginalSource.clear();
    worldGraphTransactionTestCandidateSource.clear();
    worldGraphTransactionTestHighLightScenePath.clear();
    worldGraphTransactionTestResourcesExpectedToExpire.clear();
    worldGraphTransactionTestFramesUntilNextPhase = 0;
    worldGraphTransactionTestActive = false;
    waitingForWorldGraphTransactionTestWorld = false;
    worldGraphTransactionTestPhase =
        WorldGraphTransactionTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        worldGraphTransactionTestFixtureDirectory,
        diagnostics);
    worldGraphTransactionTestFixtureDirectory.clear();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::ValidateWorldGraphTransactionFailure(
    RuntimeValidationServices& validationServices,
    const WorldGraphTransactionTestFaultInjection& injection,
    bool graphOnly,
    bool materialDefinitionTransaction,
    const std::string& phaseName,
    const std::string& expectedFailureText,
    const DiagnosticsSubsystem& diagnostics)
{
    std::string beforeDetails;
    const std::string beforeFingerprint =
        CaptureWorldGraphRuntimeFingerprint(
            validationServices,
            worldGraphTransactionTestSourcePath,
            worldGraphTransactionTestBatchSourcePath,
            true,
            &beforeDetails);
    const std::array<size_t, 9> beforeCounts =
        validationServices
            .CaptureBackendSnapshot()
            .identityCounts;

    validationServices.SetWorldGraphTransactionFaultInjection(
        injection);
    bool succeeded = false;
    std::string failureMessage;
    if (materialDefinitionTransaction)
    {
        const RuntimeResult<WorldHandle> result =
            validationServices
                .ExecuteMaterialDefinitionWorldGraphTransaction(
                    {"runtimeTest/M_shaderReloadTest.json"},
                    worldGraphTransactionTestNextBatchId++);
        succeeded = result.IsSuccess();
        if (result.IsFailure())
        {
            failureMessage =
                FormatRuntimeError(result.Error());
        }
    }
    else if (graphOnly)
    {
        const RuntimeResult<void> result =
            validationServices.ReloadRenderGraphResources();
        succeeded = result.IsSuccess();
        if (result.IsFailure())
        {
            failureMessage =
                FormatRuntimeError(result.Error());
        }
    }
    else
    {
        const RuntimeResult<WorldHandle> result =
            validationServices.ExecuteWorldGraphTransaction(
                validationServices
                    .GetActiveWorldHandle()
                    .scenePath);
        succeeded = result.IsSuccess();
        if (result.IsFailure())
        {
            failureMessage =
                FormatRuntimeError(result.Error());
        }
    }
    validationServices.SetWorldGraphTransactionFaultInjection({});

    if (succeeded)
    {
        throw std::runtime_error(
            phaseName +
            " unexpectedly committed");
    }
    if (failureMessage.find(expectedFailureText) ==
        std::string::npos)
    {
        throw std::runtime_error(
            phaseName +
            " reported an unexpected failure: " +
            failureMessage);
    }

    std::string afterDetails;
    const std::string afterFingerprint =
        CaptureWorldGraphRuntimeFingerprint(
            validationServices,
            worldGraphTransactionTestSourcePath,
            worldGraphTransactionTestBatchSourcePath,
            true,
            &afterDetails);
    if (afterFingerprint != beforeFingerprint)
    {
        size_t beforeOffset = 0;
        size_t afterOffset = 0;
        std::string firstDifference =
            "no differing field could be isolated";
        while (beforeOffset < beforeDetails.size() ||
               afterOffset < afterDetails.size())
        {
            const size_t beforeEnd =
                beforeDetails.find(';', beforeOffset);
            const size_t afterEnd =
                afterDetails.find(';', afterOffset);
            const std::string beforeField =
                beforeDetails.substr(
                    beforeOffset,
                    beforeEnd == std::string::npos
                        ? std::string::npos
                        : beforeEnd - beforeOffset);
            const std::string afterField =
                afterDetails.substr(
                    afterOffset,
                    afterEnd == std::string::npos
                        ? std::string::npos
                        : afterEnd - afterOffset);
            if (beforeField != afterField)
            {
                firstDifference =
                    "beforeField={" + beforeField +
                    "}, afterField={" + afterField + "}";
                break;
            }
            beforeOffset =
                beforeEnd == std::string::npos
                    ? beforeDetails.size()
                    : beforeEnd + 1;
            afterOffset =
                afterEnd == std::string::npos
                    ? afterDetails.size()
                    : afterEnd + 1;
        }
        throw std::runtime_error(
            phaseName +
            " changed the live World/graph/runtime fingerprint: before=" +
            beforeFingerprint +
            ", after=" +
            afterFingerprint +
            ", firstDifference=" +
            firstDifference);
    }

    const std::array<size_t, 9> afterCounts =
        validationServices
            .CaptureBackendSnapshot()
            .identityCounts;
    if (afterCounts != beforeCounts)
    {
        throw std::runtime_error(
            phaseName +
            " leaked or destroyed renderer identities: before={" +
            FormatBackendIdentityCounts(beforeCounts) +
            "}, after={" +
            FormatBackendIdentityCounts(afterCounts) +
            "}");
    }

    diagnostics.ReportInfo(
        "World/graph transaction rollback passed " +
        phaseName +
        ": fingerprint=" +
        afterFingerprint +
        ", backend={" +
        FormatBackendIdentityCounts(afterCounts) +
        "}, failure=" +
        failureMessage +
        ".");
}

void RuntimeTestHooks::
AdvanceWorldGraphTransactionTestAfterRenderedFrames(
    WorldGraphTransactionTestPhase nextPhase) noexcept
{
    worldGraphTransactionTestPhase = nextPhase;
    worldGraphTransactionTestFramesUntilNextPhase = 3;
}

void RuntimeTestHooks::UpdateWorldGraphTransactionTest(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!worldGraphTransactionTestActive)
    {
        return;
    }

    try
    {
        if (worldGraphTransactionTestFramesUntilNextPhase > 0)
        {
            --worldGraphTransactionTestFramesUntilNextPhase;
            return;
        }

        if (worldGraphTransactionTestPhase ==
            WorldGraphTransactionTestPhase::WaitRetireDrain)
        {
            ResourceRetireQueue& retireQueue =
                ResourceRetireQueue::GetInstance();
            const size_t pending =
                retireQueue.GetPendingCount();
            worldGraphTransactionTestMaxPendingRetiredResources =
                std::max(
                    worldGraphTransactionTestMaxPendingRetiredResources,
                    pending);
            if (pending == 0)
            {
                if (worldGraphTransactionTestMaxPendingRetiredResources == 0)
                {
                    throw std::runtime_error(
                        "successful transactions did not enqueue epoch-retired packages");
                }
                if (!worldGraphTransactionTestOldWorld.expired() ||
                    !worldGraphTransactionTestOldWorldPackage.expired() ||
                    !worldGraphTransactionTestOldGraphPackage.expired() ||
                    !worldGraphTransactionTestGraphReloadPackage.expired() ||
                    !worldGraphTransactionTestOldLightBuffer.expired() ||
                    !worldGraphTransactionTestOldMaterial.expired() ||
                    !worldGraphTransactionTestOldMaterialInstance.expired() ||
                    !worldGraphTransactionTestOldObjectResources.expired())
                {
                    throw std::runtime_error(
                        "retire queue drained while an old World/resource/graph owner remained live");
                }
                for (const std::weak_ptr<void>& retired :
                     worldGraphTransactionTestResourcesExpectedToExpire)
                {
                    if (!retired.expired())
                    {
                        throw std::runtime_error(
                            "retire queue drained while a tracked transaction resource remained live");
                    }
                }

                const std::shared_ptr<void> retainedTexture =
                    worldGraphTransactionTestOldTexture.lock();
                const std::shared_ptr<Texture> activeTexture =
                    FindShaderReloadTestPrimaryTexture();
                if (!retainedTexture || !activeTexture ||
                    retainedTexture.get() != activeTexture.get())
                {
                    throw std::runtime_error(
                        "compatible MaterialInstance texture identity was not retained across World/M_ transactions");
                }

                const std::array<size_t, 9> finalCounts =
                    validationServices
                        .CaptureBackendSnapshot()
                        .identityCounts;
                if (finalCounts !=
                    worldGraphTransactionTestBackendCountsBeforeSuccess)
                {
                    const std::vector<std::string> finalNames =
                        validationServices
                            .CaptureBackendSnapshot()
                            .imageResourceNames;
                    throw std::runtime_error(
                        "renderer identity counts did not return to the pre-commit baseline: baseline={" +
                        FormatBackendIdentityCounts(
                            worldGraphTransactionTestBackendCountsBeforeSuccess) +
                        "}, final={" +
                            FormatBackendIdentityCounts(finalCounts) +
                            "}, imageResourceDiff={" +
                            FormatImageResourceDebugNameDifference(
                                worldGraphTransactionTestImageResourceNamesBeforeSuccess,
                                finalNames) +
                            "}");
                }
                diagnostics.ReportInfo(
                    "World/graph transaction epoch retirement drain passed: "
                    "maxRetirePending=" +
                    std::to_string(
                        worldGraphTransactionTestMaxPendingRetiredResources) +
                    ", finalBackend={" +
                    FormatBackendIdentityCounts(finalCounts) +
                    "}.");
                worldGraphTransactionTestPhase =
                    WorldGraphTransactionTestPhase::ResizeFatalFailure;
                return;
            }

            --worldGraphTransactionTestRetireDrainFramesRemaining;
            if (worldGraphTransactionTestRetireDrainFramesRemaining <= 0)
            {
                throw std::runtime_error(
                    "timed out waiting for retired World/graph packages to drain; pending=" +
                    std::to_string(pending));
            }
            return;
        }

        WorldGraphTransactionTestFaultInjection injection;
        switch (worldGraphTransactionTestPhase)
        {
        case WorldGraphTransactionTestPhase::GraphResourceFailure:
            injection.failGraphResourceCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                false,
                false,
                "World transaction graph resource creation item 2",
                "Injected render graph resource creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::RenderPassFailure);
            return;

        case WorldGraphTransactionTestPhase::RenderPassFailure:
            injection.failRenderPassCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                false,
                false,
                "World transaction render pass creation item 2",
                "Injected render pass creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::FramebufferFailure);
            return;

        case WorldGraphTransactionTestPhase::FramebufferFailure:
            injection.failFramebufferCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                false,
                false,
                "World transaction framebuffer creation item 2",
                "Injected framebuffer creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::PassMaterialContractFailure);
            return;

        case WorldGraphTransactionTestPhase::PassMaterialContractFailure:
            injection.failPassMaterialContract = true;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                false,
                false,
                "World transaction pass material contract precheck",
                "Injected pass material contract failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::DescriptorFailure);
            return;

        case WorldGraphTransactionTestPhase::DescriptorFailure:
            injection.failDescriptorCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                false,
                false,
                "World transaction descriptor creation item 2",
                "Injected render graph descriptor creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::CandidateWorldFailure);
            return;

        case WorldGraphTransactionTestPhase::CandidateWorldFailure:
            injection.failAfterCandidateWorldBuilt = true;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                false,
                false,
                "candidate World built before commit",
                "Injected failure after candidate World build",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::ViewTargetFailure);
            return;

        case WorldGraphTransactionTestPhase::ViewTargetFailure:
            injection.failViewTargetPrecheck = true;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                false,
                false,
                "view target precheck",
                "Candidate World has no controller view target",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::RuntimeBindingFailure);
            return;

        case WorldGraphTransactionTestPhase::RuntimeBindingFailure:
            injection.failAfterRuntimeBindingPrepared = true;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                false,
                false,
                "World runtime binding prepared before commit",
                "Injected failure after runtime binding prepare",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::BeforeCommitFailure);
            return;

        case WorldGraphTransactionTestPhase::BeforeCommitFailure:
            injection.failBeforeCommit = true;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                false,
                false,
                "all prepare complete before commit",
                "Injected failure after all World/graph/runtime prepare steps",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::GraphOnlyResourceFailure);
            return;

        case WorldGraphTransactionTestPhase::GraphOnlyResourceFailure:
            injection.failGraphResourceCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                true,
                false,
                "graph-only resource creation item 2",
                "Injected render graph resource creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::GraphOnlyDescriptorFailure);
            return;

        case WorldGraphTransactionTestPhase::GraphOnlyDescriptorFailure:
            injection.failDescriptorCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                true,
                false,
                "graph-only descriptor creation item 2",
                "Injected render graph descriptor creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::GraphOnlyBeforeCommitFailure);
            return;

        case WorldGraphTransactionTestPhase::GraphOnlyBeforeCommitFailure:
            injection.failBeforeCommit = true;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                true,
                false,
                "graph-only all prepare complete before commit",
                "Injected failure after RenderGraph reload prepare",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::SuccessCommit);
            return;

        case WorldGraphTransactionTestPhase::SuccessCommit:
        {
            const RuntimeValidationOwnerSnapshot beforeOwners =
                validationServices.CaptureOwnerSnapshot();
            const RuntimeValidationWorldPackageIdentities beforePackage =
                validationServices.CaptureWorldPackageIdentities();
            const RuntimeValidationBackendSnapshot beforeBackend =
                validationServices.CaptureBackendSnapshot();
            const uint64_t oldGeneration =
                beforeOwners.world.generation;
            const size_t oldLightCapacity =
                beforeOwners.lightCapacity;
            const std::vector<uint64_t> oldLightHandles =
                beforeOwners.lightBufferIds;
            worldGraphTransactionTestGenerationBeforeSuccess =
                oldGeneration;
            worldGraphTransactionTestBackendCountsBeforeSuccess =
                beforeBackend.identityCounts;
            worldGraphTransactionTestImageResourceNamesBeforeSuccess =
                beforeBackend.imageResourceNames;
            worldGraphTransactionTestOldWorld =
                beforePackage.world;
            worldGraphTransactionTestOldWorldPackage =
                beforePackage.worldResources;
            worldGraphTransactionTestOldMaterial =
                beforePackage.material;
            worldGraphTransactionTestOldMaterialInstance =
                beforePackage.materialInstance;
            worldGraphTransactionTestOldObjectResources =
                beforePackage.objectResources;
            if (beforePackage.world.expired() ||
                beforePackage.worldResources.expired() ||
                worldGraphTransactionTestOldMaterial.expired() ||
                worldGraphTransactionTestOldMaterialInstance.expired() ||
                worldGraphTransactionTestOldObjectResources.expired() ||
                beforePackage.primaryTexture.expired())
            {
                throw std::runtime_error(
                    "high-light success phase could not capture the old runtime package");
            }
            worldGraphTransactionTestOldTexture =
                beforePackage.primaryTexture;

            const std::filesystem::path highLightScenePath =
                CreateWorldGraphTransactionHighLightScene(
                    worldGraphTransactionTestScenePath,
                    oldLightCapacity + 1);
            worldGraphTransactionTestHighLightScenePath =
                highLightScenePath.string();

            const RuntimeResult<WorldHandle> result =
                validationServices.ExecuteWorldGraphTransaction(
                    worldGraphTransactionTestHighLightScenePath);
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "high-light World success transaction failed: " +
                    FormatRuntimeError(result.Error()));
            }

            const RuntimeValidationOwnerSnapshot afterOwners =
                validationServices.CaptureOwnerSnapshot();
            const RuntimeValidationWorldPackageIdentities afterPackage =
                validationServices.CaptureWorldPackageIdentities();
            const uint64_t committedGeneration =
                afterOwners.world.generation;
            if (committedGeneration <= oldGeneration ||
                afterOwners.worldResourceGeneration !=
                    committedGeneration ||
                afterOwners.renderGraphGeneration !=
                    committedGeneration ||
                afterOwners.renderSystemGeneration !=
                    committedGeneration ||
                afterOwners.controllerGeneration !=
                    committedGeneration)
            {
                throw std::runtime_error(
                    "successful transaction did not advance World/cache/graph/RenderSystem/Controller generations together");
            }

            const size_t newLightCapacity =
                afterOwners.lightCapacity;
            const std::vector<uint64_t> newLightHandles =
                afterOwners.lightBufferIds;
            if (newLightCapacity <= oldLightCapacity ||
                newLightHandles.empty() ||
                newLightHandles == oldLightHandles)
            {
                throw std::runtime_error(
                    "high-light World transaction did not replace the frame light buffer");
            }
            if (afterPackage.primaryTextureIdentity !=
                beforePackage.primaryTextureIdentity)
            {
                throw std::runtime_error(
                    "compatible texture was not retained by the high-light World transaction");
            }

            const std::weak_ptr<void> retiredWorld =
                validationServices.FindPendingRetiredResource(
                    "WorldTransaction:World",
                    oldGeneration);
            const std::weak_ptr<void> retiredPackage =
                validationServices.FindPendingRetiredResource(
                    "WorldTransaction:WorldLocalResources",
                    oldGeneration);
            const std::weak_ptr<void> retiredGraph =
                validationServices.FindPendingRetiredResource(
                    "WorldTransaction:RenderGraph",
                    oldGeneration);
            const std::weak_ptr<void> retiredLightBuffer =
                validationServices.FindPendingRetiredResource(
                    "WorldTransaction:FrameLightBuffer",
                    oldGeneration);
            if (retiredWorld.expired() ||
                retiredPackage.expired() ||
                retiredGraph.expired() ||
                retiredLightBuffer.expired() ||
                retiredWorld.lock().get() !=
                    beforePackage.world.lock().get() ||
                retiredPackage.lock().get() !=
                    beforePackage.worldResources.lock().get())
            {
                throw std::runtime_error(
                    "successful transaction did not enqueue the exact old World/resource/graph packages");
            }
            worldGraphTransactionTestOldGraphPackage =
                retiredGraph;
            worldGraphTransactionTestOldLightBuffer =
                retiredLightBuffer;
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredWorld);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredPackage);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredGraph);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredLightBuffer);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                worldGraphTransactionTestOldMaterial);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                worldGraphTransactionTestOldMaterialInstance);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                worldGraphTransactionTestOldObjectResources);
            worldGraphTransactionTestMaxPendingRetiredResources =
                std::max(
                    worldGraphTransactionTestMaxPendingRetiredResources,
                    validationServices
                        .GetPendingRetiredResourceCount());
            diagnostics.ReportInfo(
                "World/graph transaction synchronized commit passed: oldGeneration=" +
                std::to_string(oldGeneration) +
                ", newGeneration=" +
                std::to_string(committedGeneration) +
                ", oldLightCapacity=" +
                std::to_string(oldLightCapacity) +
                ", newLightCapacity=" +
                std::to_string(newLightCapacity) +
                ", retirePending=" +
                std::to_string(
                    validationServices
                        .GetPendingRetiredResourceCount()) +
                ", backendBefore={" +
                FormatBackendIdentityCounts(
                    worldGraphTransactionTestBackendCountsBeforeSuccess) +
                "}, backendWithRetired={" +
                FormatBackendIdentityCounts(
                    validationServices
                        .CaptureBackendSnapshot()
                        .identityCounts) +
                "}.");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::GraphOnlySuccess);
            return;
        }

        case WorldGraphTransactionTestPhase::GraphOnlySuccess:
        {
            const RuntimeValidationOwnerSnapshot before =
                validationServices.CaptureOwnerSnapshot();

            const RuntimeResult<void> result =
                validationServices.ReloadRenderGraphResources();
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "graph-only success transaction failed: " +
                    FormatRuntimeError(result.Error()));
            }

            const RuntimeValidationOwnerSnapshot after =
                validationServices.CaptureOwnerSnapshot();
            if (!SameWorldHandle(before.world, after.world) ||
                before.worldIdentity != after.worldIdentity ||
                !SameRendererResourceFingerprint(
                    before.rendererResources,
                    after.rendererResources) ||
                before.renderGraphGeneration !=
                    after.renderGraphGeneration ||
                before.renderSystemGeneration !=
                    after.renderSystemGeneration ||
                before.controllerGeneration !=
                    after.controllerGeneration)
            {
                throw std::runtime_error(
                    "graph-only success changed a World/cache/RenderSystem/Controller owner");
            }
            if (after.renderGraphGpuFingerprint ==
                before.renderGraphGpuFingerprint)
            {
                throw std::runtime_error(
                    "graph-only success did not replace graph GPU identities");
            }
            const std::weak_ptr<void> retiredGraph =
                validationServices.FindPendingRetiredResource(
                    "RenderGraphReload:State",
                    before.renderGraphGeneration);
            if (retiredGraph.expired())
            {
                throw std::runtime_error(
                    "graph-only success did not enqueue the old graph state");
            }
            worldGraphTransactionTestGraphReloadPackage =
                retiredGraph;
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredGraph);
            worldGraphTransactionTestMaxPendingRetiredResources =
                std::max(
                    worldGraphTransactionTestMaxPendingRetiredResources,
                    validationServices
                        .GetPendingRetiredResourceCount());
            diagnostics.ReportInfo(
                "World/graph transaction graph-only commit passed: ownerGeneration=" +
                std::to_string(before.renderGraphGeneration) +
                ", beforeGraph=" +
                    before.renderGraphGpuFingerprint +
                ", afterGraph=" +
                    after.renderGraphGpuFingerprint +
                ".");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::MinimizeNoop);
            return;
        }

        case WorldGraphTransactionTestPhase::MinimizeNoop:
        {
            const std::string beforeFingerprint =
                CaptureWorldGraphRuntimeFingerprint(
                    validationServices,
                    worldGraphTransactionTestSourcePath,
                    worldGraphTransactionTestBatchSourcePath);
            const std::array<size_t, 9> beforeCounts =
                validationServices
                    .CaptureBackendSnapshot()
                    .identityCounts;
            const RuntimeResult<void> result =
                validationServices.RecreateRendererForWindowResize(
                    0,
                    0);
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "zero-size resize did not return a successful no-op: " +
                    FormatRuntimeError(result.Error()));
            }
            const std::string afterFingerprint =
                CaptureWorldGraphRuntimeFingerprint(
                    validationServices,
                    worldGraphTransactionTestSourcePath,
                    worldGraphTransactionTestBatchSourcePath);
            const std::array<size_t, 9> afterCounts =
                validationServices
                    .CaptureBackendSnapshot()
                    .identityCounts;
            if (afterFingerprint != beforeFingerprint ||
                afterCounts != beforeCounts)
            {
                throw std::runtime_error(
                    "zero-size resize modified live runtime or backend identities");
            }
            diagnostics.ReportInfo(
                "World/graph transaction zero-size resize no-op passed.");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::ResizeSuccess);
            return;
        }

        case WorldGraphTransactionTestPhase::ResizeSuccess:
        {
            const RuntimeValidationOwnerSnapshot beforeOwners =
                validationServices.CaptureOwnerSnapshot();
            const RuntimeRendererResourceFingerprint
                beforeResources =
                    beforeOwners.rendererResources;
            const auto beforePrefilteredTexture =
                beforeResources.worldTextures.find(
                    "prefilteredEnvironmentCube");
            if (beforePrefilteredTexture ==
                    beforeResources.worldTextures.end() ||
                beforePrefilteredTexture->second == 0)
            {
                throw std::runtime_error(
                    "resize test could not capture the active prefiltered environment cube");
            }
            const uint64_t beforeObjectDescriptorPool =
                GetObjectDescriptorPoolIdentity(
                    FindShaderReloadTestObjectResources());
            if (beforeObjectDescriptorPool == 0)
            {
                throw std::runtime_error(
                    "resize test could not capture the active object descriptor package");
            }
            const RuntimeValidationBackendSnapshot beforeBackend =
                validationServices.CaptureBackendSnapshot();

            const RuntimeResult<void> result =
                validationServices.RecreateRendererForWindowResize(
                    beforeBackend.swapchainWidth,
                    beforeBackend.swapchainHeight);
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "same-size resize transaction failed: " +
                    FormatRuntimeError(result.Error()));
            }
            const RuntimeValidationOwnerSnapshot afterOwners =
                validationServices.CaptureOwnerSnapshot();
            const RuntimeRendererResourceFingerprint& afterResources =
                afterOwners.rendererResources;
            const auto afterPrefilteredTexture =
                afterResources.worldTextures.find(
                    "prefilteredEnvironmentCube");
            const uint64_t afterObjectDescriptorPool =
                GetObjectDescriptorPoolIdentity(
                    FindShaderReloadTestObjectResources());
            if (!SameWorldHandle(
                    beforeOwners.world,
                    afterOwners.world) ||
                beforeOwners.worldIdentity !=
                    afterOwners.worldIdentity ||
                !SameRendererResourceFingerprintExceptWorldTexture(
                    beforeResources,
                    afterResources,
                    "prefilteredEnvironmentCube") ||
                afterOwners.renderGraphGeneration !=
                    beforeOwners.renderGraphGeneration ||
                afterOwners.renderSystemGeneration !=
                    beforeOwners.renderSystemGeneration ||
                afterOwners.controllerGeneration !=
                    beforeOwners.controllerGeneration)
            {
                throw std::runtime_error(
                    "successful resize changed a stable runtime owner or generation:" +
                    DescribeRendererResourceFingerprintDifference(
                        beforeResources,
                        afterResources));
            }
            if (afterPrefilteredTexture ==
                    afterResources.worldTextures.end() ||
                afterPrefilteredTexture->second == 0 ||
                afterPrefilteredTexture->second ==
                    beforePrefilteredTexture->second ||
                afterObjectDescriptorPool == 0 ||
                afterObjectDescriptorPool ==
                    beforeObjectDescriptorPool)
            {
                throw std::runtime_error(
                    "successful resize did not replace its environment or object descriptor packages");
            }
            const RuntimeValidationBackendSnapshot afterBackend =
                validationServices.CaptureBackendSnapshot();
            if (afterOwners.renderGraphGpuFingerprint ==
                    beforeOwners.renderGraphGpuFingerprint ||
                afterBackend.identityCounts !=
                    beforeBackend.identityCounts)
            {
                throw std::runtime_error(
                    "successful resize did not replace graph GPU state with stable backend counts");
            }
            diagnostics.ReportInfo(
                "World/graph transaction same-size resize passed: generation=" +
                std::to_string(
                    beforeOwners.renderGraphGeneration) +
                ", beforeGraph=" +
                    beforeOwners.renderGraphGpuFingerprint +
                ", afterGraph=" +
                    afterOwners.renderGraphGpuFingerprint +
                ", prefilteredEnvironment=" +
                std::to_string(
                    beforePrefilteredTexture->second) +
                "->" +
                std::to_string(
                    afterPrefilteredTexture->second) +
                ", objectDescriptorPool=" +
                std::to_string(
                    beforeObjectDescriptorPool) +
                "->" +
                std::to_string(
                    afterObjectDescriptorPool) +
                ".");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::ShaderReloadSuccess);
            return;
        }

        case WorldGraphTransactionTestPhase::ShaderReloadSuccess:
        {
            const RuntimeValidationOwnerSnapshot before =
                validationServices.CaptureOwnerSnapshot();
            const uint64_t beforeReloadGeneration =
                validationServices.CaptureShaderReloadState().latestManualShaderReloadCommittedGeneration;
            RuntimeCommandExecutionResult commandResult;
            commandResult.shaderReloadRequested = true;
            commandResult.shaderReloadScope =
                RuntimeShaderReloadScope::All;
            validationServices.ProcessShaderRuntimeRequest(
                commandResult);
            const RuntimeValidationOwnerSnapshot after =
                validationServices.CaptureOwnerSnapshot();
            const ShaderReloadRuntimeStateSnapshot reloadState =
                validationServices.CaptureShaderReloadState();
            if (validationServices.IsClosing() ||
                validationServices
                        .CaptureShaderReloadState()
                        .latestManualShaderReloadCommittedGeneration <=
                    beforeReloadGeneration ||
                !SameWorldHandle(
                    before.world,
                    after.world) ||
                before.worldIdentity !=
                    after.worldIdentity ||
                !SameRendererResourceFingerprint(
                    before.rendererResources,
                    after.rendererResources) ||
                after.renderGraphGeneration !=
                    before.renderGraphGeneration ||
                after.renderSystemGeneration !=
                    before.renderSystemGeneration ||
                after.controllerGeneration !=
                    before.controllerGeneration)
            {
                throw std::runtime_error(
                    "shader reload interleave changed World/graph ownership or did not commit");
            }

            validationServices.SetShaderMonitorScanSuspended(true);
            worldGraphTransactionTestMonitorSuspended = true;
            WriteTextFileAtomically(
                worldGraphTransactionTestSourcePath,
                worldGraphTransactionTestCandidateSource);
            diagnostics.ReportInfo(
                "World/graph transaction shader reload interleave passed: generation=" +
                std::to_string(
                    reloadState
                        .latestManualShaderReloadCommittedGeneration) +
                ".");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::MaterialDefinitionRuntimeFailure);
            return;
        }

        case WorldGraphTransactionTestPhase::MaterialDefinitionRuntimeFailure:
            injection.failDescriptorCreationAt = 2;
            ValidateWorldGraphTransactionFailure(
                validationServices,
                injection,
                false,
                true,
                "M_ schema rebuild with runtime descriptor failure",
                "Injected render graph descriptor creation failure",
                diagnostics);
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::MaterialDefinitionSuccess);
            return;

        case WorldGraphTransactionTestPhase::MaterialDefinitionSuccess:
        {
            const RuntimeValidationOwnerSnapshot before =
                validationServices.CaptureOwnerSnapshot();
            const RuntimeValidationWorldPackageIdentities oldPackage =
                validationServices.CaptureWorldPackageIdentities();
            const uint64_t oldGeneration =
                before.world.generation;
            if (oldPackage.world.expired() ||
                oldPackage.worldResources.expired() ||
                oldPackage.material.expired() ||
                oldPackage.materialInstance.expired() ||
                oldPackage.objectResources.expired() ||
                oldPackage.primaryTexture.expired())
            {
                throw std::runtime_error(
                    "candidate M_ success could not capture the old runtime package");
            }

            const RuntimeResult<WorldHandle> result =
                validationServices
                    .ExecuteMaterialDefinitionWorldGraphTransaction(
                        {"runtimeTest/M_shaderReloadTest.json"},
                        worldGraphTransactionTestNextBatchId++);
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "candidate M_ success transaction failed: " +
                    FormatRuntimeError(result.Error()));
            }
            const RuntimeValidationOwnerSnapshot after =
                validationServices.CaptureOwnerSnapshot();
            const RuntimeValidationWorldPackageIdentities activePackage =
                validationServices.CaptureWorldPackageIdentities();
            const uint64_t committedGeneration =
                after.world.generation;
            if (committedGeneration <= oldGeneration ||
                activePackage.generation !=
                    committedGeneration ||
                after.worldResourceGeneration !=
                    committedGeneration ||
                after.renderGraphGeneration !=
                    committedGeneration ||
                after.renderSystemGeneration !=
                    committedGeneration ||
                after.controllerGeneration !=
                    committedGeneration ||
                !activePackage.materialInstanceHasCandidateParameter ||
                activePackage.primaryTextureIdentity !=
                    oldPackage.primaryTextureIdentity)
            {
                throw std::runtime_error(
                    "candidate M_ success did not atomically publish generations, schema, and compatible texture");
            }

            const std::weak_ptr<void> retiredWorld =
                validationServices.FindPendingRetiredResource(
                    "WorldTransaction:World",
                    oldGeneration);
            const std::weak_ptr<void> retiredPackage =
                validationServices.FindPendingRetiredResource(
                    "WorldTransaction:WorldLocalResources",
                    oldGeneration);
            const std::weak_ptr<void> retiredGraph =
                validationServices.FindPendingRetiredResource(
                    "WorldTransaction:RenderGraph",
                    oldGeneration);
            if (retiredWorld.expired() ||
                retiredPackage.expired() ||
                retiredGraph.expired() ||
                retiredWorld.lock().get() !=
                    oldPackage.world.lock().get() ||
                retiredPackage.lock().get() !=
                    oldPackage.worldResources.lock().get())
            {
                throw std::runtime_error(
                    "candidate M_ success did not enqueue the exact old runtime packages");
            }
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredWorld);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredPackage);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredGraph);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                oldPackage.material);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                oldPackage.materialInstance);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                oldPackage.objectResources);
            worldGraphTransactionTestMaxPendingRetiredResources =
                std::max(
                    worldGraphTransactionTestMaxPendingRetiredResources,
                    validationServices
                        .GetPendingRetiredResourceCount());
            diagnostics.ReportInfo(
                "World/graph transaction candidate M_ commit passed: oldGeneration=" +
                std::to_string(oldGeneration) +
                ", newGeneration=" +
                std::to_string(committedGeneration) +
                ", retirePending=" +
                std::to_string(
                    validationServices
                        .GetPendingRetiredResourceCount()) +
                ".");
            AdvanceWorldGraphTransactionTestAfterRenderedFrames(
                WorldGraphTransactionTestPhase::RestoreOriginalCommit);
            return;
        }

        case WorldGraphTransactionTestPhase::RestoreOriginalCommit:
        {
            WriteTextFileAtomically(
                worldGraphTransactionTestSourcePath,
                worldGraphTransactionTestOriginalSource);
            const RuntimeValidationOwnerSnapshot before =
                validationServices.CaptureOwnerSnapshot();
            const RuntimeValidationWorldPackageIdentities candidatePackage =
                validationServices.CaptureWorldPackageIdentities();
            const uint64_t candidateGeneration =
                before.world.generation;
            if (candidatePackage.world.expired() ||
                candidatePackage.worldResources.expired() ||
                candidatePackage.material.expired() ||
                candidatePackage.materialInstance.expired() ||
                candidatePackage.objectResources.expired() ||
                candidatePackage.primaryTexture.expired())
            {
                throw std::runtime_error(
                    "original M_ restore could not capture the candidate runtime package");
            }
            const RuntimeResult<WorldHandle> result =
                validationServices
                    .ExecuteMaterialDefinitionWorldGraphTransaction(
                        {"runtimeTest/M_shaderReloadTest.json"},
                        worldGraphTransactionTestNextBatchId++);
            if (result.IsFailure())
            {
                throw std::runtime_error(
                    "original M_ restore transaction failed: " +
                    FormatRuntimeError(result.Error()));
            }
            const RuntimeValidationOwnerSnapshot after =
                validationServices.CaptureOwnerSnapshot();
            const RuntimeValidationWorldPackageIdentities restoredPackage =
                validationServices.CaptureWorldPackageIdentities();
            const uint64_t restoredGeneration =
                after.world.generation;
            if (restoredGeneration <= candidateGeneration ||
                restoredPackage.generation !=
                    restoredGeneration ||
                after.worldResourceGeneration !=
                    restoredGeneration ||
                after.renderGraphGeneration !=
                    restoredGeneration ||
                after.renderSystemGeneration !=
                    restoredGeneration ||
                after.controllerGeneration !=
                    restoredGeneration)
            {
                throw std::runtime_error(
                    "original schema restore did not commit all runtime owners together");
            }
            if (restoredPackage.materialInstanceHasCandidateParameter ||
                restoredPackage.primaryTextureIdentity !=
                    candidatePackage.primaryTextureIdentity)
            {
                throw std::runtime_error(
                    "original M_ restore left candidate state active or replaced a compatible texture");
            }
            const std::weak_ptr<void> retiredWorld =
                validationServices.FindPendingRetiredResource(
                    "WorldTransaction:World",
                    candidateGeneration);
            const std::weak_ptr<void> retiredPackage =
                validationServices.FindPendingRetiredResource(
                    "WorldTransaction:WorldLocalResources",
                    candidateGeneration);
            const std::weak_ptr<void> retiredGraph =
                validationServices.FindPendingRetiredResource(
                    "WorldTransaction:RenderGraph",
                    candidateGeneration);
            if (retiredWorld.expired() ||
                retiredPackage.expired() ||
                retiredGraph.expired() ||
                retiredWorld.lock().get() !=
                    candidatePackage.world.lock().get() ||
                retiredPackage.lock().get() !=
                    candidatePackage.worldResources.lock().get())
            {
                throw std::runtime_error(
                    "schema restore did not enqueue the exact candidate packages for retirement");
            }
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredWorld);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredPackage);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                retiredGraph);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                candidatePackage.material);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                candidatePackage.materialInstance);
            worldGraphTransactionTestResourcesExpectedToExpire.push_back(
                candidatePackage.objectResources);
            worldGraphTransactionTestMaxPendingRetiredResources =
                std::max(
                    worldGraphTransactionTestMaxPendingRetiredResources,
                    validationServices
                        .GetPendingRetiredResourceCount());
            diagnostics.ReportInfo(
                "World/graph transaction original schema restore passed: "
                "candidateGeneration=" +
                std::to_string(candidateGeneration) +
                ", restoredGeneration=" +
                std::to_string(restoredGeneration) +
                ", retirePending=" +
                std::to_string(
                    validationServices
                        .GetPendingRetiredResourceCount()) +
                ".");
            worldGraphTransactionTestPhase =
                WorldGraphTransactionTestPhase::WaitRetireDrain;
            worldGraphTransactionTestRetireDrainFramesRemaining =
                RetireDrainFrameBudget;
            return;
        }

        case WorldGraphTransactionTestPhase::ResizeFatalFailure:
        {
            const RuntimeValidationBackendSnapshot backend =
                validationServices.CaptureBackendSnapshot();
            injection.failResizeAfterSwapchainRecreate =
                true;
            validationServices.SetWorldGraphTransactionFaultInjection(
                injection);
            const RuntimeResult<void> result =
                validationServices.RecreateRendererForWindowResize(
                    backend.swapchainWidth,
                    backend.swapchainHeight);
            validationServices.SetWorldGraphTransactionFaultInjection(
                {});
            if (result.IsSuccess() ||
                FormatRuntimeError(result.Error()).find(
                    "Injected resize failure after swapchain recreation") ==
                    std::string::npos ||
                !validationServices.IsClosing() ||
                validationServices.GetExitCode() != 0)
            {
                throw std::runtime_error(
                    "post-swapchain resize failure did not stop the runtime with the expected fatal contract");
            }

            if (worldGraphTransactionTestMonitorSuspended)
            {
                validationServices.RefreshShaderMonitorBaseline({
                    "runtimeTest/M_shaderReloadTest.json"});
                validationServices.SetShaderMonitorScanSuspended(false);
            }
            worldGraphTransactionTestMonitorSuspended = false;
            worldGraphTransactionTestSourcePath.clear();
            worldGraphTransactionTestOriginalSource.clear();
            worldGraphTransactionTestCandidateSource.clear();
            worldGraphTransactionTestHighLightScenePath.clear();
            worldGraphTransactionTestResourcesExpectedToExpire.clear();
            worldGraphTransactionTestActive = false;
            worldGraphTransactionTestPhase =
                WorldGraphTransactionTestPhase::Idle;
            runtimeTestStatus =
                RuntimeTestStatus::Succeeded;
            CleanupGeneratedRuntimeFixture(
                worldGraphTransactionTestFixtureDirectory,
                diagnostics);
            worldGraphTransactionTestFixtureDirectory.clear();
            diagnostics.ReportInfo(
                "World/graph transaction runtime test completed: "
                "World and graph rollback faults rendered through, "
                "synchronized World/light commit, graph-only commit, "
                "minimize/resize, shader/M_ interleave, retirement drain, "
                "and fatal post-swapchain failure checks passed.");
            return;
        }

        case WorldGraphTransactionTestPhase::Idle:
        case WorldGraphTransactionTestPhase::WaitWorldLoad:
        case WorldGraphTransactionTestPhase::WaitRetireDrain:
            return;
        }
    }
    catch (const std::exception& exception)
    {
        if (validationServices.IsClosing())
        {
            validationServices.MarkClosingTestFailure();
        }
        validationServices.SetWorldGraphTransactionFaultInjection({});
        FailWorldGraphTransactionTest(
            &validationServices,
            std::string(
                "World/graph transaction runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

} // namespace VL

