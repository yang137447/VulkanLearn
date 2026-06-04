#include "engine/runtimeTestHooks.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "engine/diagnosticsSubsystem.h"
#include "engine/runtimeCommandExecutor.h"
#include "render/resource/resourceRetireQueue.h"

namespace VL
{
namespace
{

constexpr int RetireDrainFrameBudget = 180;

bool SameWorldHandle(const WorldHandle& lhs, const WorldHandle& rhs)
{
    return lhs.IsValid() &&
        rhs.IsValid() &&
        lhs.generation == rhs.generation &&
        lhs.scenePath == rhs.scenePath;
}

bool SameRendererResourceFingerprint(
    const RuntimeRendererResourceFingerprint& lhs,
    const RuntimeRendererResourceFingerprint& rhs)
{
    return lhs.captured &&
        rhs.captured &&
        lhs.worldOwnerGeneration == rhs.worldOwnerGeneration &&
        lhs.worldTextures == rhs.worldTextures &&
        lhs.renderableObjects == rhs.renderableObjects &&
        lhs.materials == rhs.materials &&
        lhs.materialInstances == rhs.materialInstances &&
        lhs.objectResources == rhs.objectResources &&
        lhs.textures == rhs.textures &&
        lhs.passMaterialInstances == rhs.passMaterialInstances;
}

std::string FormatRendererResourceFingerprint(
    const RuntimeRendererResourceFingerprint& fingerprint)
{
    if (!fingerprint.captured)
    {
        return "not captured";
    }

    return "ownerGeneration=" +
        std::to_string(fingerprint.worldOwnerGeneration) +
        ", worldTextures=" +
        std::to_string(fingerprint.worldTextures.size()) +
        ", renderableObjects=" +
        std::to_string(fingerprint.renderableObjects.size()) +
        ", materials=" +
        std::to_string(fingerprint.materials.size()) +
        ", materialInstances=" +
        std::to_string(fingerprint.materialInstances.size()) +
        ", objectResources=" +
        std::to_string(fingerprint.objectResources.size()) +
        ", textures=" +
        std::to_string(fingerprint.textures.size()) +
        ", passMaterialInstances=" +
        std::to_string(fingerprint.passMaterialInstances.size());
}

std::filesystem::path BuildGeneratedFixtureDirectory(
    const std::string& resourcePath,
    const std::string& namePrefix)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::path(resourcePath) /
        "generated" /
        "runtime-validation" /
        (namePrefix + std::to_string(now));
}

void WriteTextFile(
    const std::filesystem::path& path,
    const std::string& contents)
{
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to write generated runtime test fixture: " + path.string());
    }

    file << contents;
}

std::filesystem::path CreateGeneratedMaterialFailureScene(const std::string& resourcePath)
{
    const std::filesystem::path fixtureDirectory =
        BuildGeneratedFixtureDirectory(resourcePath, "bad-material-");
    std::filesystem::create_directories(fixtureDirectory);

    const std::filesystem::path materialInstancePath = fixtureDirectory / "MI_bad_runtime_test.json";
    const std::filesystem::path meshAssetPath = fixtureDirectory / "SM_bad_runtime_test.json";
    const std::filesystem::path scenePath = fixtureDirectory / "SC_bad_material_runtime_test.json";

    // The generated scene and mesh are valid enough to reach renderer resource
    // loading. The material instance then fails on an unknown parameter, which
    // verifies rollback after pass materials/resources have already been staged.
    WriteTextFile(
        materialInstancePath,
        R"({
  "name": "Bad Runtime Test Material Instance",
  "type": "materialInstance",
  "material": "shader/glsl/M_vertexColor.json",
  "parameters": {
    "u_missingGeneratedRuntimeTestParam": [1, 1, 1, 1]
  },
  "textures": {}
})");

    WriteTextFile(
        meshAssetPath,
        "{\n"
        "  \"name\": \"Bad Runtime Test Mesh\",\n"
        "  \"type\": \"mesh\",\n"
        "  \"modelDataPath\": \"models/datas/axis.obj\",\n"
        "  \"materialSlots\": [\n"
        "    {\n"
        "      \"name\": \"Default\",\n"
        "      \"materialInstancePath\": \"" + materialInstancePath.generic_string() + "\"\n"
        "    }\n"
        "  ]\n"
        "}\n");

    WriteTextFile(
        scenePath,
        "{\n"
        "  \"name\": \"Bad Material Runtime Test Scene\",\n"
        "  \"type\": \"scene\",\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"name\": \"BadMaterialMesh_001\",\n"
        "      \"type\": \"mesh\",\n"
        "      \"modelPath\": \"" + meshAssetPath.generic_string() + "\",\n"
        "      \"position\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1],\n"
        "      \"rotation\": [0, 0, 0]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"Camera_01\",\n"
        "      \"type\": \"camera\",\n"
        "      \"fov\": 90,\n"
        "      \"near_clip\": 0.1,\n"
        "      \"far_clip\": 2000,\n"
        "      \"position\": [0, 2, 6],\n"
        "      \"rotation\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1]\n"
        "    }\n"
        "  ]\n"
        "}\n");

    return scenePath;
}

