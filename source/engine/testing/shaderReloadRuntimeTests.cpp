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
bool RuntimeTestHooks::SurfaceArtifactDependsOnSourceWithDigest(
    RuntimeValidationServices& validationServices,
    const std::string& surfaceLogicalBuildId,
    const std::string& dependencyIdentity,
    const std::string& expectedDigest)
{
    return validationServices.SurfaceArtifactDependsOnSourceWithDigest(
        surfaceLogicalBuildId,
        dependencyIdentity,
        expectedDigest);
}

bool RuntimeTestHooks::UiArtifactFragmentMatchesCurrentSource(
    RuntimeValidationServices& validationServices) const
{
    return validationServices.UiArtifactFragmentMatchesCurrentSource();
}
std::string

RuntimeTestHooks::CaptureShaderDefinitionRuntimeFingerprint(
    RuntimeValidationServices& validationServices) const
{
    return CaptureWorldGraphRuntimeFingerprint(
        validationServices,
        shaderDefinitionReloadTestSourcePath,
        shaderDefinitionReloadTestBatchSourcePath,
        false);
}

std::string

RuntimeTestHooks::CaptureShaderShutdownInflightFingerprint(
    RuntimeValidationServices& validationServices) const
{
    return validationServices.CaptureShaderShutdownInflightFingerprint();
}

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

bool RuntimeTestHooks::BeginShaderAutoReloadTest(
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

        shaderAutoReloadTestSourcePath =
            sourcePath.string();
        shaderAutoReloadTestVertexSourcePath =
            (std::filesystem::path(
                CommonFunction::GetProjectPath()) /
                "shader" / "glsl" / "runtimeTest" /
                "mf_shaderReloadTestVertex.glsl").string();
        shaderAutoReloadTestSurfaceSourcePath =
            (std::filesystem::path(
                CommonFunction::GetProjectPath()) /
                "shader" / "glsl" / "runtimeTest" /
                "mf_shaderReloadTestSurface.glsl").string();
        shaderAutoReloadTestOriginalSource =
            ReadTextFileBytes(sourcePath);
        shaderAutoReloadTestOriginalVertexSource =
            ReadTextFileBytes(shaderAutoReloadTestVertexSourcePath);
        shaderAutoReloadTestOriginalSurfaceSource =
            ReadTextFileBytes(shaderAutoReloadTestSurfaceSourcePath);
        shaderAutoReloadTestCompatibleSourceA =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.bgr * 0.9");
        shaderAutoReloadTestSyntaxErrorSource =
            BuildShaderReloadSyntaxErrorSource();
        shaderAutoReloadTestCompatibleSourceB =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.rgb * 0.5");
        shaderAutoReloadTestRapidSourceC1 =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.rgb * 0.75");
        shaderAutoReloadTestRapidSourceC2 =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.rgb * 0.45");
        shaderAutoReloadTestRapidSourceC3 =
            BuildShaderReloadCompatibleSource(
                "u_reloadTestColor.rgb * 0.25");
        shaderAutoReloadTestLeafSourceA =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalVertexSource,
                "return CreateDefaultMaterialVertex(vertexInput);",
                "MaterialVertex result = CreateDefaultMaterialVertex(vertexInput);\n"
                "    result.localPosition += vec3(0.0);\n"
                "    return result;");
        shaderAutoReloadTestLeafSourceB =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalVertexSource,
                "return CreateDefaultMaterialVertex(vertexInput);",
                "MaterialVertex result = CreateDefaultMaterialVertex(vertexInput);\n"
                "    result.localPosition += vec3(0.001);\n"
                "    return result;");
        shaderAutoReloadTestLeafSourceC =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalVertexSource,
                "return CreateDefaultMaterialVertex(vertexInput);",
                "MaterialVertex result = CreateDefaultMaterialVertex(vertexInput);\n"
                "    result.localPosition += vec3(0.002);\n"
                "    return result;");
        shaderAutoReloadTestSurfaceSourceA =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalSurfaceSource,
                "surface.opacity = u_reloadTestColor.a;",
                "surface.opacity = u_reloadTestColor.a * 0.99;");
        shaderAutoReloadTestSurfaceSourceB =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalSurfaceSource,
                "surface.opacity = u_reloadTestColor.a;",
                "surface.opacity = u_reloadTestColor.a * 0.98;");
        shaderAutoReloadTestSurfaceSourceC =
            ReplaceFirstOccurrence(
                shaderAutoReloadTestOriginalSurfaceSource,
                "surface.opacity = u_reloadTestColor.a;",
                "surface.opacity = u_reloadTestColor.a * 0.97;");
        shaderAutoReloadTestMtimeOnlySource =
            shaderAutoReloadTestOriginalSource;
        shaderAutoReloadTestScenePath =
            scenePath.string();
        shaderAutoReloadTestFixtureDirectory =
            scenePath.parent_path().string();
        shaderAutoReloadTestDeadline = {};
        shaderAutoReloadTestRetireDrainFramesRemaining = 0;
        shaderAutoReloadTestMaxPendingRetiredResources = 0;
        shaderAutoReloadTestBaselineLatestGeneration = 0;
        shaderAutoReloadTestBaselineSurfaceGeneration.clear();
        shaderAutoReloadTestBaselineShadowGeneration.clear();
        shaderAutoReloadTestBaselineManifestDigest.clear();
        shaderAutoReloadTestBaselineResolvedGeneration.clear();
        shaderAutoReloadTestLastObservedSurfaceGeneration.clear();
        shaderAutoReloadTestCommitTransitions = 0;
        shaderAutoReloadTestPhaseEntryPending = true;
        shaderAutoReloadTestQueueManualReload = false;
        shaderAutoReloadTestManualReloadQueued = false;
        shaderAutoReloadTestUnionSources.clear();
        shaderAutoReloadTestLastSubmittedSources.clear();
        shaderAutoReloadTestBaselineObservedEpoch = 0;
        shaderAutoReloadTestBaselineSubmittedGeneration = 0;
        shaderAutoReloadTestBaselineCommittedGeneration = 0;
        shaderAutoReloadTestBaselineFailedGeneration = 0;
        shaderAutoReloadTestBaselineShadercInvocations = 0;
        shaderAutoReloadTestBaselineMonitorScanCount = 0;
        shaderAutoReloadTestGateGeneration = 0;
        shaderAutoReloadTestDeleteRejectedGeneration = 0;
        waitingForShaderAutoReloadTestWorld = false;
        shaderAutoReloadTestPhase =
            ShaderAutoReloadTestPhase::WaitWorldLoad;
        shaderAutoReloadTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader auto reload runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create shader auto reload runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            shaderAutoReloadTestFixtureDirectory,
            diagnostics);
        shaderAutoReloadTestFixtureDirectory.clear();
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

