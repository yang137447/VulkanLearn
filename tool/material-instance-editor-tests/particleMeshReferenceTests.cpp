#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "editor/command/editorCommand.h"
#include "editor/service/assetDocumentService.h"
#include "particles/particleMeshReference.h"

namespace
{

using namespace VL;

class ParticleMeshReferenceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        root = std::filesystem::temp_directory_path() /
            ("vl-particle-mesh-" + std::to_string(stamp));
        std::filesystem::create_directories(root / "Meshes");
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void WriteFile(
        const std::filesystem::path& path,
        const std::string& text = "content")
    {
        std::ofstream file(path, std::ios::binary);
        file << text;
    }

    static std::string ErrorMessage(
        const std::filesystem::path& resourceRoot,
        const std::string& reference,
        const std::string& field)
    {
        try
        {
            (void)ValidateParticleMeshReference(
                resourceRoot,
                reference,
                field);
        }
        catch (const std::exception& exception)
        {
            return exception.what();
        }
        return {};
    }

    std::filesystem::path root;
};

} // namespace

TEST_F(ParticleMeshReferenceTest, AcceptsResourceRootRelativeJson)
{
    const std::filesystem::path file =
        root / "Meshes" / "SM_cube.json";
    WriteFile(file);

    const std::filesystem::path resolved =
        ValidateParticleMeshReference(
            root,
            "Meshes/SM_cube.json",
            "emitters[0].mesh.path");
    EXPECT_EQ(
        std::filesystem::weakly_canonical(resolved),
        std::filesystem::weakly_canonical(file));
}

TEST_F(ParticleMeshReferenceTest, RejectsAbsoluteTraversalMissingAndNonJson)
{
    const std::string absolute =
        (root / "Meshes" / "SM_cube.json").string();
    EXPECT_NE(
        ErrorMessage(root, absolute, "mesh.path")
            .find("resource-root-relative"),
        std::string::npos);
    EXPECT_NE(
        ErrorMessage(root, "../SM_cube.json", "mesh.path")
            .find("traversal"),
        std::string::npos);
    EXPECT_NE(
        ErrorMessage(root, "Meshes/missing.json", "mesh.path")
            .find("missing file"),
        std::string::npos);

    WriteFile(root / "Meshes" / "cube.obj");
    EXPECT_NE(
        ErrorMessage(root, "Meshes/cube.obj", "mesh.path")
            .find("must use .json"),
        std::string::npos);
}

TEST_F(ParticleMeshReferenceTest, ServiceOpenRejectsInvalidMeshReferences)
{
    using namespace VL::Editor;
    const auto absoluteMesh =
        (root / "Meshes" / "SM_cube.json").string();
    WriteFile(root / "Meshes" / "cube.obj");
    struct Case
    {
        std::string meshPath;
        std::string expected;
    };
    const std::vector<Case> cases{
        {absoluteMesh, "resource-root-relative"},
        {"../SM_cube.json", "traversal"},
        {"Meshes/missing.json", "missing file"},
        {"Meshes/cube.obj", "must use .json"}};

    uint64_t commandId = 1;
    for (const Case& testCase : cases)
    {
        const std::filesystem::path asset =
            root / ("PS_mesh_" +
                std::to_string(commandId) + ".json");
        {
            std::ofstream file(asset, std::ios::binary);
            file << nlohmann::json{
                {"name", "PS_mesh"},
                {"type", "particleSystem"},
                {"emitters", nlohmann::json::array({
                    nlohmann::json{
                        {"name", "debris"},
                        {"maxParticles", 8},
                        {"renderMode", "mesh"},
                        {"mesh", nlohmann::json{
                            {"path", testCase.meshPath}
                        }}
                    }
                })}
            }.dump(4);
        }
        AssetDocumentService service(
            root,
            EditorAssetDocumentKind::ParticleSystem);
        EditorCommandEnvelope get;
        get.commandId = commandId++;
        get.source = EditorCommandSource::RuntimeTest;
        get.type = EditorCommandType::GetParticleDocument;
        get.payload = MaterialInstanceAssetPathPayload{
            asset.filename().generic_string()};
        const EditorCommandResult result = service.Execute(get);
        EXPECT_EQ(result.status, EditorCommandStatus::Failed)
            << result.message;
        EXPECT_EQ(result.errorCode, EditorErrorCode::ValidationFailed)
            << result.message;
        EXPECT_NE(
            result.message.find("emitters[0].mesh.path"),
            std::string::npos) << result.message;
        EXPECT_NE(
            result.message.find(testCase.expected),
            std::string::npos) << result.message;
    }
}