std::filesystem::path CreateGeneratedMeshFailureScene(const std::string& resourcePath)
{
    const std::filesystem::path fixtureDirectory =
        BuildGeneratedFixtureDirectory(resourcePath, "bad-mesh-");
    std::filesystem::create_directories(fixtureDirectory);

    const std::filesystem::path meshAssetPath = fixtureDirectory / "SM_bad_mesh_runtime_test.json";
    const std::filesystem::path scenePath = fixtureDirectory / "SC_bad_mesh_runtime_test.json";

    WriteTextFile(
        meshAssetPath,
        "{\n"
        "  \"name\": \"Bad Mesh Runtime Test Mesh\",\n"
        "  \"type\": \"mesh\",\n"
        "  \"modelDataPath\": \"models/datas/RUNTIME_VALIDATION_MISSING_ASSET.obj\",\n"
        "  \"materialSlots\": [\n"
        "    {\n"
        "      \"name\": \"Default\",\n"
        "      \"materialInstancePath\": \"materials/MI_axis.json\"\n"
        "    }\n"
        "  ]\n"
        "}\n");

    WriteTextFile(
        scenePath,
        "{\n"
        "  \"name\": \"Bad Mesh Runtime Test Scene\",\n"
        "  \"type\": \"scene\",\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"name\": \"BadMesh_001\",\n"
        "      \"type\": \"mesh\",\n"
        "      \"modelPath\": \"" + meshAssetPath.generic_string() + "\",\n"
        "      \"position\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1],\n"
        "      \"rotation\": [0, 0, 0]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"Camera_01\",\n"
        "      \"type\": \"camera\",\n"
        "      \"fov\": 90,\n"
        "      \"near_clip\": 0.1,\n"
        "      \"far_clip\": 2000,\n"
        "      \"position\": [0, 2, 6],\n"
        "      \"rotation\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1]\n"
        "    }\n"
        "  ]\n"
        "}\n");

    return scenePath;
}

