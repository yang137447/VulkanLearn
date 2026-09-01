#include "engine/runtimeTestHooks.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "engine/diagnosticsSubsystem.h"
#include "engine/testing/runtimeTestFixtures.h"
#include "engine/testing/runtimeValidationServices.h"
#include "material.h"
#include "materialInstance.h"
#include "material/generator/materialParameterIncludeGenerator.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/resourceRetireQueue.h"
#include "shader/build/atomicFile.h"
#include "shader/reload/shaderCompileWorker.h"

namespace VL
{
using namespace RuntimeTestFixtures;
bool RuntimeTestHooks::BeginShaderReloadTest(
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
            "shader" /
            "glsl" /
            "runtimeTest" /
            "shaderReloadTestShared.glsl";
        const std::filesystem::path scenePath =
            CreateShaderReloadTestScene(resourcePath);

        shaderReloadTestSourcePath =
            sourcePath.string();
        shaderReloadTestOriginalSource =
            ReadTextFileBytes(sourcePath);
        shaderReloadTestCompatibleSourceA =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.bgr");
        shaderReloadTestSyntaxErrorSource =
            BuildShaderReloadSyntaxErrorSource();
        shaderReloadTestCompatibleSourceB =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.rgb * 0.5");
        shaderReloadTestAbiIncompatibleSource =
            BuildShaderReloadAbiIncompatibleSource();
        shaderReloadTestScenePath =
            scenePath.string();
        shaderReloadTestFixtureDirectory =
            scenePath.parent_path().string();
        shaderReloadTestRetireDrainFramesRemaining = 0;
        shaderReloadTestMaxPendingRetiredResources = 0;
        waitingForShaderReloadTestWorld = false;
        shaderReloadTestPhase =
            ShaderReloadTestPhase::WaitWorldLoad;
        shaderReloadTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader reload runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create shader reload runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            shaderReloadTestFixtureDirectory,
            diagnostics);
        shaderReloadTestFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginShaderComputeReloadTest(
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
        const std::filesystem::path shaderRoot =
            std::filesystem::path(CommonFunction::GetProjectPath()) /
            "shader" /
            "glsl";
        const std::filesystem::path skyShPath =
            shaderRoot / "generator" / "skySHGenerate.comp";
        const std::filesystem::path prefilterPath =
            shaderRoot / "generator" / "prefilterEnvMap.comp";
        const std::filesystem::path scenePath =
            CreateShaderReloadTestScene(resourcePath);

        shaderComputeSkyShSourcePath = skyShPath.string();
        shaderComputePrefilterSourcePath = prefilterPath.string();
        shaderComputeSkyShOriginal =
            ReadTextFileBytes(skyShPath);
        shaderComputePrefilterOriginal =
            ReadTextFileBytes(prefilterPath);
        shaderComputeSkyShCompatible =
            ReplaceFirstOccurrence(
                shaderComputeSkyShOriginal,
                "vec3 radiance = textureLod(inSkyCube, dir, 0.0).rgb;",
                "vec3 radiance = textureLod(inSkyCube, dir, 0.0).rgb * 0.9995;");
        shaderComputePrefilterCompatible =
            ReplaceFirstOccurrence(
                shaderComputePrefilterOriginal,
                "imageStore(outPrefilteredEnvironmentCube, ivec3(pixelCoord, faceIndex), vec4(color, 1.0));",
                "imageStore(outPrefilteredEnvironmentCube, ivec3(pixelCoord, faceIndex), vec4(color * 0.9995, 1.0));");
        shaderComputeSkyShAbiIncompatible =
            ReplaceFirstOccurrence(
                shaderComputeSkyShOriginal,
                "layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;",
                "layout(local_size_x = 8, local_size_y = 16, local_size_z = 1) in;");

        shaderComputeReloadTestScenePath = scenePath.string();
        shaderComputeReloadTestFixtureDirectory =
            scenePath.parent_path().string();
        shaderComputeReloadTestDeadline = {};
        shaderComputeReloadTestRetireDrainFramesRemaining = 0;
        shaderComputeReloadTestMaxPendingRetiredResources = 0;
        shaderComputeReloadTestBaselineLatestGeneration = 0;
        shaderComputeSkyShBaselineGeneration.clear();
        shaderComputePrefilterBaselineGeneration.clear();
        waitingForShaderComputeReloadTestWorld = false;
        shaderComputeReloadTestPhase =
            ShaderComputeReloadTestPhase::WaitWorldLoad;
        shaderComputeReloadTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader compute reload runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create shader compute reload runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            shaderComputeReloadTestFixtureDirectory,
            diagnostics);
        shaderComputeReloadTestFixtureDirectory.clear();
        return false;
    }
}

