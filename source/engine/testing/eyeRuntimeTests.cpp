#include "engine/runtimeTestHooks.h"

#include <array>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/diagnosticsSubsystem.h"
#include "engine/testing/runtimeValidationServices.h"
#include "render/eye/eyeAssets.h"
#include "shader/build/atomicFile.h"

namespace VL
{
namespace
{

constexpr int FirstEyeDebugView = 42;
constexpr int LastEyeDebugView = 63;
void RequireEyeCondition(bool condition, const std::string& message);


constexpr int EyePerformanceSampleFrameCount = 3;

bool IsEyeLutReadbackEnabled() noexcept
{
    const char* value = std::getenv("VULKANLEARN_EYE_LUT_READBACK");
    return value != nullptr && std::string(value) == "1";
}

size_t ExpectedEyeLutMemoryBytes()
{
    return static_cast<size_t>(EyeCausticLutWidth) *
        EyeCausticLutHeight * EyeCausticLutLayerCount *
        EyeCausticLutChannelCount * 2;
}

void ValidateEyePerformanceSnapshot(
    const RuntimeEyeValidationSnapshot& snapshot,
    std::string_view pathName)
{
    RequireEyeCondition(
        snapshot.captured && snapshot.hasEyeResources,
        std::string(pathName) + " performance snapshot has no Eye resources");
    RequireEyeCondition(
        snapshot.eyeLutMemoryBytes == ExpectedEyeLutMemoryBytes(),
        std::string(pathName) + " Eye LUT memory differs from the RGBA16F budget");
    RequireEyeCondition(
        snapshot.performanceWithinBudget,
        std::string(pathName) + " Eye frame exceeded the configured budget");
    RequireEyeCondition(
        snapshot.performanceStats.eyeDrawCount > 0,
        std::string(pathName) + " produced no Eye draw in the sampled frame");
    RequireEyeCondition(
        snapshot.performanceStats.eyeDescriptorBindCount > 0,
        std::string(pathName) +
            " produced no Eye descriptor bind in the sampled frame");
    RequireEyeCondition(
        snapshot.performanceStats.eyeLutSampleCount > 0,
        std::string(pathName) + " produced no Eye LUT sample in the sampled frame");
}

void RequireEyeCondition(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

const RuntimeEyeMaterialSnapshot* FindEyeMaterial(
    const RuntimeEyeValidationSnapshot& snapshot)
{
    for (const RuntimeEyeMaterialSnapshot& material : snapshot.materials)
    {
        if (material.shadingModelMacro == "SHADING_MODEL_EYE")
        {
            return &material;
        }
    }
    return nullptr;
}

const RuntimeEyeMaterialSnapshot* FindEyeMaterialByMode(
    const RuntimeEyeValidationSnapshot& snapshot,
    std::string_view renderMode)
{
    for (const RuntimeEyeMaterialSnapshot& material : snapshot.materials)
    {
        if (material.shadingModelMacro == "SHADING_MODEL_EYE" &&
            material.renderMode == renderMode)
        {
            return &material;
        }
    }
    return nullptr;
}

void ValidateEyeSnapshot(const RuntimeEyeValidationSnapshot& snapshot)
{
    RequireEyeCondition(
        snapshot.captured,
        "Eye validation snapshot was not captured");
    RequireEyeCondition(
        snapshot.hasEyeResources,
        "Eye validation World-local resource set is missing");
    RequireEyeCondition(
        !snapshot.sourceDigest.empty() &&
            !snapshot.artifactGenerationKey.empty(),
        "Eye resource source/artifact identity is empty");
    RequireEyeCondition(
        snapshot.lutWidth == EyeCausticLutWidth &&
            snapshot.lutHeight == EyeCausticLutHeight &&
            snapshot.lutLayers == EyeCausticLutLayerCount,
        "Eye LUT dimensions do not match the frozen contract");
    RequireEyeCondition(
        snapshot.eyeLutTextureIdentity != 0 &&
            snapshot.eyeLutTextureIdentity ==
                snapshot.boundEyeWorldTextureIdentity,
        "Eye LUT texture is not consistently bound as a World-local texture");
    RequireEyeCondition(
        snapshot.forwardEyeLutBinding,
        "forwardOpaque must expose eyeCausticLut at binding 2");
    RequireEyeCondition(
        snapshot.forwardEyeInnerPassPresent &&
            snapshot.forwardEyeCorneaPassPresent,
        "dual-shell Eye passes must expose the shared LUT binding");
    RequireEyeCondition(
        snapshot.deferredEyeLutBinding &&
            snapshot.deferredGBufferContract &&
            snapshot.sssSourcePresent,
        "Deferred Eye fallback/GBuffer/SSS source contract is incomplete");
    RequireEyeCondition(
        snapshot.lodContractValid,
        "Eye profile LOD/version contract is invalid at runtime");
    RequireEyeCondition(
        snapshot.eyeLutMemoryBytes ==
            static_cast<size_t>(EyeCausticLutWidth) *
                EyeCausticLutHeight * EyeCausticLutLayerCount *
                EyeCausticLutChannelCount * 2,
        "Eye LUT memory budget does not match RGBA16F atlas dimensions");
    RequireEyeCondition(
        snapshot.performanceWithinBudget,
        "Eye frame exceeded the configured performance budget");

    const RuntimeEyeMaterialSnapshot* material = FindEyeMaterial(snapshot);
    RequireEyeCondition(
        material != nullptr,
        "Eye validation scene did not load an Eye material instance");
    RequireEyeCondition(
        material->renderMode == "ForwardOpaque" &&
            material->hasRenderPipeline &&
            material->hasShadowRoute,
        "Eye material pipeline or Shadow pipeline contract is incomplete");
    RequireEyeCondition(
        material->hasEyeParameters,
        "Eye material parameter snapshot is incomplete");
    RequireEyeCondition(
        material->forwardDescriptorLayoutCompatible,
        "Eye material descriptor layout does not match forwardOpaque");
}

void ValidateEyeDeferredSnapshot(
    const RuntimeEyeValidationSnapshot& snapshot)
{
    RequireEyeCondition(
        snapshot.captured && snapshot.deferredGBufferContract &&
            snapshot.deferredEyeLutBinding,
        "Deferred Eye scene did not expose the GBuffer/LUT contract");
    const RuntimeEyeMaterialSnapshot* material =
        FindEyeMaterialByMode(snapshot, "Opaque");
    RequireEyeCondition(
        material != nullptr && material->hasRenderPipeline &&
            material->deferredDescriptorLayoutCompatible &&
            material->hasEyeParameters,
        "M_eyeDeferred did not bind the geometry/deferred material contract");
}

void ValidateEyeDualShellSnapshot(
    const RuntimeEyeValidationSnapshot& snapshot)
{
    const RuntimeEyeMaterialSnapshot* inner =
        FindEyeMaterialByMode(snapshot, "ForwardEyeInner");
    const RuntimeEyeMaterialSnapshot* cornea =
        FindEyeMaterialByMode(snapshot, "ForwardEyeCornea");
    RequireEyeCondition(
        inner != nullptr && cornea != nullptr,
        "Dual-shell scene did not load both Eye shell materials");
    RequireEyeCondition(
        inner->hasEyeParameters && cornea->hasEyeParameters &&
            inner->dualShellLayerContract &&
            cornea->dualShellLayerContract,
        "Dual-shell layer/contact/cilia authoring contract is incomplete");
    RequireEyeCondition(
        inner->forwardDescriptorLayoutCompatible &&
            cornea->forwardDescriptorLayoutCompatible,
        "Dual-shell pass pipeline descriptor contracts are incompatible");
}

void WriteEyeValidationFailureFixture(
    const std::filesystem::path& directory,
    std::filesystem::path& scenePath)
{
    std::filesystem::create_directories(directory);
    const std::filesystem::path materialPath = directory / "MI_eye_invalid.json";
    const std::filesystem::path meshPath = directory / "SM_eye_invalid.json";
    scenePath = directory / "SC_eye_invalid.json";

    const nlohmann::json material = {
        {"name", "Eye Validation Invalid Material"},
        {"type", "materialInstance"},
        {"material", "shader/glsl/M_eye.json"},
        // 故意引用当前 World 不存在的 profile；candidate 失败时 active World
        // 必须保持原来的 Eye resource/package 和 descriptor identity。
        {"eyeProfile", "eyeProfiles/EP_missing_for_validation.json"}};
    WriteTextFileAtomically(materialPath, material.dump(2) + "\n");

    const nlohmann::json mesh = {
        {"name", "Eye Validation Invalid Mesh"},
        {"type", "mesh"},
        {"modelDataPath", "models/datas/sphere.obj"},
        {"materialSlots", nlohmann::json::array({
            {
                {"name", "Default"},
                {"materialInstancePath", materialPath.generic_string()}
            }})}};
    WriteTextFileAtomically(meshPath, mesh.dump(2) + "\n");

    const nlohmann::json scene = {
        {"name", "Eye Validation Invalid Scene"},
        {"type", "scene"},
        {"objects", nlohmann::json::array({
            {
                {"name", "EyeValidationInvalid"},
                {"type", "mesh"},
                {"modelPath", meshPath.generic_string()},
                {"position", {0.0, 0.0, 0.0}},
                {"scale", {0.012, 0.012, 0.012}},
                {"rotation", {0.0, 0.0, 0.0}}
            },
            {
                {"name", "EyeValidationLight"},
                {"type", "directionalLight"},
                {"position", {2.0, 4.0, 4.0}},
                {"rotation", {-25.0, -35.0, 0.0}},
                {"color", {1.0, 0.95, 0.9}},
                {"intensity", 3.0}
            },
            {
                {"name", "EyeValidationCamera"},
                {"type", "camera"},
                {"fov", 45.0},
                {"near_clip", 0.01},
                {"far_clip", 100.0},
                {"position", {0.0, 0.0, 0.08}},
                {"rotation", {0.0, 0.0, 0.0}},
                {"scale", {1.0, 1.0, 1.0}}
            }
        })}};
    WriteTextFileAtomically(scenePath, scene.dump(2) + "\n");
}

bool SameWorldHandle(const WorldHandle& first, const WorldHandle& second)
{
    return first.generation == second.generation &&
        first.scenePath == second.scenePath;
}

} // namespace

bool RuntimeTestHooks::BeginEyeValidationTest(
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
        const std::filesystem::path deferredScenePath =
            resourceRoot / "scenes" / "SC_eye_deferred_probe.json";
        const std::filesystem::path dualShellScenePath =
            resourceRoot / "scenes" / "SC_eye_dual_shell_probe.json";
        const std::filesystem::path profileDirectory =
            resourceRoot / "eyeProfiles";
        RequireEyeCondition(
            std::filesystem::is_regular_file(scenePath) &&
                std::filesystem::is_regular_file(deferredScenePath) &&
                std::filesystem::is_regular_file(dualShellScenePath),
            "Eye runtime validation requires Forward, Deferred and Dual-shell probe scenes");
        RequireEyeCondition(
            std::filesystem::is_directory(profileDirectory),
            "Eye runtime validation requires the eyeProfiles directory");
        const std::vector<EyeProfileAsset> profiles =
            LoadEyeProfileAssets(resourceRoot);
        RequireEyeCondition(
            !profiles.empty(),
            "Eye runtime validation found no Eye profiles");

        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto suffix =
            std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        const std::filesystem::path fixtureDirectory =
            resourceRoot / "generated" /
            ("eye-validation-" + std::to_string(suffix));
        std::filesystem::path invalidScenePath;
        WriteEyeValidationFailureFixture(
            fixtureDirectory,
            invalidScenePath);

        eyeValidationFixtureDirectory = fixtureDirectory.string();
        eyeValidationScenePath = scenePath.string();
        eyeValidationDeferredScenePath = deferredScenePath.string();
        eyeValidationDualShellScenePath = dualShellScenePath.string();
        eyeValidationFailureScenePath = invalidScenePath.string();
        eyeValidationSourceDigest.clear();
        eyeValidationArtifactGenerationKey.clear();
        eyeValidationLutTextureIdentity = 0;
        eyeValidationBaselineWorld = {};
        eyeValidationNextDebugView = FirstEyeDebugView;
        waitingForEyeValidationWorld = false;
        eyeValidationTestPhase = EyeValidationTestPhase::WaitWorldLoad;
        eyeValidationTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Eye runtime validation started: LUT/resource contract, same-digest reuse, rollback and debug views 42-63.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create Eye runtime validation fixture: ") +
            exception.what());
        CleanupEyeValidationTestFixture();
        return false;
    }
}