std::filesystem::path CreateGeneratedTextureFailureScene(const std::string& resourcePath)
{
    const std::filesystem::path fixtureDirectory =
        BuildGeneratedFixtureDirectory(resourcePath, "bad-texture-");
    std::filesystem::create_directories(fixtureDirectory);

    const std::filesystem::path textureAssetPath = fixtureDirectory / "T_bad_runtime_test.json";
    const std::filesystem::path materialInstancePath = fixtureDirectory / "MI_bad_texture_runtime_test.json";
    const std::filesystem::path meshAssetPath = fixtureDirectory / "SM_bad_texture_runtime_test.json";
    const std::filesystem::path scenePath = fixtureDirectory / "SC_bad_texture_runtime_test.json";

    WriteTextFile(
        textureAssetPath,
        R"({
  "name": "Bad Runtime Test Texture",
  "type": "texture",
  "source": "textures/datas/RUNTIME_VALIDATION_MISSING_ASSET.png",
  "colorSpace": "srgb",
  "mipmaps": true,
  "filter": "linear",
  "wrapMode": "repeat"
})");

    WriteTextFile(
        materialInstancePath,
        "{\n"
        "  \"name\": \"Bad Runtime Test Texture Material Instance\",\n"
        "  \"type\": \"materialInstance\",\n"
        "  \"material\": \"shader/glsl/M_unlit.json\",\n"
        "  \"parameters\": {\n"
        "    \"u_tintColor\": [1, 1, 1, 1]\n"
        "  },\n"
        "  \"textures\": {\n"
        "    \"albedoMap\": \"" + textureAssetPath.generic_string() + "\"\n"
        "  }\n"
        "}\n");

    WriteTextFile(
        meshAssetPath,
        "{\n"
        "  \"name\": \"Bad Texture Runtime Test Mesh\",\n"
        "  \"type\": \"mesh\",\n"
        "  \"modelDataPath\": \"models/datas/axis.obj\",\n"
        "  \"materialSlots\": [\n"
        "    {\n"
        "      \"name\": \"Default\",\n"
        "      \"materialInstancePath\": \"" + materialInstancePath.generic_string() + "\"\n"
        "    }\n"
        "  ]\n"
        "}\n");

    WriteTextFile(
        scenePath,
        "{\n"
        "  \"name\": \"Bad Texture Runtime Test Scene\",\n"
        "  \"type\": \"scene\",\n"
        "  \"objects\": [\n"
        "    {\n"
        "      \"name\": \"BadTextureMesh_001\",\n"
        "      \"type\": \"mesh\",\n"
        "      \"modelPath\": \"" + meshAssetPath.generic_string() + "\",\n"
        "      \"position\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1],\n"
        "      \"rotation\": [0, 0, 0]\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"Camera_01\",\n"
        "      \"type\": \"camera\",\n"
        "      \"fov\": 90,\n"
        "      \"near_clip\": 0.1,\n"
        "      \"far_clip\": 2000,\n"
        "      \"position\": [0, 2, 6],\n"
        "      \"rotation\": [0, 0, 0],\n"
        "      \"scale\": [1, 1, 1]\n"
        "    }\n"
        "  ]\n"
        "}\n");

    return scenePath;
}