bool RuntimeTestHooks::BeginShaderDefinitionReloadTest(
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
            std::filesystem::path(CommonFunction::GetProjectPath()) /
            "shader" /
            "glsl" /
            "runtimeTest" /
            "M_shaderReloadTest.json";
        const std::filesystem::path batchSourcePath =
            sourcePath.parent_path() /
            "M_shaderReloadBatchTest.json";
        const std::filesystem::path scenePath =
            CreateShaderReloadTestScene(resourcePath);

        shaderDefinitionReloadTestSourcePath =
            sourcePath.string();
        shaderDefinitionReloadTestBatchSourcePath =
            batchSourcePath.string();
        shaderDefinitionReloadTestOriginal =
            ReadTextFileBytes(sourcePath);
        shaderDefinitionReloadTestBatchOriginal =
            ReadTextFileBytes(batchSourcePath);
        nlohmann::json extendedJson =
            nlohmann::json::parse(
                shaderDefinitionReloadTestOriginal);
        extendedJson["parameters"]["u_reloadExtra"] = {
            {"type", "float"},
            {"default", 0.5}};
        extendedJson["parameters"]["u_reloadRemoved"] = {
            {"type", "float"},
            {"default", 0.375}};
        shaderDefinitionReloadTestExtended =
            extendedJson.dump(2) + "\n";

        nlohmann::json deletedJson =
            extendedJson;
        deletedJson["parameters"].erase(
            "u_reloadRemoved");
        deletedJson["textures"].erase(
            "u_zReloadRetiredTexture");
        shaderDefinitionReloadTestDeleted =
            deletedJson.dump(2) + "\n";

        nlohmann::json mismatchJson =
            deletedJson;
        mismatchJson["parameters"]["u_reloadRuntimeScalar"] = {
            {"type", "vec2"},
            {"default", nlohmann::json::array({0.125, 0.25})}};
        shaderDefinitionReloadTestTypeMismatch =
            mismatchJson.dump(2) + "\n";

        nlohmann::json requiredTextureJson =
            deletedJson;
        requiredTextureJson["macros"][
            "USE_RELOAD_REQUIRED_TEXTURE"] = 1;
        requiredTextureJson["textures"][
            "u_zReloadRequiredTexture"] = {
                {"type", "sampler2D"},
                {"default", nullptr}};
        shaderDefinitionReloadTestRequiredTextureMissing =
            requiredTextureJson.dump(2) + "\n";

        shaderDefinitionReloadTestMalformed =
            "{\n  \"name\": \"M_shaderReloadTest\",\n";

        nlohmann::json includeFailureJson =
            deletedJson;
        includeFailureJson["parameters"][
            "u_reloadIncludeFailure"] = {
                {"type", "float"},
                {"default", "not-a-float"}};
        shaderDefinitionReloadTestIncludeGenerationFailure =
            includeFailureJson.dump(2) + "\n";

        nlohmann::json multiMainJson =
            deletedJson;
        multiMainJson["parameters"][
            "u_reloadMultiMain"] = {
                {"type", "float"},
                {"default", 0.625}};
        shaderDefinitionReloadTestMultiMain =
            multiMainJson.dump(2) + "\n";

        nlohmann::json multiBatchJson =
            nlohmann::json::parse(
                shaderDefinitionReloadTestBatchOriginal);
        multiBatchJson["parameters"][
            "u_reloadBatchExtra"] = {
                {"type", "float"},
                {"default", 0.75}};
        shaderDefinitionReloadTestMultiBatchValid =
            multiBatchJson.dump(2) + "\n";

        nlohmann::json multiInvalidBatch =
            multiBatchJson;
        multiInvalidBatch["parameters"][
            "u_reloadBatchInvalid"] = {
                {"type", "vec3"},
                {"default", 1.0}};
        shaderDefinitionReloadTestMultiBatchInvalid =
            multiInvalidBatch.dump(2) + "\n";

        shaderDefinitionReloadTestScenePath =
            scenePath.string();
        shaderDefinitionReloadTestFixtureDirectory =
            scenePath.parent_path().string();
        shaderDefinitionReloadTestDeadline = {};
        shaderDefinitionReloadTestRetireDrainFramesRemaining = 0;
        shaderDefinitionReloadTestMaxPendingRetiredResources = 0;
        shaderDefinitionReloadTestBaselineWorldGeneration = 0;
        shaderDefinitionReloadTestBaselineFailedGeneration = 0;
        shaderDefinitionReloadTestBaselineCommittedGeneration = 0;
        shaderDefinitionReloadTestBaselineParameterCount = 0;
        shaderDefinitionReloadTestBaselineFingerprint.clear();
        shaderDefinitionReloadTestOldMaterial.reset();
        shaderDefinitionReloadTestOldMaterialInstance.reset();
        shaderDefinitionReloadTestOldObjectResources.reset();
        shaderDefinitionReloadTestOldPrimaryTexture.reset();
        shaderDefinitionReloadTestOldRetiredTexture.reset();
        shaderDefinitionReloadTestRetainedTexture.reset();
        shaderDefinitionReloadTestRetainedTextureIdentity = 0;
        shaderDefinitionReloadTestRetainedTextureAssetIdentity.clear();
        shaderDefinitionReloadTestInitialDescriptorPoolIdentity = 0;
        waitingForShaderDefinitionReloadTestWorld = false;
        shaderDefinitionReloadTestPhase =
            ShaderDefinitionReloadTestPhase::WaitWorldLoad;
        shaderDefinitionReloadTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader material definition reload runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create shader definition reload runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            shaderDefinitionReloadTestFixtureDirectory,
            diagnostics);
        shaderDefinitionReloadTestFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginShaderUiReloadTest(
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
        const std::filesystem::path vertexPath =
            shaderRoot / "uiOverlay.vert";
        const std::filesystem::path fragmentPath =
            shaderRoot / "uiOverlay.frag";
        const std::filesystem::path scenePath =
            CreateShaderReloadTestScene(resourcePath);

        shaderUiReloadTestVertexPath = vertexPath.string();
        shaderUiReloadTestFragmentPath = fragmentPath.string();
        shaderUiReloadTestVertexOriginal =
            ReadTextFileBytes(vertexPath);
        shaderUiReloadTestFragmentOriginal =
            ReadTextFileBytes(fragmentPath);
        shaderUiReloadTestFragmentCompatible =
            ReplaceFirstOccurrence(
                shaderUiReloadTestFragmentOriginal,
                "outColor = texture(uiTexture, inTexCoord) * inColor;",
                "outColor = texture(uiTexture, inTexCoord) * inColor * 0.9995;");
        shaderUiReloadTestFragmentAbiIncompatible =
            ReplaceFirstOccurrence(
                ReplaceFirstOccurrence(
                    shaderUiReloadTestFragmentOriginal,
                    "layout(location = 0) out vec4 outColor;",
                    "layout(location = 0) out vec3 outColor;"),
                "outColor = texture(uiTexture, inTexCoord) * inColor;",
                "outColor = (texture(uiTexture, inTexCoord) * inColor).rgb;");

        shaderUiReloadTestScenePath = scenePath.string();
        shaderUiReloadTestFixtureDirectory =
            scenePath.parent_path().string();
        shaderUiReloadTestDeadline = {};
        shaderUiReloadTestRetireDrainFramesRemaining = 0;
        shaderUiReloadTestMaxPendingRetiredResources = 0;
        shaderUiReloadTestBaselineLatestGeneration = 0;
        shaderUiReloadTestPhaseEntryPending = false;
        waitingForShaderUiReloadTestWorld = false;
        shaderUiReloadTestPhase =
            ShaderUiReloadTestPhase::WaitWorldLoad;
        shaderUiReloadTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader UI overlay reload runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to create shader UI reload runtime test fixture: ") +
            exception.what());
        CleanupGeneratedRuntimeFixture(
            shaderUiReloadTestFixtureDirectory,
            diagnostics);
        shaderUiReloadTestFixtureDirectory.clear();
        return false;
    }
}

bool RuntimeTestHooks::BeginShaderShutdownInflightTest(
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
            "shader" / "glsl" / "uiOverlay.frag";
        shaderShutdownInflightSourcePath =
            sourcePath.string();
        shaderShutdownInflightOriginalSource =
            ReadTextFileBytes(sourcePath);
        shaderShutdownInflightCandidateSource =
            ReplaceFirstOccurrence(
                shaderShutdownInflightOriginalSource,
                "outColor = texture(uiTexture, inTexCoord) * inColor;",
                "outColor = texture(uiTexture, inTexCoord) * inColor * 0.99925;");
        shaderShutdownInflightBaselineFingerprint.clear();
        shaderShutdownInflightBaselineCommittedGeneration = 0;
        shaderShutdownInflightCandidateGeneration = 0;
        shaderShutdownInflightDeadline =
            std::chrono::steady_clock::now() +
            ShaderAsyncWaitTimeout;
        shaderShutdownInflightTestPhase =
            ShaderShutdownInflightTestPhase::WaitMonitorBaseline;
        shaderShutdownInflightTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Shader shutdown-in-flight runtime test started.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string(
                "Failed to initialize shader shutdown-in-flight test: ") +
            exception.what());
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

