#include "engine/runtimeTestHooks.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include "engine/diagnosticsSubsystem.h"
#include "engine/testing/runtimeTestFixtures.h"
#include "engine/testing/runtimeValidationServices.h"
#include "render/hair/hairAssets.h"
#include "shader/build/atomicFile.h"

namespace VL
{
using namespace RuntimeTestFixtures;
namespace
{

constexpr int FirstHairDebugView = 21;
constexpr int LastHairDebugView = 41;

bool ContainsMeshName(
    const RuntimeHairValidationSnapshot& snapshot,
    std::string_view expectedName)
{
    return std::find(
               snapshot.meshObjectNames.begin(),
               snapshot.meshObjectNames.end(),
               expectedName) != snapshot.meshObjectNames.end();
}

const RuntimeHairMaterialSnapshot* FindHairMaterial(
    const RuntimeHairValidationSnapshot& snapshot,
    std::string_view name)
{
    for (const RuntimeHairMaterialSnapshot& material : snapshot.materials)
    {
        if (material.name == name ||
            std::filesystem::path(material.name).stem().generic_string() == name ||
            material.name.find(name) != std::string::npos)
        {
            // 运行时名称是规范化 MI 资产路径，验证短名只用于定位 fixture。
            return &material;
        }
    }
    return nullptr;
}

bool HasMeshNamePrefix(
    const RuntimeHairValidationSnapshot& snapshot,
    std::string_view prefix,
    size_t expectedCount)
{
    size_t actualCount = 0;
    for (const std::string& meshName : snapshot.meshObjectNames)
    {
        if (meshName.rfind(prefix, 0) == 0)
        {
            ++actualCount;
        }
    }
    return actualCount == expectedCount;
}

void RequireHairCondition(
    bool condition,
    const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void RequireHairParameter(
    const RuntimeHairMaterialSnapshot& material,
    const char* parameterName,
    const char* expectedValue)
{
    const auto parameterIt = material.parameterValues.find(parameterName);
    RequireHairCondition(
        parameterIt != material.parameterValues.end(),
        "Hair validation material is missing parameter " +
            std::string(parameterName) + ": " + material.name);
    RequireHairCondition(
        parameterIt->second == expectedValue,
        "Hair validation parameter mismatch " +
            std::string(parameterName) + " on " + material.name +
            ": actual=" + parameterIt->second +
            ", expected=" + expectedValue);
}

void ValidateHairSceneSnapshot(
    const RuntimeHairValidationSnapshot& snapshot,
    size_t sceneIndex,
    std::string_view expectedSourceIdentity)
{
    RequireHairCondition(
        snapshot.captured,
        "Hair validation snapshot was not captured");
    RequireHairCondition(
        snapshot.hasHairResources,
        "Hair validation World-local HairResourceSet is missing");
    RequireHairCondition(
        !snapshot.sourceIdentity.empty() &&
            snapshot.sourceIdentity == expectedSourceIdentity,
        "Hair validation authoring source identity is not stable");
    RequireHairCondition(
        snapshot.hairLutTextureIdentity != 0 &&
            snapshot.hairLutTextureIdentity ==
                snapshot.boundHairWorldTextureIdentity,
        "Hair LUT texture is not consistently bound as a World-local texture");
    RequireHairCondition(
        snapshot.forwardHairLutBinding,
        "forwardTransparent must expose hairAzimuthalLut at binding 1");
    RequireHairCondition(
        snapshot.deferredHairLutBinding,
        "deferredLighting must expose hairAzimuthalLut at binding 10");
    RequireHairCondition(
        !snapshot.materials.empty(),
        "Hair validation scene did not load a Hair material instance");

    bool foundForwardProbe = false;
    bool foundShadowCapableHairMaterial = false;
    for (const RuntimeHairMaterialSnapshot& material : snapshot.materials)
    {
        RequireHairCondition(
            material.shadingModelMacro == "SHADING_MODEL_HAIR",
            "Hair fixture material fell back to a non-Hair shading model: " +
                material.name);
        RequireHairCondition(
            material.hasRenderPipeline,
            "Hair fixture material has no live render pipeline: " +
                material.name);
        RequireHairCondition(
            material.hasHairParameters,
            "Hair fixture material parameters do not match M_hair schema: " +
                material.name);
        if (material.renderMode == "TransparentAlphaBlend")
        {
            foundForwardProbe = true;
            RequireHairCondition(
                material.forwardDescriptorLayoutCompatible,
                "M_hairProbe forward pipeline is incompatible with pass Set 3");
        }
        if (material.hasShadowPipeline)
        {
            foundShadowCapableHairMaterial = true;
        }
    }

    switch (sceneIndex)
    {
    case 0:
        RequireHairCondition(
            ContainsMeshName(snapshot, "HairSingleCard"),
            "single-card fixture object is missing");
        break;
    case 1:
        RequireHairCondition(
            ContainsMeshName(snapshot, "HairCrossCardMirrored"),
            "crossed-card fixture did not exercise mirrored UV orientation");
        break;
    case 2:
        RequireHairCondition(
            HasMeshNamePrefix(snapshot, "HairDenseCard_", 6),
            "dense-hair fixture did not load the expected card/LOD sweep");
        break;
    case 3:
        RequireHairCondition(
            foundForwardProbe,
            "backlit-hair fixture did not load M_hairProbe");
        break;
    case 4:
        RequireHairCondition(
            foundShadowCapableHairMaterial,
            "hair-shadow fixture has no ShadowDepth-capable Hair material");
        RequireHairCondition(
            ContainsMeshName(snapshot, "HairShadowCardMirrored"),
            "hair-shadow fixture did not exercise mirrored coverage");
        break;
    case 5:
    {
        const RuntimeHairMaterialSnapshot* scatter = FindHairMaterial(
            snapshot,
            "MI_hair_sweep_scatter");
        const RuntimeHairMaterialSnapshot* backlit = FindHairMaterial(
            snapshot,
            "MI_hair_sweep_backlit");
        const RuntimeHairMaterialSnapshot* specular = FindHairMaterial(
            snapshot,
            "MI_hair_sweep_specular");
        const RuntimeHairMaterialSnapshot* roughness = FindHairMaterial(
            snapshot,
            "MI_hair_sweep_roughness");
        const RuntimeHairMaterialSnapshot* tangent = FindHairMaterial(
            snapshot,
            "MI_hair_sweep_tangent");
        RequireHairCondition(
            scatter != nullptr && backlit != nullptr &&
                specular != nullptr && roughness != nullptr &&
                tangent != nullptr,
            "Hair parameter sweep did not load all five material instances");
        RequireHairParameter(
            *scatter,
            "u_hairScattering",
            "1,0,0.22,0.25");
        RequireHairParameter(
            *backlit,
            "u_hairScattering",
            "0.25,1,0.22,0.25");
        RequireHairParameter(
            *specular,
            "u_pbrFactors",
            "0.32,0,1,0.95");
        RequireHairParameter(
            *roughness,
            "u_hairScattering",
            "0.25,0.35,0.85,0.65");
        RequireHairCondition(
            ContainsMeshName(snapshot, "HairSweepTangentMirrored"),
            "Hair tangent sweep did not exercise mirrored handedness");
        break;
    }
    default:
        throw std::runtime_error("Unknown Hair validation scene index");
    }
}

void RestoreHairMetadataFile(
    const std::filesystem::path& path,
    bool existed,
    const std::string& originalContents)
{
    if (existed)
    {
        WriteTextFileAtomically(path, originalContents);
        return;
    }

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
}

} // namespace

bool RuntimeTestHooks::BeginHairValidationTest(
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
        const std::filesystem::path authoredMetadataPath =
            resourceRoot / "hair" / "hairAzimuthalLut.json";
        const std::filesystem::path generatedMetadataPath =
            resourceRoot / "generated" / "hairAzimuthalLut.json";

        if (!std::filesystem::is_regular_file(authoredMetadataPath))
        {
            throw std::runtime_error(
                "Hair runtime validation requires the authored LUT metadata "
                "asset: " + authoredMetadataPath.string());
        }

        const HairAzimuthalLutAsset authoredAsset =
            LoadHairAzimuthalLutAsset(resourceRoot);
        hairValidationExpectedSourceIdentity =
            authoredAsset.metadata.sourceIdentity;

        std::filesystem::create_directories(generatedMetadataPath.parent_path());
        hairValidationGeneratedMetadataPath = generatedMetadataPath.string();
        hairValidationHadGeneratedMetadata =
            std::filesystem::is_regular_file(generatedMetadataPath);
        hairValidationOriginalGeneratedMetadata =
            hairValidationHadGeneratedMetadata
            ? ReadTextFileBytes(generatedMetadataPath)
            : std::string();

        const HairValidationFixture fixture =
            CreateHairValidationFixtures(resourcePath);
        hairValidationFixtureDirectory = fixture.directory.string();
        hairValidationScenePaths.clear();
        hairValidationScenePaths.reserve(fixture.scenePaths.size());
        for (const std::filesystem::path& scenePath : fixture.scenePaths)
        {
            hairValidationScenePaths.push_back(scenePath.string());
        }
        if (hairValidationScenePaths.size() != 6)
        {
            throw std::runtime_error(
                "Hair validation fixture must contain five reference scenes and one parameter sweep");
        }

        hairValidationSceneIndex = 0;
        hairValidationNextDebugView = FirstHairDebugView;
        waitingForHairValidationWorld = false;
        hairValidationTestPhase = HairValidationTestPhase::WaitWorldLoad;
        hairValidationTestActive = true;
        runtimeTestStatus = RuntimeTestStatus::Running;
        diagnostics.ReportInfo(
            "Hair runtime validation started: five fixed scenes, one parameter/geometry sweep and debug views 21-41.");
        return true;
    }
    catch (const std::exception& exception)
    {
        runtimeTestStatus = RuntimeTestStatus::Failed;
        diagnostics.ReportError(
            std::string("Failed to create Hair runtime validation fixture: ") +
            exception.what());
        CleanupHairValidationTestFixture();
        return false;
    }
}

void RuntimeTestHooks::UpdateHairValidationTest(
    RuntimeValidationServices& validationServices,
    const DiagnosticsSubsystem& diagnostics)
{
    if (!hairValidationTestActive)
    {
        return;
    }

    try
    {
        if (hairValidationTestPhase == HairValidationTestPhase::ValidateWorld)
        {
            const RuntimeHairValidationSnapshot snapshot =
                validationServices.CaptureHairValidationSnapshot();
            ValidateHairSceneSnapshot(
                snapshot,
                hairValidationSceneIndex,
                hairValidationExpectedSourceIdentity);

            if (hairValidationSceneIndex + 1 <
                hairValidationScenePaths.size())
            {
                ++hairValidationSceneIndex;
                hairValidationTestPhase =
                    HairValidationTestPhase::WaitWorldLoad;
                diagnostics.ReportInfo(
                    "Hair validation scene contract passed; advancing to scene " +
                    std::to_string(hairValidationSceneIndex + 1) + "/" +
                    std::to_string(hairValidationScenePaths.size()) + ".");
            }
            else
            {
                hairValidationTestPhase =
                    HairValidationTestPhase::QueueDebugView;
                diagnostics.ReportInfo(
                    "Hair material and geometry sweeps passed; starting debug view contract.");
            }
            return;
        }

        if (hairValidationTestPhase == HairValidationTestPhase::QueueDebugView)
        {
            if (hairValidationNextDebugView > LastHairDebugView)
            {
                hairValidationTestActive = false;
                hairValidationTestPhase = HairValidationTestPhase::Idle;
                runtimeTestStatus = RuntimeTestStatus::Succeeded;
                CleanupHairValidationTestFixture();
                diagnostics.ReportInfo(
                    "Hair runtime validation succeeded: debug views 21-41 are command-addressable.");
                return;
            }

            RuntimeCommand command;
            command.type = RuntimeCommandType::SetDebugViewMode;
            command.intValue = hairValidationNextDebugView;
            command.sourceText = "runtime-test: hair-validation-debug-view";
            validationServices.QueueRuntimeCommand(std::move(command));
            hairValidationTestPhase = HairValidationTestPhase::WaitDebugView;
            return;
        }

        if (hairValidationTestPhase == HairValidationTestPhase::WaitDebugView)
        {
            RequireHairCondition(
                validationServices.GetDebugViewMode() ==
                    hairValidationNextDebugView,
                "Hair debug view command did not reach the renderer: mode=" +
                    std::to_string(hairValidationNextDebugView));
            ++hairValidationNextDebugView;
            hairValidationTestPhase = HairValidationTestPhase::QueueDebugView;
            return;
        }
    }
    catch (const std::exception& exception)
    {
        FailHairValidationTest(
            exception.what(),
            diagnostics);
    }
}

void RuntimeTestHooks::FailHairValidationTest(
    const std::string& message,
    const DiagnosticsSubsystem& diagnostics)
{
    hairValidationTestActive = false;
    waitingForHairValidationWorld = false;
    hairValidationTestPhase = HairValidationTestPhase::Idle;
    runtimeTestStatus = RuntimeTestStatus::Failed;
    CleanupHairValidationTestFixture();
    diagnostics.ReportError(
        "Hair runtime validation failed: " + message);
}

void RuntimeTestHooks::CleanupHairValidationTestFixture() noexcept
{
    try
    {
        if (!hairValidationGeneratedMetadataPath.empty())
        {
            RestoreHairMetadataFile(
                hairValidationGeneratedMetadataPath,
                hairValidationHadGeneratedMetadata,
                hairValidationOriginalGeneratedMetadata);
        }
    }
    catch (...)
    {
    }

    if (!hairValidationFixtureDirectory.empty())
    {
        std::error_code removeError;
        std::filesystem::remove_all(
            hairValidationFixtureDirectory,
            removeError);
    }

    hairValidationFixtureDirectory.clear();
    hairValidationScenePaths.clear();
    hairValidationGeneratedMetadataPath.clear();
    hairValidationOriginalGeneratedMetadata.clear();
    hairValidationHadGeneratedMetadata = false;
    hairValidationExpectedSourceIdentity.clear();
}

} // namespace VL