std::filesystem::path CreateGeneratedHighLightStressScene(const std::string& resourcePath)
{
    constexpr int GeneratedPointLightCount = 96;
    constexpr double TwoPi = 6.28318530717958647692;

    const std::filesystem::path fixtureDirectory =
        BuildGeneratedFixtureDirectory(resourcePath, "high-lights-");
    std::filesystem::create_directories(fixtureDirectory);

    const std::filesystem::path scenePath = fixtureDirectory / "SC_high_light_runtime_test.json";
    std::ostringstream scene;
    scene
        << "{\n"
        << "  \"name\": \"High Light Runtime Test Scene\",\n"
        << "  \"type\": \"scene\",\n"
        << "  \"objects\": [\n"
        << "    {\n"
        << "      \"name\": \"Oak_Complex_Rules_001\",\n"
        << "      \"type\": \"mesh\",\n"
        << "      \"modelPath\": \"models/SM_speedtree_oak_complex_rules.json\",\n"
        << "      \"position\": [0, 0, 0],\n"
        << "      \"scale\": [1, 1, 1],\n"
        << "      \"rotation\": [0, 0, 0]\n"
        << "    },\n"
        << "    {\n"
        << "      \"name\": \"Axis_001\",\n"
        << "      \"type\": \"mesh\",\n"
        << "      \"modelPath\": \"models/SM_axis.json\",\n"
        << "      \"position\": [0, 0, 0],\n"
        << "      \"scale\": [1, 1, 1],\n"
        << "      \"rotation\": [0, 0, 0]\n"
        << "    },\n"
        << "    {\n"
        << "      \"name\": \"Sun_Light\",\n"
        << "      \"type\": \"directionalLight\",\n"
        << "      \"position\": [0, 1, 0],\n"
        << "      \"rotation\": [0, 45, 0],\n"
        << "      \"color\": [1, 1, 1],\n"
        << "      \"intensity\": 1.0\n"
        << "    },\n"
        << "    {\n"
        << "      \"name\": \"Camera_01\",\n"
        << "      \"type\": \"camera\",\n"
        << "      \"fov\": 90,\n"
        << "      \"near_clip\": 0.1,\n"
        << "      \"far_clip\": 2000,\n"
        << "      \"position\": [0, 2, 6],\n"
        << "      \"rotation\": [0, 0, 0],\n"
        << "      \"scale\": [1, 1, 1]\n"
        << "    },\n"
        << "    {\n"
        << "      \"name\": \"Environment_01\",\n"
        << "      \"type\": \"environment\",\n"
        << "      \"hdrPath\": \"hdri/sunset.exr\",\n"
        << "      \"cubeSize\": 512\n"
        << "    }";

    for (int lightIndex = 0; lightIndex < GeneratedPointLightCount; ++lightIndex)
    {
        const double angle = TwoPi * static_cast<double>(lightIndex) /
            static_cast<double>(GeneratedPointLightCount);
        const double x = std::cos(angle) * 6.0;
        const double z = std::sin(angle) * 6.0;

        scene
            << ",\n"
            << "    {\n"
            << "      \"name\": \"Generated_PointLight_" << lightIndex << "\",\n"
            << "      \"type\": \"pointLight\",\n"
            << "      \"position\": [" << x << ", 2.5, " << z << "],\n"
            << "      \"rotation\": [0, 0, 0],\n"
            << "      \"color\": [1.0, 0.85, 0.65],\n"
            << "      \"intensity\": 0.25,\n"
            << "      \"radius\": 5.0\n"
            << "    }";
    }

    scene
        << "\n"
        << "  ]\n"
        << "}\n";

    WriteTextFile(scenePath, scene.str());
    return scenePath;
}

void UpdateMaxPendingRetiredResources(size_t pendingCount, size_t& maxPendingRetiredResources)
{
    maxPendingRetiredResources = std::max(maxPendingRetiredResources, pendingCount);
}

void CleanupGeneratedRuntimeFixture(
    const std::string& fixtureDirectory,
    const DiagnosticsSubsystem& diagnostics)
{
    if (fixtureDirectory.empty())
    {
        return;
    }

    std::error_code errorCode;
    std::filesystem::remove_all(fixtureDirectory, errorCode);
    if (errorCode)
    {
        diagnostics.ReportWarning(
            "Failed to remove generated runtime test fixture: " +
            fixtureDirectory +
            " error=" +
            errorCode.message());
    }
}

void CleanupGeneratedRuntimeFixtureIfNeeded(
    bool& cleanupFixture,
    std::string& fixtureDirectory,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!cleanupFixture)
    {
        return;
    }

    cleanupFixture = false;
    CleanupGeneratedRuntimeFixture(fixtureDirectory, diagnostics);
    fixtureDirectory.clear();
}

} // namespace

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

    if (worldReloadStressActive || failureRollbackTestActive)
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

    if (worldReloadStressActive || failureRollbackTestActive)
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