bool RuntimeTestHooks::BeginEyePerformanceTest(
    const std::string& resourcePath,
    const DiagnosticsSubsystem& diagnostics)
{
    if (runtimeTestStatus == RuntimeTestStatus::Running)
    {
        diagnostics.ReportWarning(
            "A runtime validation test is already running.");
        return false;
    }
    if (IsEyeLutReadbackEnabled())
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            "Eye performance test refuses VULKANLEARN_EYE_LUT_READBACK=1; "
            "LUT readback is not a steady-state performance baseline.");
        return false;
    }

    try
    {
        const std::filesystem::path resourceRoot(resourcePath);
        const std::filesystem::path scenePath =
            resourceRoot / "scenes" / "SC_eye_probe.json";
        const std::filesystem::path deferredScenePath =
            resourceRoot / "scenes" / "SC_eye_deferred_probe.json";
        const std::filesystem::path dualShellScenePath =
            resourceRoot / "scenes" / "SC_eye_dual_shell_probe.json";
        RequireEyeCondition(
            std::filesystem::is_regular_file(scenePath) &&
                std::filesystem::is_regular_file(deferredScenePath) &&
                std::filesystem::is_regular_file(dualShellScenePath),
            "Eye performance test requires Forward, Deferred and Dual-shell probe scenes");
        RequireEyeCondition(
            std::filesystem::is_directory(resourceRoot / "eyeProfiles"),
            "Eye performance test requires the eyeProfiles directory");
        RequireEyeCondition(
            !LoadEyeProfileAssets(resourceRoot).empty(),
            "Eye performance test found no Eye profiles");

        eyePerformanceScenePath = scenePath.string();
        eyePerformanceDeferredScenePath = deferredScenePath.string();
        eyePerformanceDualShellScenePath = dualShellScenePath.string();
        waitingForEyePerformanceWorld = false;
        eyePerformanceFramesRemaining = EyePerformanceSampleFrameCount;
        eyePerformanceWarmupFramesRemaining = 2;
        eyePerformanceTestPhase = EyePerformanceTestPhase::WaitWorldLoad;
        eyePerformanceTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Eye performance test started: three fixed frames per Forward, "
            "Deferred and Dual-shell probe; LUT readback/debug views excluded.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create Eye performance test: ") +
            exception.what());
        CleanupEyePerformanceTestFixture();
        return false;
    }
}