void RuntimeTestHooks::UpdateShaderReloadTest(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderReloadTestActive)
    {
        return;
    }

    if (shaderReloadTestPhase ==
        ShaderReloadTestPhase::WaitRetireDrain)
    {
        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        const size_t pending = retireQueue.GetPendingCount();
        shaderReloadTestMaxPendingRetiredResources =
            std::max(
                shaderReloadTestMaxPendingRetiredResources,
                pending);
        if (pending == 0)
        {
            if (shaderReloadTestMaxPendingRetiredResources == 0)
            {
                FailShaderReloadTest(
                    "Shader reload runtime test did not observe any "
                    "epoch-retired pipelines.",
                    diagnostics);
                return;
            }

            shaderReloadTestActive = false;
            shaderReloadTestPhase =
                ShaderReloadTestPhase::Idle;
            runtimeTestStatus =
                RuntimeTestStatus::Succeeded;
            CleanupGeneratedRuntimeFixture(
                shaderReloadTestFixtureDirectory,
                diagnostics);
            shaderReloadTestFixtureDirectory.clear();
            diagnostics.ReportInfo(
                "Shader reload runtime test completed: "
                "compatible Surface/Shadow commits, compile rollback, "
                "ABI rejection, pipeline creation rollback, recovery, "
                "formal artifact restoration, and epoch retirement passed.");
            return;
        }

        --shaderReloadTestRetireDrainFramesRemaining;
        if (shaderReloadTestRetireDrainFramesRemaining <= 0)
        {
            FailShaderReloadTest(
                "Shader reload runtime test timed out waiting for "
                "retired pipelines to drain. pending=" +
                    std::to_string(pending),
                diagnostics);
        }
        return;
    }

    try
    {
        const ShaderReloadRuntimeSnapshot before =
            CaptureShaderReloadRuntimeSnapshot(validationServices);
        std::string nextSource;
        bool expectSuccess = false;
        bool injectPipelineFailure = false;
        const char* expectedFailureText = nullptr;
        const char* phaseName = nullptr;

        switch (shaderReloadTestPhase)
        {
        case ShaderReloadTestPhase::CompatibleCommit:
            nextSource = shaderReloadTestCompatibleSourceA;
            expectSuccess = true;
            phaseName = "compatible commit";
            break;
        case ShaderReloadTestPhase::SyntaxFailure:
            nextSource = shaderReloadTestSyntaxErrorSource;
            expectedFailureText = "Shader compile failed";
            phaseName = "syntax failure rollback";
            break;
        case ShaderReloadTestPhase::SyntaxRecovery:
            nextSource = shaderReloadTestCompatibleSourceB;
            expectSuccess = true;
            phaseName = "syntax recovery";
            break;
        case ShaderReloadTestPhase::AbiRejection:
            nextSource =
                shaderReloadTestAbiIncompatibleSource;
            expectedFailureText =
                "ABI changed";
            phaseName = "ABI rejection";
            break;
        case ShaderReloadTestPhase::PipelineFailure:
            nextSource =
                shaderReloadTestCompatibleSourceA;
            injectPipelineFailure = true;
            expectedFailureText =
                "Injected graphics pipeline creation failure";
            phaseName = "pipeline creation rollback";
            break;
        case ShaderReloadTestPhase::PipelineRecovery:
            nextSource =
                shaderReloadTestCompatibleSourceA;
            expectSuccess = true;
            phaseName = "pipeline creation recovery";
            break;
        case ShaderReloadTestPhase::RestoreOriginal:
            nextSource = shaderReloadTestOriginalSource;
            expectSuccess = true;
            phaseName = "original source restore";
            break;
        case ShaderReloadTestPhase::Idle:
        case ShaderReloadTestPhase::WaitWorldLoad:
        case ShaderReloadTestPhase::WaitRetireDrain:
            return;
        }

        WriteTextFileAtomically(
            shaderReloadTestSourcePath,
            nextSource);

        const RuntimeValidationManualShaderReloadResult operation =
            validationServices.ExecuteManualGraphicsShaderReload(
                {"runtimeTest/shaderReloadTestShared.glsl"},
                injectPipelineFailure);

        const ShaderReloadRuntimeSnapshot after =
            CaptureShaderReloadRuntimeSnapshot(validationServices);
        if (expectSuccess)
        {
            if (!operation.succeeded)
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " unexpectedly failed: " +
                    operation.failureMessage);
            }
            if (!operation.committed ||
                operation.affectedBuildCount != 2 ||
                operation.pipelinesCreated != 2 ||
                operation.pipelinesRetired != 2)
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " did not commit the complete Surface/Shadow batch");
            }
            if (before.surfacePipeline ==
                    after.surfacePipeline ||
                before.shadowPipeline ==
                    after.shadowPipeline ||
                before.surfaceGeneration ==
                    after.surfaceGeneration ||
                before.shadowGeneration ==
                    after.shadowGeneration)
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " did not replace both live Pipeline generations");
            }
            diagnostics.ReportInfo(
                std::string("Shader reload runtime test passed ") +
                phaseName +
                ": builds=2, pipelinesCreated=2, retired=2.");
        }
        else
        {
            if (operation.succeeded)
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " unexpectedly committed");
            }
            if (expectedFailureText == nullptr ||
                operation.failureMessage.find(expectedFailureText) ==
                    std::string::npos)
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " reported an unexpected error: " +
                    operation.failureMessage);
            }
            if (!SameShaderReloadRuntimeSnapshot(
                    before,
                    after))
            {
                throw std::runtime_error(
                    std::string(phaseName) +
                    " changed a live Pipeline or formal artifact");
            }
            diagnostics.ReportInfo(
                std::string("Shader reload runtime test passed ") +
                phaseName +
                ": old Pipeline and formal artifact remained active.");
        }

        switch (shaderReloadTestPhase)
        {
        case ShaderReloadTestPhase::CompatibleCommit:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::SyntaxFailure;
            break;
        case ShaderReloadTestPhase::SyntaxFailure:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::SyntaxRecovery;
            break;
        case ShaderReloadTestPhase::SyntaxRecovery:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::AbiRejection;
            break;
        case ShaderReloadTestPhase::AbiRejection:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::PipelineFailure;
            break;
        case ShaderReloadTestPhase::PipelineFailure:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::PipelineRecovery;
            break;
        case ShaderReloadTestPhase::PipelineRecovery:
            shaderReloadTestPhase =
                ShaderReloadTestPhase::RestoreOriginal;
            break;
        case ShaderReloadTestPhase::RestoreOriginal:
            shaderReloadTestSourcePath.clear();
            shaderReloadTestOriginalSource.clear();
            shaderReloadTestPhase =
                ShaderReloadTestPhase::WaitRetireDrain;
            shaderReloadTestRetireDrainFramesRemaining =
                RetireDrainFrameBudget;
            shaderReloadTestMaxPendingRetiredResources =
                ResourceRetireQueue::GetInstance()
                    .GetPendingCount();
            break;
        case ShaderReloadTestPhase::Idle:
        case ShaderReloadTestPhase::WaitWorldLoad:
        case ShaderReloadTestPhase::WaitRetireDrain:
            break;
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderReloadTest(
            std::string(
                "Shader reload runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

void RuntimeTestHooks::FailShaderReloadTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderReloadTestSourcePath.empty() &&
        !shaderReloadTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderReloadTestSourcePath,
                shaderReloadTestOriginalSource);
        }
        catch (const std::exception& exception)
        {
            diagnostics.ReportError(
                std::string(
                    "Shader reload runtime test could not restore "
                    "the source fixture: ") +
                exception.what());
        }
    }
    shaderReloadTestSourcePath.clear();
    shaderReloadTestOriginalSource.clear();
    shaderReloadTestActive = false;
    waitingForShaderReloadTestWorld = false;
    shaderReloadTestPhase =
        ShaderReloadTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        shaderReloadTestFixtureDirectory,
        diagnostics);
    shaderReloadTestFixtureDirectory.clear();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::FailShaderComputeReloadTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderComputeSkyShSourcePath.empty() &&
        !shaderComputeSkyShOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderComputeSkyShSourcePath,
                shaderComputeSkyShOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderComputePrefilterSourcePath.empty() &&
        !shaderComputePrefilterOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderComputePrefilterSourcePath,
                shaderComputePrefilterOriginal);
        }
        catch (...)
        {
        }
    }
    shaderComputeReloadTestActive = false;
    waitingForShaderComputeReloadTestWorld = false;
    shaderComputeReloadTestPhaseEntryPending = false;
    shaderComputeReloadTestPhase =
        ShaderComputeReloadTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        shaderComputeReloadTestFixtureDirectory,
        diagnostics);
    shaderComputeReloadTestFixtureDirectory.clear();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::UpdateShaderComputeReloadTest(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderComputeReloadTestActive)
    {
        return;
    }

    if (shaderComputeReloadTestPhase ==
        ShaderComputeReloadTestPhase::WaitRetireDrain)
    {
        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        const size_t pending = retireQueue.GetPendingCount();
        shaderComputeReloadTestMaxPendingRetiredResources =
            std::max(
                shaderComputeReloadTestMaxPendingRetiredResources,
                pending);
        if (pending == 0)
        {
            if (shaderComputeReloadTestMaxPendingRetiredResources == 0)
            {
                FailShaderComputeReloadTest(
                    "Shader compute reload runtime test did not observe any "
                    "epoch-retired compute resources.",
                    diagnostics);
                return;
            }
            shaderComputeReloadTestActive = false;
            shaderComputeReloadTestPhase =
                ShaderComputeReloadTestPhase::Idle;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            CleanupGeneratedRuntimeFixture(
                shaderComputeReloadTestFixtureDirectory,
                diagnostics);
            shaderComputeReloadTestFixtureDirectory.clear();
            diagnostics.ReportInfo(
                "Shader compute reload runtime test completed: "
                "ABI-compatible compute batch commit, ABI rejection with "
                "descriptor retention, recovery, and epoch retirement passed.");
            return;
        }

        --shaderComputeReloadTestRetireDrainFramesRemaining;
        if (shaderComputeReloadTestRetireDrainFramesRemaining <= 0)
        {
            FailShaderComputeReloadTest(
                "Shader compute reload runtime test timed out waiting for "
                "retired resources to drain. pending=" +
                    std::to_string(pending),
                diagnostics);
        }
        return;
    }

    try
    {
        if (shaderComputeReloadTestPhaseEntryPending)
        {
            shaderComputeReloadTestPhaseEntryPending = false;
            WriteTextFileAtomically(
                shaderComputeSkyShSourcePath,
                shaderComputeSkyShCompatible);
            WriteTextFileAtomically(
                shaderComputePrefilterSourcePath,
                shaderComputePrefilterCompatible);
            shaderComputeSkyShBaselineGeneration =
                validationServices.GetComputeShaderGeneration(
                    "generator/skySHGenerate");
            shaderComputePrefilterBaselineGeneration =
                validationServices.GetComputeShaderGeneration(
                    "generator/prefilterEnvMap");
            shaderComputeReloadTestBaselineLatestGeneration =
                validationServices.CaptureShaderReloadState().latestSubmittedAutoReloadGeneration;
            shaderComputeReloadTestDeadline =
                std::chrono::steady_clock::now() +
                ShaderAsyncWaitTimeout;
        }

        const ShaderReloadRuntimeStateSnapshot reloadState =
            validationServices.CaptureShaderReloadState();
        const bool pendingSourcesSettled =
            reloadState.pendingAutoReloadSources.empty() ||
            (reloadState.failedPendingAutoReloadSourceEpoch != 0 &&
             reloadState.failedPendingAutoReloadSourceEpoch ==
                 reloadState.pendingAutoReloadSourceEpoch);
        const bool workerSettled =
            reloadState.workerRunning &&
            reloadState.workerIdle &&
            pendingSourcesSettled &&
            reloadState.inFlightAutoReloadGeneration == 0;
        const std::string skyGeneration =
            validationServices.GetComputeShaderGeneration(
                "generator/skySHGenerate");
        const std::string prefilterGeneration =
            validationServices.GetComputeShaderGeneration(
                "generator/prefilterEnvMap");

        switch (shaderComputeReloadTestPhase)
        {
        case ShaderComputeReloadTestPhase::WaitCompatibleCommit:
            if (workerSettled &&
                reloadState.latestSubmittedAutoReloadGeneration >
                    shaderComputeReloadTestBaselineLatestGeneration)
            {
                if (skyGeneration ==
                        shaderComputeSkyShBaselineGeneration ||
                    prefilterGeneration ==
                        shaderComputePrefilterBaselineGeneration)
                {
                    throw std::runtime_error(
                        "compatible compute batch did not commit both "
                        "pipelines");
                }
                diagnostics.ReportInfo(
                    "Shader compute reload runtime test passed the "
                    "ABI-compatible compute batch commit.");
                WriteTextFileAtomically(
                    shaderComputeSkyShSourcePath,
                    shaderComputeSkyShAbiIncompatible);
                shaderComputeSkyShBaselineGeneration =
                    skyGeneration;
                shaderComputePrefilterBaselineGeneration =
                    prefilterGeneration;
                shaderComputeReloadTestBaselineLatestGeneration =
                    reloadState.latestSubmittedAutoReloadGeneration;
                shaderComputeReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderComputeReloadTestPhase =
                    ShaderComputeReloadTestPhase::WaitAbiRejection;
            }
            break;

        case ShaderComputeReloadTestPhase::WaitAbiRejection:
            if (workerSettled &&
                reloadState.latestSubmittedAutoReloadGeneration >
                    shaderComputeReloadTestBaselineLatestGeneration)
            {
                if (skyGeneration !=
                        shaderComputeSkyShBaselineGeneration ||
                    prefilterGeneration !=
                        shaderComputePrefilterBaselineGeneration)
                {
                    throw std::runtime_error(
                        "ABI-incompatible compute edit changed a live "
                        "pipeline or descriptor package");
                }
                diagnostics.ReportInfo(
                    "Shader compute reload runtime test passed ABI "
                    "rejection with old resource retention.");
                WriteTextFileAtomically(
                    shaderComputeSkyShSourcePath,
                    shaderComputeSkyShOriginal);
                WriteTextFileAtomically(
                    shaderComputePrefilterSourcePath,
                    shaderComputePrefilterOriginal);
                shaderComputeSkyShBaselineGeneration =
                    skyGeneration;
                shaderComputePrefilterBaselineGeneration =
                    prefilterGeneration;
                shaderComputeReloadTestBaselineLatestGeneration =
                    reloadState.latestSubmittedAutoReloadGeneration;
                shaderComputeReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderComputeReloadTestPhase =
                    ShaderComputeReloadTestPhase::RestoreOriginal;
            }
            break;

        case ShaderComputeReloadTestPhase::RestoreOriginal:
            if (workerSettled &&
                reloadState.latestSubmittedAutoReloadGeneration >
                    shaderComputeReloadTestBaselineLatestGeneration)
            {
                if (skyGeneration ==
                        shaderComputeSkyShBaselineGeneration ||
                    prefilterGeneration ==
                        shaderComputePrefilterBaselineGeneration)
                {
                    throw std::runtime_error(
                        "compute original restore did not commit both "
                        "pipelines");
                }
                diagnostics.ReportInfo(
                    "Shader compute reload runtime test passed original "
                    "source restore.");
                shaderComputeReloadTestPhase =
                    ShaderComputeReloadTestPhase::WaitRetireDrain;
                shaderComputeReloadTestRetireDrainFramesRemaining =
                    RetireDrainFrameBudget;
                shaderComputeReloadTestMaxPendingRetiredResources =
                    ResourceRetireQueue::GetInstance()
                        .GetPendingCount();
                return;
            }
            break;

        case ShaderComputeReloadTestPhase::Idle:
        case ShaderComputeReloadTestPhase::WaitWorldLoad:
        case ShaderComputeReloadTestPhase::WaitRetireDrain:
            break;
        }

        if (std::chrono::steady_clock::now() >=
            shaderComputeReloadTestDeadline)
        {
            throw std::runtime_error(
                "timed out waiting for the compute reload pipeline to "
                "settle in phase " +
                std::to_string(
                    static_cast<int>(shaderComputeReloadTestPhase)) +
                "; skyGen=" + skyGeneration +
                ", baselineSky=" +
                    shaderComputeSkyShBaselineGeneration +
                ", prefilterGen=" + prefilterGeneration +
                ", latest=" +
                    std::to_string(
                        validationServices.CaptureShaderReloadState().latestSubmittedAutoReloadGeneration) +
                ", baselineLatest=" +
                    std::to_string(
                        shaderComputeReloadTestBaselineLatestGeneration) +
                ", workerIdle=" +
                    std::string(
                        reloadState.workerRunning &&
                                reloadState.workerIdle
                            ? "true"
                            : "false") +
                ", hasResult=" +
                    std::string(
                        reloadState.workerHasCompletedResult
                            ? "true"
                            : "false") +
                ", inFlight=" +
                    std::to_string(
                        reloadState.workerInFlightGeneration) +
                ", pendingPlan=" +
                    std::string(
                        !reloadState.pendingAutoReloadSources.empty()
                            ? "true"
                            : "false"));
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderComputeReloadTest(
            std::string(
                "Shader compute reload runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

} // namespace VL