void RuntimeTestHooks::Update(
    CommandBus& commandBus,
    const DiagnosticsSubsystem& diagnostics)
{
    if (worldReloadStressActive &&
        waitingForRetireDrain)
    {
        ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();
        const size_t pendingRetiredResources = retireQueue.GetPendingCount();
        UpdateMaxPendingRetiredResources(pendingRetiredResources, maxPendingRetiredResources);

        if (pendingRetiredResources == 0)
        {
            if (totalWorldReloads > 1 && maxPendingRetiredResources == 0)
            {
                worldReloadStressActive = false;
                waitingForRetireDrain = false;
                runtimeTestStatus = RuntimeTestStatus::Failed;
                diagnostics.ReportError(
                    "World reload stress failed because no retired world-local resources were observed after repeated reloads.");
                CleanupGeneratedRuntimeFixtureIfNeeded(
                    cleanupGeneratedReloadStressFixture,
                    generatedReloadStressFixtureDirectory,
                    diagnostics);
                return;
            }

            worldReloadStressActive = false;
            waitingForRetireDrain = false;
            runtimeTestStatus = RuntimeTestStatus::Succeeded;
            diagnostics.ReportInfo(
                "World reload stress completed: " +
                std::to_string(completedWorldReloads) +
                "/" +
                std::to_string(totalWorldReloads) +
                " reloads succeeded, retire queue max pending=" +
                std::to_string(maxPendingRetiredResources) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
            CleanupGeneratedRuntimeFixtureIfNeeded(
                cleanupGeneratedReloadStressFixture,
                generatedReloadStressFixtureDirectory,
                diagnostics);
            return;
        }

        --retireDrainFramesRemaining;
        if (retireDrainFramesRemaining <= 0)
        {
            worldReloadStressActive = false;
            waitingForRetireDrain = false;
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload stress failed because retired world-local resources did not drain before the frame budget expired. pending=" +
                std::to_string(pendingRetiredResources) +
                ", submittedEpoch=" +
                std::to_string(retireQueue.GetLastSubmittedEpoch()) +
                ", completedEpoch=" +
                std::to_string(retireQueue.GetLastCompletedEpoch()) +
                ".");
            CleanupGeneratedRuntimeFixtureIfNeeded(
                cleanupGeneratedReloadStressFixture,
                generatedReloadStressFixtureDirectory,
                diagnostics);
        }
        return;
    }

    if (worldReloadStressActive &&
        !waitingForWorldReloadResult &&
        remainingWorldReloads > 0)
    {
        RuntimeCommand command;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = worldReloadStressScenePath;
        command.sourceText = "runtime-test: reloadstress";
        commandBus.Queue(std::move(command));

        waitingForWorldReloadResult = true;
        --remainingWorldReloads;

        diagnostics.ReportInfo(
            "World reload stress queued load " +
            std::to_string(completedWorldReloads + 1) +
            "/" +
            std::to_string(totalWorldReloads));
        return;
    }

    if (!failureRollbackTestActive || waitingForFailureRollbackResult)
    {
        return;
    }

    RuntimeCommand command;
    command.type = RuntimeCommandType::LoadWorld;
    command.stringValue = failureRollbackScenePath;
    command.sourceText = "runtime-test: reloadfail";
    commandBus.Queue(std::move(command));

    waitingForFailureRollbackResult = true;
    diagnostics.ReportInfo(
        "World reload failure rollback test queued expected-failure load: " +
        failureRollbackScenePath);
}