void RuntimeTestHooks::UpdateEyeValidationTest(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!eyeValidationTestActive)
    {
        return;
    }

    try
    {
        if (eyeValidationTestPhase ==
            EyeValidationTestPhase::ValidateWorld)
        {
            const RuntimeEyeValidationSnapshot snapshot =
                validationServices.CaptureEyeValidationSnapshot();
            ValidateEyeSnapshot(snapshot);
            eyeValidationBaselineWorld =
                validationServices.GetActiveWorldHandle();
            eyeValidationSourceDigest = snapshot.sourceDigest;
            eyeValidationArtifactGenerationKey =
                snapshot.artifactGenerationKey;
            eyeValidationLutTextureIdentity =
                snapshot.eyeLutTextureIdentity;
            eyeValidationTestPhase =
                EyeValidationTestPhase::QueueDeferred;
            diagnostics.ReportInfo(
                "Eye Forward contract passed; checking Deferred GBuffer fallback.");
            return;
        }

        if (eyeValidationTestPhase ==
            EyeValidationTestPhase::ValidateDeferred)
        {
            const RuntimeEyeValidationSnapshot snapshot =
                validationServices.CaptureEyeValidationSnapshot();
            RequireEyeCondition(
                snapshot.captured && snapshot.hasEyeResources &&
                    snapshot.eyeLutTextureIdentity ==
                        snapshot.boundEyeWorldTextureIdentity,
                "Deferred Eye snapshot lost the active resource package");
            ValidateEyeDeferredSnapshot(snapshot);
            eyeValidationTestPhase = EyeValidationTestPhase::QueueDualShell;
            diagnostics.ReportInfo(
                "Eye Deferred contract passed; checking dual-shell inner/cornea routing.");
            return;
        }

        if (eyeValidationTestPhase ==
            EyeValidationTestPhase::ValidateDualShell)
        {
            const RuntimeEyeValidationSnapshot snapshot =
                validationServices.CaptureEyeValidationSnapshot();
            RequireEyeCondition(
                snapshot.captured && snapshot.hasEyeResources &&
                    snapshot.forwardEyeInnerPassPresent &&
                    snapshot.forwardEyeCorneaPassPresent,
                "Dual-shell Eye snapshot lost shared pass resources");
            ValidateEyeDualShellSnapshot(snapshot);
            eyeValidationTestPhase = EyeValidationTestPhase::QueueSameWorldReload;
            diagnostics.ReportInfo(
                "Eye dual-shell contract passed; checking same-digest World reload reuse.");
            return;
        }

        if (eyeValidationTestPhase ==
            EyeValidationTestPhase::ValidateReload)
        {
            const RuntimeEyeValidationSnapshot snapshot =
                validationServices.CaptureEyeValidationSnapshot();
            ValidateEyeSnapshot(snapshot);
            RequireEyeCondition(
                snapshot.sourceDigest == eyeValidationSourceDigest &&
                    snapshot.artifactGenerationKey ==
                        eyeValidationArtifactGenerationKey,
                "same-digest Eye reload changed resource identity metadata");
            RequireEyeCondition(
                snapshot.eyeLutTextureIdentity ==
                    eyeValidationLutTextureIdentity,
                "same-digest Eye reload did not reuse LUT texture identity");
            // 失败 candidate 必须与最新一次成功提交后的 active World 比较；
            // 同场景 reload 会合法推进 generation，不能继续使用第一次 probe 基线。
            eyeValidationBaselineWorld =
                validationServices.GetActiveWorldHandle();
            eyeValidationTestPhase = EyeValidationTestPhase::QueueFailure;
            diagnostics.ReportInfo(
                "Eye same-digest reload reused the LUT; checking failed candidate rollback.");
            return;
        }

        if (eyeValidationTestPhase == EyeValidationTestPhase::QueueDebugView)
        {
            if (eyeValidationNextDebugView > LastEyeDebugView)
            {
                eyeValidationTestActive = false;
                eyeValidationTestPhase = EyeValidationTestPhase::Idle;
                runtimeTestStatus = RuntimeTestStatus::Succeeded;
                CleanupEyeValidationTestFixture();
                diagnostics.ReportInfo(
                    "Eye runtime validation succeeded: resource transaction, rollback and debug views 42-63 passed.");
                return;
            }

            RuntimeCommand command;
            command.type = RuntimeCommandType::SetDebugViewMode;
            command.intValue = eyeValidationNextDebugView;
            command.sourceText = "runtime-test: eye-validation-debug-view";
            validationServices.QueueRuntimeCommand(std::move(command));
            eyeValidationTestPhase = EyeValidationTestPhase::WaitDebugView;
            return;
        }

        if (eyeValidationTestPhase == EyeValidationTestPhase::WaitDebugView)
        {
            RequireEyeCondition(
                validationServices.GetDebugViewMode() ==
                    eyeValidationNextDebugView,
                "Eye debug view command did not reach the renderer: mode=" +
                    std::to_string(eyeValidationNextDebugView));
            ++eyeValidationNextDebugView;
            eyeValidationTestPhase = EyeValidationTestPhase::QueueDebugView;
        }
    }
    catch (const std::exception& exception)
    {
        FailEyeValidationTest(exception.what(), diagnostics);
    }
}