void RuntimeTestHooks::FailShaderAutoReloadTest(
    RuntimeValidationServices* validationServices,
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (validationServices)
    {
        validationServices->DisableShaderCompileGate();
    }
    if (!shaderAutoReloadTestSourcePath.empty() &&
        !shaderAutoReloadTestOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestSourcePath,
                shaderAutoReloadTestOriginalSource);
        }
        catch (const std::exception& exception)
        {
            diagnostics.ReportError(
                std::string(
                    "Shader auto reload runtime test could not restore "
                    "the source fixture: ") +
                exception.what());
        }
    }
    if (!shaderAutoReloadTestVertexSourcePath.empty() &&
        !shaderAutoReloadTestOriginalVertexSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestVertexSourcePath,
                shaderAutoReloadTestOriginalVertexSource);
        }
        catch (const std::exception& exception)
        {
            diagnostics.ReportError(
                std::string(
                    "Shader auto reload runtime test could not restore "
                    "the vertex fixture: ") +
                exception.what());
        }
    }
    if (!shaderAutoReloadTestSurfaceSourcePath.empty() &&
        !shaderAutoReloadTestOriginalSurfaceSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderAutoReloadTestSurfaceSourcePath,
                shaderAutoReloadTestOriginalSurfaceSource);
        }
        catch (const std::exception& exception)
        {
            diagnostics.ReportError(
                std::string(
                    "Shader auto reload runtime test could not restore "
                    "the surface fixture: ") +
                exception.what());
        }
    }
    shaderAutoReloadTestSourcePath.clear();
    shaderAutoReloadTestVertexSourcePath.clear();
    shaderAutoReloadTestSurfaceSourcePath.clear();
    shaderAutoReloadTestOriginalSource.clear();
    shaderAutoReloadTestOriginalVertexSource.clear();
    shaderAutoReloadTestOriginalSurfaceSource.clear();
    shaderAutoReloadTestActive = false;
    waitingForShaderAutoReloadTestWorld = false;
    shaderAutoReloadTestPhaseEntryPending = false;
    shaderAutoReloadTestPhase =
        ShaderAutoReloadTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        shaderAutoReloadTestFixtureDirectory,
        diagnostics);
    shaderAutoReloadTestFixtureDirectory.clear();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::UpdateShaderAutoReloadTest(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderAutoReloadTestActive)
    {
        return;
    }

    const std::string sharedIdentity =
        "runtimeTest/shaderReloadTestShared.glsl";
    const std::string vertexIdentity =
        "runtimeTest/mf_shaderReloadTestVertex.glsl";
    const std::string surfaceIdentity =
        "runtimeTest/mf_shaderReloadTestSurface.glsl";
    const std::vector<std::string> unionSources = {
        sharedIdentity,
        surfaceIdentity,
        vertexIdentity};

    try
    {
        if (shaderAutoReloadTestPhase ==
            ShaderAutoReloadTestPhase::WaitRetireDrain)
        {
            ResourceRetireQueue& retireQueue =
                ResourceRetireQueue::GetInstance();
            const size_t pending = retireQueue.GetPendingCount();
            shaderAutoReloadTestMaxPendingRetiredResources =
                std::max(
                    shaderAutoReloadTestMaxPendingRetiredResources,
                    pending);
            if (pending == 0)
            {
                if (shaderAutoReloadTestMaxPendingRetiredResources == 0)
                {
                    throw std::runtime_error(
                        "automatic reload matrix did not observe an "
                        "epoch-retired pipeline");
                }

                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestOriginalSource);
                WriteTextFileAtomically(
                    shaderAutoReloadTestVertexSourcePath,
                    shaderAutoReloadTestOriginalVertexSource);
                WriteTextFileAtomically(
                    shaderAutoReloadTestSurfaceSourcePath,
                    shaderAutoReloadTestOriginalSurfaceSource);
                CleanupGeneratedRuntimeFixture(
                    shaderAutoReloadTestFixtureDirectory,
                    diagnostics);
                shaderAutoReloadTestFixtureDirectory.clear();
                shaderAutoReloadTestActive = false;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::Idle;
                runtimeTestStatus = RuntimeTestStatus::Succeeded;
                diagnostics.ReportInfo(
                    "Shader auto reload runtime test completed: "
                    "in-flight stale generations, A1/A2/A3 coalescing, "
                    "independent source union, include/leaf dependency "
                    "updates, frozen-include deletion rollback, "
                    "mtime-only scan suppression, syntax failure/recovery, "
                    "manual/auto supersession, and epoch retirement passed.");
                return;
            }

            --shaderAutoReloadTestRetireDrainFramesRemaining;
            if (shaderAutoReloadTestRetireDrainFramesRemaining <= 0)
            {
                throw std::runtime_error(
                    "timed out waiting for retired pipelines to drain; "
                    "pending=" + std::to_string(pending));
            }
            return;
        }

        if (shaderAutoReloadTestPhaseEntryPending)
        {
            shaderAutoReloadTestPhaseEntryPending = false;
            shaderAutoReloadTestDeadline =
                std::chrono::steady_clock::now() +
                ShaderAsyncWaitTimeout;

            switch (shaderAutoReloadTestPhase)
            {
            case ShaderAutoReloadTestPhase::WaitA1Gate:
            {
                if (!validationServices
                         .CaptureShaderReloadState()
                         .workerRunning)
                {
                    throw std::runtime_error(
                        "automatic reload matrix lost monitor or worker");
                }
                validationServices.SetShaderMonitorPollInterval(
                    std::chrono::milliseconds(0));
                const ShaderAutoReloadRuntimeSnapshot baseline =
                    CaptureShaderAutoReloadRuntimeSnapshot(validationServices);
                shaderAutoReloadTestInitialSurfaceGeneration =
                    baseline.surfaceGeneration;
                shaderAutoReloadTestInitialShadowGeneration =
                    baseline.shadowGeneration;
                shaderAutoReloadTestInitialManifestDigest =
                    baseline.manifestDigest;
                shaderAutoReloadTestInitialResolvedGeneration =
                    baseline.resolvedGeneration;
                shaderAutoReloadTestBaselineObservedEpoch =
                    validationServices.CaptureShaderReloadState().latestObservedSourceEpoch;
                validationServices.ArmShaderCompileGate();
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestCompatibleSourceA);
                diagnostics.ReportInfo(
                    "Shader auto reload matrix phase A1: "
                    "wrote A1 and armed the worker gate.");
                break;
            }
            case ShaderAutoReloadTestPhase::WaitA2Stable:
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC1);
                shaderAutoReloadTestBaselineObservedEpoch =
                    validationServices.CaptureShaderReloadState().latestObservedSourceEpoch;
                break;
            case ShaderAutoReloadTestPhase::WaitA3Stable:
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC2);
                shaderAutoReloadTestBaselineObservedEpoch =
                    validationServices.CaptureShaderReloadState().latestObservedSourceEpoch;
                break;
            case ShaderAutoReloadTestPhase::WaitSyntaxFailure:
                shaderAutoReloadTestBaselineManifestDigest =
                    CaptureShaderAutoReloadRuntimeSnapshot(validationServices)
                        .manifestDigest;
                shaderAutoReloadTestBaselineResolvedGeneration =
                    CaptureShaderAutoReloadRuntimeSnapshot(validationServices)
                        .resolvedGeneration;
                shaderAutoReloadTestBaselineFailedGeneration =
                    validationServices.CaptureShaderReloadState().latestAutoReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestSyntaxErrorSource);
                break;
            case ShaderAutoReloadTestPhase::WaitSyntaxRecovery:
                shaderAutoReloadTestBaselineCommittedGeneration =
                    validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration;
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestCompatibleSourceB);
                break;
            case ShaderAutoReloadTestPhase::WaitUnionGate:
                validationServices.ArmShaderCompileGate();
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC3);
                break;
            case ShaderAutoReloadTestPhase::WaitUnionYStable:
                WriteTextFileAtomically(
                    shaderAutoReloadTestVertexSourcePath,
                    shaderAutoReloadTestLeafSourceA);
                shaderAutoReloadTestBaselineObservedEpoch =
                    validationServices.CaptureShaderReloadState().latestObservedSourceEpoch;
                break;
            case ShaderAutoReloadTestPhase::WaitUnionZStable:
                WriteTextFileAtomically(
                    shaderAutoReloadTestSurfaceSourcePath,
                    shaderAutoReloadTestSurfaceSourceA);
                shaderAutoReloadTestBaselineObservedEpoch =
                    validationServices.CaptureShaderReloadState().latestObservedSourceEpoch;
                break;
            case ShaderAutoReloadTestPhase::WaitDeleteGate:
            {
                const ShaderAutoReloadRuntimeSnapshot baseline =
                    CaptureShaderAutoReloadRuntimeSnapshot(validationServices);
                shaderAutoReloadTestBaselineManifestDigest =
                    baseline.manifestDigest;
                shaderAutoReloadTestBaselineResolvedGeneration =
                    baseline.resolvedGeneration;
                shaderAutoReloadTestBaselineFailedGeneration =
                    validationServices.CaptureShaderReloadState().latestAutoReloadFailedGeneration;
                validationServices.ArmShaderCompileGate();
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC1);
                break;
            }
            case ShaderAutoReloadTestPhase::WaitDeleteCandidateRejected:
                if (std::filesystem::exists(
                        shaderAutoReloadTestSourcePath))
                {
                    throw std::runtime_error(
                        "delete phase expected the frozen include source "
                        "to be absent");
                }
                break;
            case ShaderAutoReloadTestPhase::WaitDeleteFailure:
                break;
            case ShaderAutoReloadTestPhase::WaitDeleteRecovery:
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestOriginalSource);
                shaderAutoReloadTestBaselineCommittedGeneration =
                    validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration;
                break;
            case ShaderAutoReloadTestPhase::WaitMtimeOnlyScan:
            {
                const std::filesystem::path path =
                    shaderAutoReloadTestSourcePath;
                shaderAutoReloadTestOriginalWriteTime =
                    std::filesystem::last_write_time(path);
                shaderAutoReloadTestOriginalWriteTimeCaptured = true;
                shaderAutoReloadTestBaselineObservedEpoch =
                    validationServices.CaptureShaderReloadState().latestObservedSourceEpoch;
                shaderAutoReloadTestBaselineSubmittedGeneration =
                    validationServices.CaptureShaderReloadState().latestSubmittedAutoReloadGeneration;
                shaderAutoReloadTestBaselineCommittedGeneration =
                    validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration;
                shaderAutoReloadTestBaselineShadercInvocations =
                    validationServices.CaptureShaderReloadState().totalAutoReloadShadercInvocations;
                shaderAutoReloadTestBaselineMonitorScanCount =
                    validationServices.CaptureShaderReloadState().monitorScanCount;
                std::filesystem::last_write_time(
                    path,
                    shaderAutoReloadTestOriginalWriteTime +
                        std::chrono::seconds(1));
                break;
            }
            case ShaderAutoReloadTestPhase::WaitManualGate:
                validationServices.ArmShaderCompileGate();
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC2);
                break;
            case ShaderAutoReloadTestPhase::WaitManualCommit:
            {
                const ShaderAutoReloadRuntimeSnapshot baseline =
                    CaptureShaderAutoReloadRuntimeSnapshot(validationServices);
                shaderAutoReloadTestBaselineSurfaceGeneration =
                    baseline.surfaceGeneration;
                shaderAutoReloadTestBaselineShadowGeneration =
                    baseline.shadowGeneration;
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestRapidSourceC3);
                shaderAutoReloadTestQueueManualReload = true;
                shaderAutoReloadTestManualReloadQueued = false;
                shaderAutoReloadTestBaselineCommittedGeneration =
                    validationServices.CaptureShaderReloadState().latestManualShaderReloadCommittedGeneration;
                RuntimeCommand command;
                command.type = RuntimeCommandType::ReloadShaders;
                command.shaderReloadScope =
                    RuntimeShaderReloadScope::Changed;
                command.sourceText =
                    "runtime-test: manual shader reload supersession";
                validationServices.QueueRuntimeCommand(
                    std::move(command));
                shaderAutoReloadTestManualReloadQueued = true;
                break;
            }
            case ShaderAutoReloadTestPhase::RestoreOriginal:
                WriteTextFileAtomically(
                    shaderAutoReloadTestSourcePath,
                    shaderAutoReloadTestOriginalSource);
                WriteTextFileAtomically(
                    shaderAutoReloadTestVertexSourcePath,
                    shaderAutoReloadTestOriginalVertexSource);
                WriteTextFileAtomically(
                    shaderAutoReloadTestSurfaceSourcePath,
                    shaderAutoReloadTestOriginalSurfaceSource);
                shaderAutoReloadTestBaselineCommittedGeneration =
                    validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration;
                break;
            default:
                break;
            }
        }

        const ShaderAutoReloadRuntimeSnapshot snapshot =
            CaptureShaderAutoReloadRuntimeSnapshot(validationServices);
        const bool workerAtGate =
            validationServices.IsShaderCompileGateWaiting();
        const bool pendingShared =
            validationServices.CaptureShaderReloadState().pendingAutoReloadSources.count(
                sharedIdentity) != 0;
        const bool pendingVertex =
            validationServices.CaptureShaderReloadState().pendingAutoReloadSources.count(
                vertexIdentity) != 0;
        const bool pendingSurface =
            validationServices.CaptureShaderReloadState().pendingAutoReloadSources.count(
                surfaceIdentity) != 0;

        switch (shaderAutoReloadTestPhase)
        {
        case ShaderAutoReloadTestPhase::WaitA1Gate:
            if (workerAtGate &&
                validationServices.CaptureShaderReloadState().inFlightAutoReloadGeneration != 0)
            {
                shaderAutoReloadTestGateGeneration =
                    validationServices.CaptureShaderReloadState().inFlightAutoReloadGeneration;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitA2Stable;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitA2Stable:
            if (pendingShared &&
                validationServices.CaptureShaderReloadState().latestObservedSourceEpoch >
                    shaderAutoReloadTestBaselineObservedEpoch)
            {
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitA3Stable;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitA3Stable:
            if (pendingShared &&
                validationServices.CaptureShaderReloadState().latestObservedSourceEpoch >
                    shaderAutoReloadTestBaselineObservedEpoch)
            {
                validationServices.ReleaseShaderCompileGate();
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitA1Stale;
                shaderAutoReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitA1Stale:
            if (validationServices.CaptureShaderReloadState().latestAutoReloadStaleDiscardGeneration ==
                shaderAutoReloadTestGateGeneration)
            {
                if (!SameShaderAutoReloadRuntimeSnapshot(
                        snapshot,
                        ShaderAutoReloadRuntimeSnapshot{
                            {},
                            {},
                            shaderAutoReloadTestInitialSurfaceGeneration,
                            shaderAutoReloadTestInitialShadowGeneration,
                            shaderAutoReloadTestInitialManifestDigest,
                            shaderAutoReloadTestInitialResolvedGeneration}))
                {
                    throw std::runtime_error(
                        "A1 stale result changed live pipeline, manifest, "
                        "or resolved draw generation");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed A1 in-flight stale "
                    "discard after A2/A3.");
                shaderAutoReloadTestBaselineCommittedGeneration =
                    validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitA3Commit;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitA3Commit:
            if (validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration >
                    shaderAutoReloadTestBaselineCommittedGeneration &&
                snapshot.surfaceGeneration !=
                    shaderAutoReloadTestInitialSurfaceGeneration)
            {
                const std::string expectedDigest =
                    ContentHasher::HashFile(
                        shaderAutoReloadTestSourcePath).ToHex();
                if (!SurfaceArtifactDependsOnSourceWithDigest(
                        validationServices,
                        snapshot.surfaceLogicalBuildId,
                        sharedIdentity,
                        expectedDigest))
                {
                    throw std::runtime_error(
                        "A3 commit does not match the final source digest");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed A3 final-source "
                    "commit.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitSyntaxFailure;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitSyntaxFailure:
            if (validationServices.CaptureShaderReloadState().latestAutoReloadFailedGeneration >
                    shaderAutoReloadTestBaselineFailedGeneration)
            {
                if (snapshot.manifestDigest !=
                        shaderAutoReloadTestBaselineManifestDigest ||
                    snapshot.resolvedGeneration !=
                        shaderAutoReloadTestBaselineResolvedGeneration)
                {
                    throw std::runtime_error(
                        "syntax failure changed live artifacts");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed syntax failure "
                    "retention with pending source preserved.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitSyntaxRecovery;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitSyntaxRecovery:
            if (validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration >
                    shaderAutoReloadTestBaselineCommittedGeneration &&
                snapshot.surfaceGeneration !=
                    shaderAutoReloadTestInitialSurfaceGeneration)
            {
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed syntax recovery.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitUnionGate;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitUnionGate:
            if (workerAtGate &&
                validationServices.CaptureShaderReloadState().inFlightAutoReloadGeneration != 0)
            {
                shaderAutoReloadTestGateGeneration =
                    validationServices.CaptureShaderReloadState().inFlightAutoReloadGeneration;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitUnionYStable;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitUnionYStable:
            if (pendingVertex &&
                validationServices.CaptureShaderReloadState().latestObservedSourceEpoch >
                    shaderAutoReloadTestBaselineObservedEpoch)
            {
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitUnionZStable;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitUnionZStable:
            if (pendingSurface &&
                validationServices.CaptureShaderReloadState().latestObservedSourceEpoch >
                    shaderAutoReloadTestBaselineObservedEpoch)
            {
                validationServices.ReleaseShaderCompileGate();
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitUnionStale;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitUnionStale:
            if (validationServices.CaptureShaderReloadState().latestAutoReloadStaleDiscardGeneration ==
                shaderAutoReloadTestGateGeneration)
            {
                shaderAutoReloadTestLastSubmittedSources =
                    validationServices.CaptureShaderReloadState().lastSubmittedAutoReloadSources;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitUnionCommit;
                shaderAutoReloadTestBaselineCommittedGeneration =
                    validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitUnionCommit:
            if (validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration >
                    shaderAutoReloadTestBaselineCommittedGeneration)
            {
                if (!ContainsAllSourceIdentities(
                        validationServices.CaptureShaderReloadState().lastSubmittedAutoReloadSources,
                        unionSources))
                {
                    throw std::runtime_error(
                        "independent source changes were not submitted as "
                        "one pending union");
                }
                if (!validationServices
                         .ManifestArtifactsDependOnAllSources(
                             {
                                 snapshot.surfaceLogicalBuildId,
                                 snapshot.shadowLogicalBuildId},
                             unionSources))
                {
                    throw std::runtime_error(
                        "surface/shadow dependency artifacts did not both "
                        "update for include and leaf changes");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed independent-source "
                    "pending union and Surface/Shadow dependency update.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitDeleteGate;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitDeleteGate:
            if (workerAtGate &&
                validationServices.CaptureShaderReloadState().inFlightAutoReloadGeneration != 0)
            {
                shaderAutoReloadTestGateGeneration =
                    validationServices.CaptureShaderReloadState().inFlightAutoReloadGeneration;
                shaderAutoReloadTestBaselineObservedEpoch =
                    validationServices.CaptureShaderReloadState().latestObservedSourceEpoch;
                std::filesystem::remove(
                    shaderAutoReloadTestSourcePath);
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitDeleteCandidateRejected;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitDeleteCandidateRejected:
            if (pendingShared &&
                validationServices.CaptureShaderReloadState().latestObservedSourceEpoch >
                    shaderAutoReloadTestBaselineObservedEpoch)
            {
                validationServices.ReleaseShaderCompileGate();
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitDeleteFailure;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitDeleteFailure:
            if (validationServices.CaptureShaderReloadState().latestAutoReloadStaleDiscardGeneration ==
                    shaderAutoReloadTestGateGeneration &&
                validationServices.CaptureShaderReloadState().latestAutoReloadFailedGeneration >
                    shaderAutoReloadTestBaselineFailedGeneration)
            {
                if (snapshot.manifestDigest !=
                        shaderAutoReloadTestBaselineManifestDigest ||
                    snapshot.resolvedGeneration !=
                        shaderAutoReloadTestBaselineResolvedGeneration)
                {
                    throw std::runtime_error(
                        "deleted include changed the live state");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed frozen-include "
                    "stale discard and stable deletion failure.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitDeleteRecovery;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitDeleteRecovery:
            if (validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration >
                    shaderAutoReloadTestBaselineCommittedGeneration)
            {
                if (!std::filesystem::exists(
                        shaderAutoReloadTestSourcePath))
                {
                    throw std::runtime_error(
                        "delete recovery did not restore the include file");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed include deletion "
                    "recovery.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitMtimeOnlyScan;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitMtimeOnlyScan:
            if (validationServices.CaptureShaderReloadState().monitorScanCount >
                shaderAutoReloadTestBaselineMonitorScanCount)
            {
                if (validationServices.CaptureShaderReloadState().latestObservedSourceEpoch !=
                        shaderAutoReloadTestBaselineObservedEpoch ||
                    validationServices.CaptureShaderReloadState().latestSubmittedAutoReloadGeneration !=
                        shaderAutoReloadTestBaselineSubmittedGeneration ||
                    validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration !=
                        shaderAutoReloadTestBaselineCommittedGeneration ||
                    validationServices.CaptureShaderReloadState().totalAutoReloadShadercInvocations !=
                        shaderAutoReloadTestBaselineShadercInvocations ||
                    !SameShaderAutoReloadRuntimeSnapshot(
                        snapshot,
                        CaptureShaderAutoReloadRuntimeSnapshot(validationServices)))
                {
                    throw std::runtime_error(
                        "mtime-only change caused source generation, "
                        "shaderc, or live artifact changes");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed mtime-only scan "
                    "suppression.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitManualGate;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitManualGate:
            if (workerAtGate &&
                validationServices.CaptureShaderReloadState().inFlightAutoReloadGeneration != 0)
            {
                shaderAutoReloadTestGateGeneration =
                    validationServices.CaptureShaderReloadState().inFlightAutoReloadGeneration;
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitManualCommit;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitManualCommit:
            if (validationServices.CaptureShaderReloadState().latestManualShaderReloadCommittedGeneration >
                shaderAutoReloadTestBaselineCommittedGeneration)
            {
                if (snapshot.surfaceGeneration ==
                        shaderAutoReloadTestInitialSurfaceGeneration)
                {
                    throw std::runtime_error(
                        "manual reload did not replace the live artifact");
                }
                shaderAutoReloadTestBaselineResolvedGeneration =
                    snapshot.resolvedGeneration;
                validationServices.ReleaseShaderCompileGate();
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitManualStale;
            }
            break;
        case ShaderAutoReloadTestPhase::WaitManualStale:
            if (validationServices.CaptureShaderReloadState().latestAutoReloadStaleDiscardGeneration ==
                shaderAutoReloadTestGateGeneration)
            {
                if (snapshot.resolvedGeneration !=
                        shaderAutoReloadTestBaselineResolvedGeneration)
                {
                    throw std::runtime_error(
                        "stale auto result overwrote manual shader reload");
                }
                diagnostics.ReportInfo(
                    "Shader auto reload matrix passed manual/auto "
                    "supersession.");
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::RestoreOriginal;
                shaderAutoReloadTestPhaseEntryPending = true;
            }
            break;
        case ShaderAutoReloadTestPhase::RestoreOriginal:
            if (validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration >
                    shaderAutoReloadTestBaselineCommittedGeneration &&
                snapshot.surfaceGeneration ==
                    shaderAutoReloadTestInitialSurfaceGeneration)
            {
                shaderAutoReloadTestPhase =
                    ShaderAutoReloadTestPhase::WaitRetireDrain;
                shaderAutoReloadTestRetireDrainFramesRemaining =
                    RetireDrainFrameBudget;
                shaderAutoReloadTestMaxPendingRetiredResources =
                    ResourceRetireQueue::GetInstance()
                        .GetPendingCount();
            }
            break;
        default:
            break;
        }

        if (std::chrono::steady_clock::now() >=
            shaderAutoReloadTestDeadline)
        {
            throw std::runtime_error(
                "automatic reload matrix timed out in phase " +
                std::to_string(
                    static_cast<int>(shaderAutoReloadTestPhase)) +
                ", pending=" +
                std::to_string(
                    validationServices.CaptureShaderReloadState().pendingAutoReloadSources.size()) +
                ", inFlight=" +
                std::to_string(
                    validationServices.CaptureShaderReloadState().inFlightAutoReloadGeneration));
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderAutoReloadTest(
            &validationServices,
            std::string(
                "Shader auto reload runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
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

void RuntimeTestHooks::FailShaderDefinitionReloadTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderDefinitionReloadTestSourcePath.empty() &&
        !shaderDefinitionReloadTestOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderDefinitionReloadTestSourcePath,
                shaderDefinitionReloadTestOriginal);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    shaderDefinitionReloadTestSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    if (!shaderDefinitionReloadTestBatchSourcePath.empty() &&
        !shaderDefinitionReloadTestBatchOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderDefinitionReloadTestBatchSourcePath,
                shaderDefinitionReloadTestBatchOriginal);
            try
            {
                MaterialParameterIncludeGenerator::GenerateInclude(
                    shaderDefinitionReloadTestBatchSourcePath);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }
    shaderDefinitionReloadTestActive = false;
    waitingForShaderDefinitionReloadTestWorld = false;
    shaderDefinitionReloadTestPhaseEntryPending = false;
    shaderDefinitionReloadTestPhase =
        ShaderDefinitionReloadTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        shaderDefinitionReloadTestFixtureDirectory,
        diagnostics);
    shaderDefinitionReloadTestFixtureDirectory.clear();
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

void RuntimeTestHooks::UpdateShaderDefinitionReloadTest(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderDefinitionReloadTestActive)
    {
        return;
    }

    if (shaderDefinitionReloadTestPhase ==
        ShaderDefinitionReloadTestPhase::WaitRetireDrain)
    {
        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        const size_t pending = retireQueue.GetPendingCount();
        shaderDefinitionReloadTestMaxPendingRetiredResources =
            std::max(
                shaderDefinitionReloadTestMaxPendingRetiredResources,
                pending);
        if (pending == 0)
        {
            if (shaderDefinitionReloadTestMaxPendingRetiredResources == 0)
            {
                FailShaderDefinitionReloadTest(
                    "Shader definition reload runtime test did not observe "
                    "any epoch-retired material resources.",
                    diagnostics);
                return;
            }
            if (!shaderDefinitionReloadTestOldMaterial.expired() ||
                !shaderDefinitionReloadTestOldMaterialInstance.expired() ||
                !shaderDefinitionReloadTestOldObjectResources.expired() ||
                !shaderDefinitionReloadTestOldPrimaryTexture.expired() ||
                !shaderDefinitionReloadTestOldRetiredTexture.expired())
            {
                FailShaderDefinitionReloadTest(
                    "Shader definition reload runtime test drained the "
                    "retire queue while old Material/MI/Texture/descriptor "
                    "owners were still alive.",
                    diagnostics);
                return;
            }
            if (shaderDefinitionReloadTestRetainedTexture.expired())
            {
                FailShaderDefinitionReloadTest(
                    "Shader definition reload runtime test lost the "
                    "compatible migrated Texture after retirement drain.",
                    diagnostics);
                return;
            }
            ValidateShaderDefinitionMigratedState(
                FindShaderReloadTestMaterialInstance(),
                shaderDefinitionReloadTestRetainedTextureIdentity,
                false,
                false,
                false);
            if (!validationServices.CaptureShaderReloadState().pendingMaterialDefinitionSources.empty())
            {
                FailShaderDefinitionReloadTest(
                    "Shader definition reload runtime test completed with "
                    "pending material definition sources.",
                    diagnostics);
                return;
            }
            shaderDefinitionReloadTestActive = false;
            shaderDefinitionReloadTestPhase =
                ShaderDefinitionReloadTestPhase::Idle;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            CleanupGeneratedRuntimeFixture(
                shaderDefinitionReloadTestFixtureDirectory,
                diagnostics);
            shaderDefinitionReloadTestFixtureDirectory.clear();
            diagnostics.ReportInfo(
                "Shader material definition reload runtime test completed: "
                "runtime parameter and Texture state preservation, field deletion, "
                "type/required-texture/malformed/include failures, multi-M_ "
                "all-or-nothing recovery, restore, and epoch retirement "
                "passed. retainedTexture=" +
                std::to_string(
                    shaderDefinitionReloadTestRetainedTextureIdentity) +
                ", maxRetirePending=" +
                std::to_string(
                    shaderDefinitionReloadTestMaxPendingRetiredResources) +
                ".");
            return;
        }

        --shaderDefinitionReloadTestRetireDrainFramesRemaining;
        if (shaderDefinitionReloadTestRetireDrainFramesRemaining <= 0)
        {
            FailShaderDefinitionReloadTest(
                "Shader definition reload runtime test timed out waiting "
                "for retired resources to drain. pending=" +
                    std::to_string(pending),
                diagnostics);
        }
        return;
    }

    try
    {
        if (shaderDefinitionReloadTestPhaseEntryPending)
        {
            shaderDefinitionReloadTestPhaseEntryPending = false;
            const std::shared_ptr<Material> entryMaterial =
                FindShaderReloadTestMaterial();
            const std::shared_ptr<MaterialInstance>
                entryInstance =
                    FindShaderReloadTestMaterialInstance();
            const std::shared_ptr<
                RendererObjectResourceEntry>
                    entryObjectResources =
                        FindShaderReloadTestObjectResources();
            if (!entryMaterial || !entryInstance ||
                !entryObjectResources)
            {
                throw std::runtime_error(
                    "definition fixture runtime owners are not live");
            }

            const MaterialInstanceStateSnapshot entrySnapshot =
                entryInstance->CaptureStateSnapshot();
            const auto primaryTextureIt =
                entrySnapshot.textures.find(
                    "u_reloadTexture");
            const auto alternateTextureIt =
                entrySnapshot.textures.find(
                    "u_reloadAlternateTexture");
            const auto retiredTextureIt =
                entrySnapshot.textures.find(
                    "u_zReloadRetiredTexture");
            if (primaryTextureIt ==
                    entrySnapshot.textures.end() ||
                alternateTextureIt ==
                    entrySnapshot.textures.end() ||
                retiredTextureIt ==
                    entrySnapshot.textures.end() ||
                !primaryTextureIt->second.texture ||
                !alternateTextureIt->second.texture ||
                !retiredTextureIt->second.texture)
            {
                throw std::runtime_error(
                    "definition fixture did not load all Texture bindings");
            }

            entryInstance->SetParameter(
                "u_reloadRuntimeScalar",
                0.875f);
            entryInstance->SetParameter(
                "u_reloadTestColor",
                Eigen::Vector4f(
                    0.85f,
                    0.15f,
                    0.45f,
                    0.65f));
            entryInstance->SetTexture(
                "u_reloadTexture",
                alternateTextureIt->second.texture,
                alternateTextureIt->second
                    .textureAssetIdentity,
                alternateTextureIt->second
                    .textureCacheIdentity);

            shaderDefinitionReloadTestOldMaterial =
                entryMaterial;
            shaderDefinitionReloadTestOldMaterialInstance =
                entryInstance;
            shaderDefinitionReloadTestOldObjectResources =
                entryObjectResources;
            shaderDefinitionReloadTestOldPrimaryTexture =
                primaryTextureIt->second.texture;
            shaderDefinitionReloadTestOldRetiredTexture =
                retiredTextureIt->second.texture;
            shaderDefinitionReloadTestRetainedTexture =
                alternateTextureIt->second.texture;
            shaderDefinitionReloadTestRetainedTextureIdentity =
                reinterpret_cast<std::uintptr_t>(
                    alternateTextureIt->second.texture.get());
            shaderDefinitionReloadTestRetainedTextureAssetIdentity =
                alternateTextureIt->second
                    .textureAssetIdentity.value_or(
                        std::string());
            shaderDefinitionReloadTestInitialDescriptorPoolIdentity =
                GetObjectDescriptorPoolIdentity(
                    entryObjectResources);
            shaderDefinitionReloadTestBaselineWorldGeneration =
                validationServices.GetActiveWorldHandle()
                    .generation;
            shaderDefinitionReloadTestBaselineParameterCount =
                entryMaterial
                ? static_cast<int>(
                      entryMaterial->GetMaterialDescriptorSchema()
                          .GetParameters()
                          .size())
                : 0;
            shaderDefinitionReloadTestBaselineCommittedGeneration =
                validationServices
                    .CaptureShaderReloadState()
                    .latestMaterialDefinitionReloadCommittedGeneration;
            shaderDefinitionReloadTestBaselineFingerprint =
                CaptureShaderDefinitionRuntimeFingerprint(
                    validationServices);
            WriteTextFileAtomically(
                shaderDefinitionReloadTestSourcePath,
                shaderDefinitionReloadTestExtended);
            shaderDefinitionReloadTestDeadline =
                std::chrono::steady_clock::now() +
                ShaderDefinitionWaitTimeout;
            diagnostics.ReportInfo(
                "Shader definition reload runtime test mutated live state: "
                "u_reloadRuntimeScalar=0.875, "
                "u_reloadTestColor=[0.85,0.15,0.45,0.65], "
                "u_reloadTexturePointer=" +
                std::to_string(
                    shaderDefinitionReloadTestRetainedTextureIdentity) +
                ", u_reloadTextureAsset=" +
                shaderDefinitionReloadTestRetainedTextureAssetIdentity +
                ", oldDescriptorPool=" +
                std::to_string(
                    shaderDefinitionReloadTestInitialDescriptorPoolIdentity) +
                ", baselineFingerprint=" +
                shaderDefinitionReloadTestBaselineFingerprint +
                ".");
        }

        const std::shared_ptr<Material> material =
            FindShaderReloadTestMaterial();
        const int parameterCount =
            material
            ? static_cast<int>(
                  material->GetMaterialDescriptorSchema()
                      .GetParameters()
                      .size())
            : 0;
        const ShaderReloadRuntimeStateSnapshot reloadState =
            validationServices.CaptureShaderReloadState();

        switch (shaderDefinitionReloadTestPhase)
        {
        case ShaderDefinitionReloadTestPhase::WaitAddedParameter:
            if (reloadState
                    .latestMaterialDefinitionReloadCommittedGeneration >
                shaderDefinitionReloadTestBaselineCommittedGeneration)
            {
                std::shared_ptr<MaterialInstance> instance =
                    FindShaderReloadTestMaterialInstance();
                ValidateShaderDefinitionMigratedState(
                    instance,
                    shaderDefinitionReloadTestRetainedTextureIdentity,
                    true,
                    true,
                    false);
                if (parameterCount !=
                    shaderDefinitionReloadTestBaselineParameterCount + 2)
                {
                    throw std::runtime_error(
                        "extended schema parameter count is incorrect");
                }
                const uint64_t descriptorPoolIdentity =
                    GetObjectDescriptorPoolIdentity(
                        FindShaderReloadTestObjectResources());
                if (descriptorPoolIdentity == 0 ||
                    descriptorPoolIdentity ==
                        shaderDefinitionReloadTestInitialDescriptorPoolIdentity)
                {
                    throw std::runtime_error(
                        "extended schema did not create a replacement "
                        "descriptor package");
                }
                diagnostics.ReportInfo(
                    "Shader definition reload runtime test passed added-field "
                    "state transfer: scalar=0.875, extraDefault=0.5, "
                    "removedDefault=0.375, retainedTexture=" +
                    std::to_string(
                        shaderDefinitionReloadTestRetainedTextureIdentity) +
                    ", descriptorPool=" +
                    std::to_string(
                        descriptorPoolIdentity) +
                    ", worldGeneration=" +
                    std::to_string(
                        validationServices
                            .GetActiveWorldHandle()
                            .generation) +
                    ".");
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestDeleted);
                shaderDefinitionReloadTestBaselineCommittedGeneration =
                    reloadState
                        .latestMaterialDefinitionReloadCommittedGeneration;
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitDeletedParameter;
            }
            break;

        case ShaderDefinitionReloadTestPhase::WaitDeletedParameter:
            if (reloadState
                    .latestMaterialDefinitionReloadCommittedGeneration >
                shaderDefinitionReloadTestBaselineCommittedGeneration)
            {
                const std::shared_ptr<MaterialInstance> instance =
                    FindShaderReloadTestMaterialInstance();
                ValidateShaderDefinitionMigratedState(
                    instance,
                    shaderDefinitionReloadTestRetainedTextureIdentity,
                    true,
                    false,
                    false);
                if (instance->HasTexture(
                        "u_zReloadRetiredTexture"))
                {
                    throw std::runtime_error(
                        "deleted Texture schema field survived in the "
                        "candidate MaterialInstance");
                }
                diagnostics.ReportInfo(
                    "Shader definition reload runtime test passed deleted "
                    "field removal: u_reloadRemoved absent, retiredTexture "
                    "absent, compatible runtime values retained.");
                shaderDefinitionReloadTestBaselineFingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        validationServices);
                shaderDefinitionReloadTestBaselineFailedGeneration =
                    reloadState
                        .latestMaterialDefinitionReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestTypeMismatch);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitTypeMismatchRetention;
            }
            break;

        case ShaderDefinitionReloadTestPhase::WaitTypeMismatchRetention:
            if (reloadState
                    .latestMaterialDefinitionReloadFailedGeneration >
                shaderDefinitionReloadTestBaselineFailedGeneration)
            {
                const std::string fingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        validationServices);
                if (fingerprint !=
                    shaderDefinitionReloadTestBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "type-mismatched schema changed the live package: "
                        "before=" +
                        shaderDefinitionReloadTestBaselineFingerprint +
                        ", after=" + fingerprint);
                }
                diagnostics.ReportInfo(
                    "Shader definition rollback fingerprint preserved after "
                    "same-name type mismatch: " + fingerprint + ".");
                shaderDefinitionReloadTestBaselineFailedGeneration =
                    reloadState
                        .latestMaterialDefinitionReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestRequiredTextureMissing);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitRequiredTextureFailure;
            }
            break;

        case ShaderDefinitionReloadTestPhase::WaitRequiredTextureFailure:
            if (reloadState
                    .latestMaterialDefinitionReloadFailedGeneration >
                shaderDefinitionReloadTestBaselineFailedGeneration)
            {
                const std::string fingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        validationServices);
                if (fingerprint !=
                    shaderDefinitionReloadTestBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "missing required Texture changed the live package");
                }
                diagnostics.ReportInfo(
                    "Shader definition rollback fingerprint preserved after "
                    "missing required Texture: " + fingerprint + ".");
                shaderDefinitionReloadTestBaselineFailedGeneration =
                    reloadState
                        .latestMaterialDefinitionReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestMalformed);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitMalformedDefinitionFailure;
            }
            break;

        case ShaderDefinitionReloadTestPhase::
            WaitMalformedDefinitionFailure:
            if (reloadState
                    .latestMaterialDefinitionReloadFailedGeneration >
                shaderDefinitionReloadTestBaselineFailedGeneration)
            {
                const std::string fingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        validationServices);
                if (fingerprint !=
                    shaderDefinitionReloadTestBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "malformed M_ JSON changed the live package");
                }
                diagnostics.ReportInfo(
                    "Shader definition rollback fingerprint preserved after "
                    "malformed M_ JSON: " + fingerprint + ".");
                shaderDefinitionReloadTestBaselineFailedGeneration =
                    reloadState
                        .latestMaterialDefinitionReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestIncludeGenerationFailure);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitIncludeGenerationFailure;
            }
            break;

        case ShaderDefinitionReloadTestPhase::
            WaitIncludeGenerationFailure:
            if (reloadState
                    .latestMaterialDefinitionReloadFailedGeneration >
                shaderDefinitionReloadTestBaselineFailedGeneration)
            {
                const std::string fingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        validationServices);
                if (fingerprint !=
                    shaderDefinitionReloadTestBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "generated include validation failure changed the "
                        "live package");
                }
                diagnostics.ReportInfo(
                    "Shader definition rollback fingerprint preserved after "
                    "generated include validation failure: " +
                    fingerprint + ".");
                shaderDefinitionReloadTestBaselineFailedGeneration =
                    reloadState
                        .latestMaterialDefinitionReloadFailedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestMultiMain);
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestBatchSourcePath,
                    shaderDefinitionReloadTestMultiBatchInvalid);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitMultiDefinitionFailure;
            }
            break;

        case ShaderDefinitionReloadTestPhase::
            WaitMultiDefinitionFailure:
            if (reloadState
                    .latestMaterialDefinitionReloadFailedGeneration >
                shaderDefinitionReloadTestBaselineFailedGeneration)
            {
                const std::string fingerprint =
                    CaptureShaderDefinitionRuntimeFingerprint(
                        validationServices);
                if (fingerprint !=
                    shaderDefinitionReloadTestBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "multi-M_ failed batch changed the live package");
                }
                if (validationServices.CaptureShaderReloadState().pendingMaterialDefinitionSources.count(
                        "runtimeTest/M_shaderReloadTest.json") == 0 ||
                    validationServices.CaptureShaderReloadState().pendingMaterialDefinitionSources.count(
                        "runtimeTest/M_shaderReloadBatchTest.json") == 0)
                {
                    throw std::runtime_error(
                        "multi-M_ failed batch did not retain the complete "
                        "pending source union");
                }
                diagnostics.ReportInfo(
                    "Shader definition multi-M_ rollback preserved "
                    "fingerprint=" + fingerprint +
                    ", pendingUnion=" +
                    std::to_string(
                        validationServices.CaptureShaderReloadState().pendingMaterialDefinitionSources.size()) +
                    ".");
                shaderDefinitionReloadTestBaselineCommittedGeneration =
                    reloadState
                        .latestMaterialDefinitionReloadCommittedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestBatchSourcePath,
                    shaderDefinitionReloadTestMultiBatchValid);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::
                        WaitMultiDefinitionRecovery;
            }
            break;

        case ShaderDefinitionReloadTestPhase::
            WaitMultiDefinitionRecovery:
            if (reloadState
                    .latestMaterialDefinitionReloadCommittedGeneration >
                shaderDefinitionReloadTestBaselineCommittedGeneration)
            {
                ValidateShaderDefinitionMigratedState(
                    FindShaderReloadTestMaterialInstance(),
                    shaderDefinitionReloadTestRetainedTextureIdentity,
                    true,
                    false,
                    true);
                const std::shared_ptr<MaterialInstance> batchInstance =
                    FindShaderReloadBatchTestMaterialInstance();
                if (!batchInstance ||
                    !batchInstance->HasParameter(
                        "u_reloadBatchExtra") ||
                    batchInstance->GetParameter<float>(
                        "u_reloadBatchExtra") != 0.75f)
                {
                    throw std::runtime_error(
                        "multi-M_ recovery did not publish both candidate "
                        "schemas");
                }
                diagnostics.ReportInfo(
                    "Shader definition multi-M_ recovery committed both "
                    "schemas atomically: generation=" +
                    std::to_string(
                        reloadState
                            .latestMaterialDefinitionReloadCommittedGeneration) +
                    ", retainedTexture=" +
                    std::to_string(
                        shaderDefinitionReloadTestRetainedTextureIdentity) +
                    ".");
                shaderDefinitionReloadTestBaselineCommittedGeneration =
                    reloadState
                        .latestMaterialDefinitionReloadCommittedGeneration;
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestSourcePath,
                    shaderDefinitionReloadTestOriginal);
                WriteTextFileAtomically(
                    shaderDefinitionReloadTestBatchSourcePath,
                    shaderDefinitionReloadTestBatchOriginal);
                shaderDefinitionReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderDefinitionWaitTimeout;
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::RestoreOriginal;
            }
            break;

        case ShaderDefinitionReloadTestPhase::RestoreOriginal:
            if (reloadState
                    .latestMaterialDefinitionReloadCommittedGeneration >
                shaderDefinitionReloadTestBaselineCommittedGeneration)
            {
                ValidateShaderDefinitionMigratedState(
                    FindShaderReloadTestMaterialInstance(),
                    shaderDefinitionReloadTestRetainedTextureIdentity,
                    false,
                    false,
                    false);
                const std::shared_ptr<MaterialInstance> batchInstance =
                    FindShaderReloadBatchTestMaterialInstance();
                if (!batchInstance ||
                    batchInstance->HasParameter(
                        "u_reloadBatchExtra"))
                {
                    throw std::runtime_error(
                        "original secondary M_ schema was not restored");
                }
                diagnostics.ReportInfo(
                    "Shader definition reload runtime test passed original "
                    "schema restore while retaining compatible live values.");
                shaderDefinitionReloadTestPhase =
                    ShaderDefinitionReloadTestPhase::WaitRetireDrain;
                shaderDefinitionReloadTestRetireDrainFramesRemaining =
                    RetireDrainFrameBudget;
                shaderDefinitionReloadTestMaxPendingRetiredResources =
                    ResourceRetireQueue::GetInstance()
                        .GetPendingCount();
                return;
            }
            if (std::chrono::steady_clock::now() >=
                shaderDefinitionReloadTestDeadline)
            {
                throw std::runtime_error(
                    "timed out waiting for the original schema restore");
            }
            break;

        case ShaderDefinitionReloadTestPhase::Idle:
        case ShaderDefinitionReloadTestPhase::WaitWorldLoad:
        case ShaderDefinitionReloadTestPhase::WaitRetireDrain:
            break;
        }

        if (shaderDefinitionReloadTestPhase !=
                ShaderDefinitionReloadTestPhase::WaitRetireDrain &&
            shaderDefinitionReloadTestPhase !=
                ShaderDefinitionReloadTestPhase::Idle &&
            shaderDefinitionReloadTestPhase !=
                ShaderDefinitionReloadTestPhase::WaitWorldLoad)
        {
            if (std::chrono::steady_clock::now() >=
                shaderDefinitionReloadTestDeadline)
            {
                throw std::runtime_error(
                    "timed out waiting for the current definition reload "
                    "matrix phase");
            }
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderDefinitionReloadTest(
            std::string(
                "Shader definition reload runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

void RuntimeTestHooks::FailShaderUiReloadTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderUiReloadTestVertexPath.empty() &&
        !shaderUiReloadTestVertexOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderUiReloadTestVertexPath,
                shaderUiReloadTestVertexOriginal);
        }
        catch (...)
        {
        }
    }
    if (!shaderUiReloadTestFragmentPath.empty() &&
        !shaderUiReloadTestFragmentOriginal.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderUiReloadTestFragmentPath,
                shaderUiReloadTestFragmentOriginal);
        }
        catch (...)
        {
        }
    }
    shaderUiReloadTestActive = false;
    waitingForShaderUiReloadTestWorld = false;
    shaderUiReloadTestPhaseEntryPending = false;
    shaderUiReloadTestPhase =
        ShaderUiReloadTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupGeneratedRuntimeFixture(
        shaderUiReloadTestFixtureDirectory,
        diagnostics);
    shaderUiReloadTestFixtureDirectory.clear();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::UpdateShaderUiReloadTest(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderUiReloadTestActive)
    {
        return;
    }

    if (shaderUiReloadTestPhase ==
        ShaderUiReloadTestPhase::WaitRetireDrain)
    {
        ResourceRetireQueue& retireQueue =
            ResourceRetireQueue::GetInstance();
        const size_t pending = retireQueue.GetPendingCount();
        shaderUiReloadTestMaxPendingRetiredResources =
            std::max(
                shaderUiReloadTestMaxPendingRetiredResources,
                pending);
        if (pending == 0)
        {
            if (shaderUiReloadTestMaxPendingRetiredResources == 0)
            {
                FailShaderUiReloadTest(
                    "Shader UI overlay reload runtime test did not observe "
                    "any epoch-retired UI pipeline resources.",
                    diagnostics);
                return;
            }
            shaderUiReloadTestActive = false;
            shaderUiReloadTestPhase =
                ShaderUiReloadTestPhase::Idle;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            CleanupGeneratedRuntimeFixture(
                shaderUiReloadTestFixtureDirectory,
                diagnostics);
            shaderUiReloadTestFixtureDirectory.clear();
            diagnostics.ReportInfo(
                "Shader UI overlay reload runtime test completed: "
                "ABI-compatible pair swap, ABI rejection, restore, and "
                "epoch retirement passed.");
            return;
        }

        --shaderUiReloadTestRetireDrainFramesRemaining;
        if (shaderUiReloadTestRetireDrainFramesRemaining <= 0)
        {
            FailShaderUiReloadTest(
                "Shader UI overlay reload runtime test timed out waiting "
                "for retired resources to drain. pending=" +
                    std::to_string(pending),
                diagnostics);
        }
        return;
    }

    try
    {
        if (shaderUiReloadTestPhaseEntryPending)
        {
            shaderUiReloadTestPhaseEntryPending = false;
            WriteTextFileAtomically(
                shaderUiReloadTestFragmentPath,
                shaderUiReloadTestFragmentCompatible);
            shaderUiReloadTestBaselineLatestGeneration =
                validationServices.CaptureShaderReloadState().latestSubmittedAutoReloadGeneration;
            shaderUiReloadTestDeadline =
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
        const bool fragmentMatchesCurrent =
            UiArtifactFragmentMatchesCurrentSource(validationServices);

        switch (shaderUiReloadTestPhase)
        {
        case ShaderUiReloadTestPhase::WaitCompatibleCommit:
            if (workerSettled &&
                reloadState.latestSubmittedAutoReloadGeneration >
                    shaderUiReloadTestBaselineLatestGeneration)
            {
                if (!fragmentMatchesCurrent)
                {
                    throw std::runtime_error(
                        "compatible UI edit did not commit the candidate "
                        "pipeline pair");
                }
                diagnostics.ReportInfo(
                    "Shader UI overlay reload runtime test passed the "
                    "ABI-compatible pipeline pair swap.");
                WriteTextFileAtomically(
                    shaderUiReloadTestFragmentPath,
                    shaderUiReloadTestFragmentAbiIncompatible);
                shaderUiReloadTestBaselineLatestGeneration =
                    reloadState.latestSubmittedAutoReloadGeneration;
                shaderUiReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderUiReloadTestPhase =
                    ShaderUiReloadTestPhase::WaitAbiRejection;
            }
            break;

        case ShaderUiReloadTestPhase::WaitAbiRejection:
            if (workerSettled &&
                reloadState.latestSubmittedAutoReloadGeneration >
                    shaderUiReloadTestBaselineLatestGeneration)
            {
                if (fragmentMatchesCurrent)
                {
                    throw std::runtime_error(
                        "ABI-incompatible UI edit replaced the live "
                        "pipeline pair");
                }
                diagnostics.ReportInfo(
                    "Shader UI overlay reload runtime test passed ABI "
                    "rejection with old pipeline retention.");
                WriteTextFileAtomically(
                    shaderUiReloadTestFragmentPath,
                    shaderUiReloadTestFragmentOriginal);
                shaderUiReloadTestBaselineLatestGeneration =
                    reloadState.latestSubmittedAutoReloadGeneration;
                shaderUiReloadTestDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                shaderUiReloadTestPhase =
                    ShaderUiReloadTestPhase::RestoreOriginal;
            }
            break;

        case ShaderUiReloadTestPhase::RestoreOriginal:
            if (workerSettled &&
                reloadState.latestSubmittedAutoReloadGeneration >
                    shaderUiReloadTestBaselineLatestGeneration)
            {
                if (!fragmentMatchesCurrent)
                {
                    throw std::runtime_error(
                        "UI original restore did not commit the candidate "
                        "pipeline pair");
                }
                diagnostics.ReportInfo(
                    "Shader UI overlay reload runtime test passed original "
                    "source restore.");
                shaderUiReloadTestPhase =
                    ShaderUiReloadTestPhase::WaitRetireDrain;
                shaderUiReloadTestRetireDrainFramesRemaining =
                    RetireDrainFrameBudget;
                shaderUiReloadTestMaxPendingRetiredResources =
                    ResourceRetireQueue::GetInstance()
                        .GetPendingCount();
                return;
            }
            break;

        case ShaderUiReloadTestPhase::Idle:
        case ShaderUiReloadTestPhase::WaitWorldLoad:
        case ShaderUiReloadTestPhase::WaitRetireDrain:
            break;
        }

        if (std::chrono::steady_clock::now() >=
            shaderUiReloadTestDeadline)
        {
            throw std::runtime_error(
                "timed out waiting for the UI overlay reload pipeline to "
                "settle in phase " +
                std::to_string(
                    static_cast<int>(shaderUiReloadTestPhase)));
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderUiReloadTest(
            std::string(
                "Shader UI overlay reload runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

void RuntimeTestHooks::FailShaderShutdownInflightTest(
    RuntimeValidationServices& validationServices,
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderShutdownInflightSourcePath.empty() &&
        !shaderShutdownInflightOriginalSource.empty())
    {
        try
        {
            WriteTextFileAtomically(
                shaderShutdownInflightSourcePath,
                shaderShutdownInflightOriginalSource);
        }
        catch (...)
        {
        }
    }
    validationServices.SetShaderMonitorScanSuspended(true);
    shaderShutdownInflightTestActive = false;
    shaderShutdownInflightTestPhase =
        ShaderShutdownInflightTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    validationServices.MarkClosingTestFailure();
    diagnostics.ReportError(message);
}

void RuntimeTestHooks::UpdateShaderShutdownInflightTest(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderShutdownInflightTestActive)
    {
        return;
    }

    try
    {
        const ShaderReloadRuntimeStateSnapshot initialState =
            validationServices.CaptureShaderReloadState();
        if (!initialState.workerRunning)
        {
            throw std::runtime_error(
                "shutdown-in-flight test lost its monitor, worker, or compiler");
        }

        switch (shaderShutdownInflightTestPhase)
        {
        case ShaderShutdownInflightTestPhase::WaitMonitorBaseline:
            if (initialState.monitorScanCount > 0 &&
                !initialState.monitorHasUnstableSourceChanges &&
                initialState.workerIdle &&
                initialState.pendingAutoReloadSources.empty() &&
                initialState.inFlightAutoReloadGeneration == 0)
            {
                shaderShutdownInflightBaselineFingerprint =
                    CaptureShaderShutdownInflightFingerprint(
                        validationServices);
                shaderShutdownInflightBaselineCommittedGeneration =
                    initialState.latestAutoReloadCommittedGeneration;
                validationServices.ArmShaderPostCompileGate();
                validationServices.SetShaderMonitorPollInterval(
                        std::chrono::milliseconds(0));
                WriteTextFileAtomically(
                    shaderShutdownInflightSourcePath,
                    shaderShutdownInflightCandidateSource);
                shaderShutdownInflightTestPhase =
                    ShaderShutdownInflightTestPhase::WaitPostCompileGate;
                shaderShutdownInflightDeadline =
                    std::chrono::steady_clock::now() +
                    ShaderAsyncWaitTimeout;
                diagnostics.ReportInfo(
                    "Shader shutdown-in-flight test armed the post-compile gate and changed uiOverlay.frag.");
            }
            break;

        case ShaderShutdownInflightTestPhase::WaitPostCompileGate:
            if (validationServices.IsShaderPostCompileGateWaiting())
            {
                const ShaderReloadRuntimeStateSnapshot gateState =
                    validationServices.CaptureShaderReloadState();
                shaderShutdownInflightCandidateGeneration =
                    gateState.workerInFlightGeneration;
                if (shaderShutdownInflightCandidateGeneration == 0 ||
                    gateState.latestSubmittedAutoReloadGeneration !=
                        shaderShutdownInflightCandidateGeneration)
                {
                    throw std::runtime_error(
                        "post-compile gate did not retain the submitted generation");
                }
                if (gateState.workerHasCompletedResult ||
                    gateState.latestAutoReloadCommittedGeneration !=
                        shaderShutdownInflightBaselineCommittedGeneration)
                {
                    throw std::runtime_error(
                        "complete shutdown candidate was published or committed before shutdown");
                }
                const std::string currentFingerprint =
                    CaptureShaderShutdownInflightFingerprint(
                        validationServices);
                if (currentFingerprint !=
                    shaderShutdownInflightBaselineFingerprint)
                {
                    throw std::runtime_error(
                        "live or formal artifact fingerprint changed before shutdown");
                }

                validationServices.SetShaderMonitorScanSuspended(true);
                WriteTextFileAtomically(
                    shaderShutdownInflightSourcePath,
                    shaderShutdownInflightOriginalSource);
                shaderShutdownInflightTestPhase =
                    ShaderShutdownInflightTestPhase::AwaitShutdown;
                validationServices.MarkClosingTestFailure();
                diagnostics.ReportInfo(
                    "Shader candidate ready at post-compile gate: generation=" +
                    std::to_string(
                        shaderShutdownInflightCandidateGeneration) +
                    "; live/formal fingerprint unchanged; shutdown requested.");
                return;
            }
            break;

        case ShaderShutdownInflightTestPhase::AwaitShutdown:
        case ShaderShutdownInflightTestPhase::Idle:
            break;
        }

        if (std::chrono::steady_clock::now() >=
            shaderShutdownInflightDeadline)
        {
            throw std::runtime_error(
                "shutdown-in-flight test timed out in phase " +
                std::to_string(
                    static_cast<int>(
                        shaderShutdownInflightTestPhase)));
        }
    }
    catch (const std::exception& exception)
    {
        FailShaderShutdownInflightTest(
            validationServices,
            std::string(
                "Shader shutdown-in-flight runtime test failed: ") +
                exception.what(),
            diagnostics);
    }
}

bool RuntimeTestHooks::
FinalizeShaderShutdownInflightTestAfterWorkerShutdown(
    RuntimeValidationServices& validationServices,
    const ShaderCompileWorkerShutdownDiagnostics& workerDiagnostics,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!shaderShutdownInflightTestActive)
    {
        return runtimeTestStatus != RuntimeTestStatus::Failed;
    }

    try
    {
        if (shaderShutdownInflightTestPhase !=
            ShaderShutdownInflightTestPhase::AwaitShutdown)
        {
            throw std::runtime_error(
                "shutdown began before the complete candidate reached its gate");
        }
        if (!workerDiagnostics.joined ||
            !workerDiagnostics.discardedCompletedCandidate ||
            workerDiagnostics.postCompileGateGeneration !=
                shaderShutdownInflightCandidateGeneration ||
            workerDiagnostics.discardedGeneration !=
                shaderShutdownInflightCandidateGeneration ||
            workerDiagnostics.completedResultPublishedGeneration != 0)
        {
            throw std::runtime_error(
                "worker shutdown diagnostics did not prove complete-candidate discard");
        }
        if (validationServices.CaptureShaderReloadState().latestAutoReloadCommittedGeneration !=
            shaderShutdownInflightBaselineCommittedGeneration)
        {
            throw std::runtime_error(
                "shutdown candidate advanced the committed reload generation");
        }
        const std::string currentFingerprint =
            CaptureShaderShutdownInflightFingerprint(
                validationServices);
        if (currentFingerprint !=
            shaderShutdownInflightBaselineFingerprint)
        {
            throw std::runtime_error(
                "pre-teardown live/formal artifact fingerprint changed");
        }
        if (ReadTextFileBytes(
                shaderShutdownInflightSourcePath) !=
            shaderShutdownInflightOriginalSource)
        {
            throw std::runtime_error(
                "shutdown-in-flight source fixture was not restored");
        }

        shaderShutdownInflightTestActive = false;
        shaderShutdownInflightTestPhase =
            ShaderShutdownInflightTestPhase::Idle;
        runtimeTestStatus = RuntimeTestStatus::Succeeded;
        validationServices.MarkShutdownTestSucceeded();
        diagnostics.ReportInfo(
            "Shader compile worker joined: generation=" +
            std::to_string(
                shaderShutdownInflightCandidateGeneration) +
            ", discardedCompletedCandidate=true, completedResultPublished=false.");
        diagnostics.ReportInfo(
            "Shader shutdown-in-flight pre-teardown fingerprint unchanged; runtime test passed.");
        return true;
    }
    catch (const std::exception& exception)
    {
        shaderShutdownInflightTestActive = false;
        shaderShutdownInflightTestPhase =
            ShaderShutdownInflightTestPhase::Idle;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        validationServices.MarkClosingTestFailure();
        diagnostics.ReportError(
            std::string(
                "Shader shutdown-in-flight finalization failed: ") +
            exception.what());
        return false;
    }
}

} // namespace VL