void RuntimeTestHooks::NotifyCommandResult(
    const RuntimeCommandExecutionResult& commandResult,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!worldReloadStressActive || !waitingForWorldReloadResult)
    {
        if (!failureRollbackTestActive || !waitingForFailureRollbackResult)
        {
            return;
        }

        if (!commandResult.loadWorldAttempted)
        {
            return;
        }

        waitingForFailureRollbackResult = false;
        failureRollbackTestActive = false;
        const bool shouldCleanupGeneratedFixture = cleanupGeneratedFailureFixture;
        cleanupGeneratedFailureFixture = false;

        if (commandResult.loadWorldSucceeded)
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test expected load to fail, but it succeeded.");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        if (!failureRollbackExpectedErrorCode.empty())
        {
            if (!commandResult.loadWorldError.has_value())
            {
                runtimeTestStatus = RuntimeTestStatus::Failed;
                diagnostics.ReportError(
                    "World reload failure rollback test failed because the expected load failure did not expose a RuntimeError.");
                failureRollbackExpectedErrorCode.clear();
                if (shouldCleanupGeneratedFixture)
                {
                    CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                    generatedFailureFixtureDirectory.clear();
                }
                return;
            }

            if (commandResult.loadWorldError->code != failureRollbackExpectedErrorCode)
            {
                runtimeTestStatus = RuntimeTestStatus::Failed;
                diagnostics.ReportError(
                    "World reload failure rollback test failed because the load error code was " +
                    commandResult.loadWorldError->code +
                    ", expected " +
                    failureRollbackExpectedErrorCode +
                    ".");
                failureRollbackExpectedErrorCode.clear();
                if (shouldCleanupGeneratedFixture)
                {
                    CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                    generatedFailureFixtureDirectory.clear();
                }
                return;
            }
        }

        if (commandResult.worldChanged || commandResult.worldRuntimeBindingAttempted)
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test failed because a failed load attempted to rebind runtime world state.");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        if (!SameWorldHandle(
                commandResult.activeWorldBeforeCommand,
                commandResult.activeWorldAfterCommand))
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test failed because active world handle changed after the expected load failure.");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        if (!SameRendererResourceFingerprint(
                commandResult.rendererResourcesBeforeLoad,
                commandResult.rendererResourcesAfterLoad))
        {
            runtimeTestStatus = RuntimeTestStatus::Failed;
            diagnostics.ReportError(
                "World reload failure rollback test failed because renderer resource cache or pass material bindings changed after the expected load failure. before={" +
                FormatRendererResourceFingerprint(commandResult.rendererResourcesBeforeLoad) +
                "} after={" +
                FormatRendererResourceFingerprint(commandResult.rendererResourcesAfterLoad) +
                "}");
            failureRollbackExpectedErrorCode.clear();
            if (shouldCleanupGeneratedFixture)
            {
                CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
                generatedFailureFixtureDirectory.clear();
            }
            return;
        }

        runtimeTestStatus = RuntimeTestStatus::Succeeded;
        diagnostics.ReportInfo(
            "World reload failure rollback test completed: active world, renderer resource cache, and pass material bindings preserved after expected load failure.");
        failureRollbackExpectedErrorCode.clear();
        if (shouldCleanupGeneratedFixture)
        {
            CleanupGeneratedRuntimeFixture(generatedFailureFixtureDirectory, diagnostics);
            generatedFailureFixtureDirectory.clear();
        }
        return;
    }

    if (!commandResult.loadWorldAttempted)
    {
        return;
    }

    waitingForWorldReloadResult = false;

    if (!commandResult.loadWorldSucceeded)
    {
        worldReloadStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            "World reload stress aborted after " +
            std::to_string(completedWorldReloads) +
            "/" +
            std::to_string(totalWorldReloads) +
            " successful reloads.");
        CleanupGeneratedRuntimeFixtureIfNeeded(
            cleanupGeneratedReloadStressFixture,
            generatedReloadStressFixtureDirectory,
            diagnostics);
        return;
    }

    if (!commandResult.worldRuntimeBindingSucceeded)
    {
        worldReloadStressActive = false;
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            "World reload stress aborted because runtime rebinding failed after load " +
            std::to_string(completedWorldReloads + 1) +
            "/" +
            std::to_string(totalWorldReloads) +
            ".");
        CleanupGeneratedRuntimeFixtureIfNeeded(
            cleanupGeneratedReloadStressFixture,
            generatedReloadStressFixtureDirectory,
            diagnostics);
        return;
    }

    const size_t pendingRetiredResources = ResourceRetireQueue::GetInstance().GetPendingCount();
    UpdateMaxPendingRetiredResources(pendingRetiredResources, maxPendingRetiredResources);

    ++completedWorldReloads;
    if (remainingWorldReloads <= 0)
    {
        waitingForRetireDrain = true;
        retireDrainFramesRemaining = RetireDrainFrameBudget;
        diagnostics.ReportInfo(
            "World reload stress waiting for retire queue drain: pending=" +
            std::to_string(pendingRetiredResources) +
            ", maxPending=" +
            std::to_string(maxPendingRetiredResources) +
            ".");
    }
}

} // namespace VL