void RuntimeTestHooks::UpdateEyePerformanceTest(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!eyePerformanceTestActive)
    {
        return;
    }

    try
    {
        if (eyePerformanceTestPhase == EyePerformanceTestPhase::ValidateForward)
        {
            if (eyePerformanceWarmupFramesRemaining > 0)
            {
                --eyePerformanceWarmupFramesRemaining;
                return;
            }

            const RuntimeEyeValidationSnapshot snapshot =
                validationServices.CaptureEyeValidationSnapshot();
            ValidateEyePerformanceSnapshot(snapshot, "ForwardOpaque");
            RequireEyeCondition(
                snapshot.forwardEyeLutBinding,
                "ForwardOpaque performance sample lost the Eye LUT binding");
            if (--eyePerformanceFramesRemaining <= 0)
            {
                eyePerformanceFramesRemaining = EyePerformanceSampleFrameCount;
                eyePerformanceTestPhase = EyePerformanceTestPhase::QueueDeferred;
                eyePerformanceWarmupFramesRemaining = 2;
                diagnostics.ReportInfo(
                    "Eye ForwardOpaque performance budget passed; sampling Deferred.");
            }
            return;
        }

        if (eyePerformanceTestPhase == EyePerformanceTestPhase::ValidateDeferred)
        {
            if (eyePerformanceWarmupFramesRemaining > 0)
            {
                --eyePerformanceWarmupFramesRemaining;
                return;
            }

            const RuntimeEyeValidationSnapshot snapshot =
                validationServices.CaptureEyeValidationSnapshot();
            ValidateEyePerformanceSnapshot(snapshot, "Deferred");
            RequireEyeCondition(
                snapshot.deferredEyeLutBinding &&
                    snapshot.deferredGBufferContract,
                "Deferred performance sample lost the Eye GBuffer/LUT contract");
            if (--eyePerformanceFramesRemaining <= 0)
            {
                eyePerformanceFramesRemaining = EyePerformanceSampleFrameCount;
                eyePerformanceTestPhase = EyePerformanceTestPhase::QueueDualShell;
                eyePerformanceWarmupFramesRemaining = 2;
                diagnostics.ReportInfo(
                    "Eye Deferred performance budget passed; sampling Dual-shell.");
            }
            return;
        }

        if (eyePerformanceTestPhase ==
            EyePerformanceTestPhase::ValidateDualShell)
        {
            const RuntimeEyeValidationSnapshot snapshot =
                validationServices.CaptureEyeValidationSnapshot();
            if (eyePerformanceWarmupFramesRemaining > 0)
            {
                --eyePerformanceWarmupFramesRemaining;
                return;
            }

            ValidateEyePerformanceSnapshot(snapshot, "Dual-shell");
            RequireEyeCondition(
                snapshot.forwardEyeInnerPassPresent &&
                    snapshot.forwardEyeCorneaPassPresent,
                "Dual-shell performance sample lost the inner/cornea pass contract");
            if (--eyePerformanceFramesRemaining <= 0)
            {
                eyePerformanceTestActive = false;
                eyePerformanceTestPhase = EyePerformanceTestPhase::Idle;
                runtimeTestStatus = RuntimeTestStatus::Succeeded;
                CleanupEyePerformanceTestFixture();
                diagnostics.ReportInfo(
                    "Eye performance test succeeded: fixed-frame Forward, "
                    "Deferred and Dual-shell budgets stayed within limits.");
            }
        }
    }
    catch (const std::exception& exception)
    {
        FailEyePerformanceTest(exception.what(), diagnostics);
    }
}

