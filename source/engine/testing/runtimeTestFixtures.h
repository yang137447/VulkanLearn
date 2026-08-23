#pragma once

// File responsibility: Declares fixture construction and value-comparison
// helpers shared by runtime-test implementation units. Production owners stay
// behind RuntimeValidationServices and are not exposed through this header.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class Material;
class MaterialInstance;
class Texture;

namespace VL
{

class DiagnosticsSubsystem;
class RendererObjectResourceEntry;
class RuntimeValidationServices;
struct RuntimeRendererResourceFingerprint;
struct WorldHandle;

namespace RuntimeTestFixtures
{

inline constexpr int RetireDrainFrameBudget = 180;
inline constexpr std::chrono::seconds ShaderAsyncWaitTimeout{8};
inline constexpr std::chrono::seconds ShaderDefinitionWaitTimeout{12};

struct ShaderReloadRuntimeSnapshot
{
    std::uintptr_t material = 0;
    std::uintptr_t surfacePipeline = 0;
    std::uintptr_t shadowPipeline = 0;
    std::string surfaceGeneration;
    std::string shadowGeneration;
    std::string manifestDigest;
    std::string surfaceVertexDigest;
    std::string surfaceFragmentDigest;
    std::string shadowVertexDigest;
    std::string shadowFragmentDigest;
};

struct ShaderAutoReloadRuntimeSnapshot
{
    std::string surfaceLogicalBuildId;
    std::string shadowLogicalBuildId;
    std::string surfaceGeneration;
    std::string shadowGeneration;
    std::string manifestDigest;
    std::string resolvedGeneration;
};

std::string FormatBackendIdentityCounts(
    const std::array<size_t, 9>& counts);
std::string FormatImageResourceDebugNameDifference(
    const std::vector<std::string>& baseline,
    const std::vector<std::string>& current);

bool SameWorldHandle(
    const WorldHandle& lhs,
    const WorldHandle& rhs);
bool SameRendererResourceFingerprint(
    const RuntimeRendererResourceFingerprint& lhs,
    const RuntimeRendererResourceFingerprint& rhs);
bool SameRendererResourceFingerprintExceptWorldTexture(
    const RuntimeRendererResourceFingerprint& lhs,
    const RuntimeRendererResourceFingerprint& rhs,
    const std::string& replaceableTextureName);
std::string DescribeRendererResourceFingerprintDifference(
    const RuntimeRendererResourceFingerprint& lhs,
    const RuntimeRendererResourceFingerprint& rhs);
std::string FormatRendererResourceFingerprint(
    const RuntimeRendererResourceFingerprint& fingerprint);

std::string ReadTextFileBytes(
    const std::filesystem::path& path);
ShaderReloadRuntimeSnapshot CaptureShaderReloadRuntimeSnapshot(
    RuntimeValidationServices& validationServices);
bool SameShaderReloadRuntimeSnapshot(
    const ShaderReloadRuntimeSnapshot& lhs,
    const ShaderReloadRuntimeSnapshot& rhs);
ShaderAutoReloadRuntimeSnapshot
    CaptureShaderAutoReloadRuntimeSnapshot(
        RuntimeValidationServices& validationServices);
bool SameShaderAutoReloadRuntimeSnapshot(
    const ShaderAutoReloadRuntimeSnapshot& lhs,
    const ShaderAutoReloadRuntimeSnapshot& rhs);
bool ContainsAllSourceIdentities(
    const std::vector<std::string>& sources,
    const std::vector<std::string>& expected);

std::filesystem::path CreateShaderReloadTestScene(
    const std::string& resourcePath);
std::filesystem::path
    CreateWorldGraphTransactionHighLightScene(
        const std::filesystem::path& baseScenePath,
        size_t pointLightCount);
std::string BuildShaderReloadCompatibleSource(
    const std::string& expression);
std::string BuildShaderReloadSyntaxErrorSource();
std::string BuildShaderReloadAbiIncompatibleSource();
std::string ReplaceFirstOccurrence(
    const std::string& source,
    const std::string& from,
    const std::string& to);

std::shared_ptr<Material> FindShaderReloadTestMaterial();
std::shared_ptr<MaterialInstance>
    FindShaderReloadTestMaterialInstance();
std::shared_ptr<Texture> FindShaderReloadTestPrimaryTexture();
std::shared_ptr<MaterialInstance>
    FindShaderReloadBatchTestMaterialInstance();
std::shared_ptr<RendererObjectResourceEntry>
    FindShaderReloadTestObjectResources();
uint64_t GetObjectDescriptorPoolIdentity(
    const std::shared_ptr<RendererObjectResourceEntry>& entry);
void ValidateShaderDefinitionMigratedState(
    const std::shared_ptr<MaterialInstance>& instance,
    std::uintptr_t retainedTextureIdentity,
    bool expectExtra,
    bool expectRemoved,
    bool expectMultiMain);

std::filesystem::path CreateGeneratedMaterialFailureScene(
    const std::string& resourcePath);
std::filesystem::path CreateGeneratedMeshFailureScene(
    const std::string& resourcePath);
std::filesystem::path CreateGeneratedTextureFailureScene(
    const std::string& resourcePath);
std::filesystem::path CreateGeneratedHighLightStressScene(
    const std::string& resourcePath);

struct HairValidationFixture
{
    std::filesystem::path directory;
    std::vector<std::filesystem::path> scenePaths;
};

HairValidationFixture CreateHairValidationFixtures(
    const std::string& resourcePath);

void UpdateMaxPendingRetiredResources(
    size_t pendingCount,
    size_t& maxPendingRetiredResources);
void CleanupGeneratedRuntimeFixture(
    const std::string& fixtureDirectory,
    const DiagnosticsSubsystem& diagnostics);
void CleanupGeneratedRuntimeFixtureIfNeeded(
    bool& cleanupFixture,
    std::string& fixtureDirectory,
    const DiagnosticsSubsystem& diagnostics);

} // namespace RuntimeTestFixtures
} // namespace VL