void RuntimeTestHooks::FailEyePerformanceTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    eyePerformanceTestActive = false;
    waitingForEyePerformanceWorld = false;
    eyePerformanceTestPhase = EyePerformanceTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupEyePerformanceTestFixture();
    diagnostics.ReportError("Eye performance test failed: " + message);
}

void RuntimeTestHooks::CleanupEyePerformanceTestFixture() noexcept
{
    eyePerformanceScenePath.clear();
    eyePerformanceDeferredScenePath.clear();
    eyePerformanceDualShellScenePath.clear();
    waitingForEyePerformanceWorld = false;
    eyePerformanceFramesRemaining = 0;
    eyePerformanceWarmupFramesRemaining = 0;
    eyePerformanceTestPhase = EyePerformanceTestPhase::Idle;
}

void RuntimeTestHooks::FailEyeValidationTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    eyeValidationTestActive = false;
    waitingForEyeValidationWorld = false;
    eyeValidationTestPhase = EyeValidationTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupEyeValidationTestFixture();
    diagnostics.ReportError("Eye runtime validation failed: " + message);
}

void RuntimeTestHooks::CleanupEyeValidationTestFixture() noexcept
{
    if (!eyeValidationFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            eyeValidationFixtureDirectory,
            removeError);
    }
    eyeValidationFixtureDirectory.clear();
    eyeValidationScenePath.clear();
    eyeValidationDeferredScenePath.clear();
    eyeValidationDualShellScenePath.clear();
    eyeValidationFailureScenePath.clear();
    eyeValidationSourceDigest.clear();
    eyeValidationArtifactGenerationKey.clear();
    eyeValidationLutTextureIdentity = 0;
    eyeValidationBaselineWorld = {};
}

} // namespace VL
