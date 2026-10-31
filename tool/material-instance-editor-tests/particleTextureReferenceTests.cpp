#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "editor/command/editorCommand.h"
#include "editor/service/assetDocumentService.h"
#include "particles/particleTextureReference.h"

namespace
{

using namespace VL;

class ParticleTextureReferenceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        root = std::filesystem::temp_directory_path() /
            ("vl-particle-texture-" + std::to_string(stamp));
        std::filesystem::create_directories(root / "Textures");
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
            (void)ValidateParticleTextureReference(
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

TEST_F(ParticleTextureReferenceTest, AcceptsResourceRootRelativeImage)
{
    const std::filesystem::path file =
        root / "Textures" / "Fire.PNG";
    WriteFile(file);

    const std::filesystem::path resolved =
        ValidateParticleTextureReference(
            root,
            "Textures/Fire.PNG",
            "emitters[0].texture");
    EXPECT_EQ(
        std::filesystem::weakly_canonical(resolved),
        std::filesystem::weakly_canonical(file));
}

TEST_F(ParticleTextureReferenceTest, RejectsAbsolutePathWithFieldName)
{
    const std::string absolute =
        (root / "Textures" / "Fire.png").string();
    const std::string message = ErrorMessage(
        root,
        absolute,
        "emitters[2].texture");

    EXPECT_NE(message.find("emitters[2].texture"), std::string::npos);
    EXPECT_NE(message.find("resource-root-relative"), std::string::npos);
}

TEST_F(ParticleTextureReferenceTest, RejectsTraversalAndMissingFile)
{
    const std::string traversal = ErrorMessage(
        root,
        "../outside.png",
        "texture");
    EXPECT_NE(traversal.find("traversal"), std::string::npos);

    const std::string missing = ErrorMessage(
        root,
        "Textures/missing.png",
        "texture");
    EXPECT_NE(missing.find("missing file"), std::string::npos);
}

TEST_F(ParticleTextureReferenceTest, RejectsEscapeThroughSymlink)
{
    const std::filesystem::path outside =
        root.parent_path() /
        (root.filename().string() + "-outside.png");
    WriteFile(outside);
    std::error_code error;
    std::filesystem::create_symlink(
        outside,
        root / "Textures" / "linked.png",
        error);
    if (error)
    {
        std::error_code cleanup;
        std::filesystem::remove(outside, cleanup);
        GTEST_SKIP() << "Filesystem symlinks are unavailable: "
                     << error.message();
    }

    const std::string message = ErrorMessage(
        root,
        "Textures/linked.png",
        "texture");
    EXPECT_NE(message.find("escapes resource root"), std::string::npos);

    std::filesystem::remove(outside, error);
}

TEST_F(ParticleTextureReferenceTest, RejectsNonImageExtension)
{
    WriteFile(root / "Textures" / "particle.txt");
    const std::string message = ErrorMessage(
        root,
        "Textures/particle.txt",
        "emitters[1].texture");

    EXPECT_NE(message.find("emitters[1].texture"), std::string::npos);
    EXPECT_NE(message.find("png/jpg/jpeg/tga/bmp"), std::string::npos);
}

TEST_F(ParticleTextureReferenceTest, ServiceOpenRejectsInvalidTextureReferences)
{
    using namespace VL::Editor;
    const auto absoluteTexture =
        (root / "Textures" / "Fire.png").string();
    WriteFile(root / "Textures" / "particle.txt");
    struct Case
    {
        std::string texture;
        std::string expected;
    };
    const std::vector<Case> cases{
        {absoluteTexture, "resource-root-relative"},
        {"../outside.png", "traversal"},
        {"Textures/missing.png", "missing file"},
        {"Textures/particle.txt", "png/jpg/jpeg/tga/bmp"}};

    uint64_t commandId = 1;
    for (const Case& testCase : cases)
    {
        const std::filesystem::path asset =
            root / ("PS_texture_" +
                std::to_string(commandId) + ".json");
        {
            std::ofstream file(asset, std::ios::binary);
            file << nlohmann::json{
                {"name", "PS_texture"},
                {"type", "particleSystem"},
                {"emitters", nlohmann::json::array({
                    nlohmann::json{
                        {"name", "fire"},
                        {"maxParticles", 8},
                        {"texture", testCase.texture}
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
            result.message.find("emitters[0].texture"),
            std::string::npos) << result.message;
        EXPECT_NE(
            result.message.find(testCase.expected),
            std::string::npos) << result.message;
    }
}

TEST_F(ParticleTextureReferenceTest, SourceKeyTracksPathSizeTimestampAndMissingState)
{
    const std::filesystem::path texture =
        root / "Textures" / "Fire.png";
    const std::string reference = "Textures/Fire.png";
    WriteFile(texture, "a");

    const std::string first =
        BuildParticleTextureSourceKey(reference, texture);
    const std::string stable =
        BuildParticleTextureSourceKey(reference, texture);
    EXPECT_EQ(first, stable);
    EXPECT_NE(first.find("Textures/Fire.png"), std::string::npos);

    // 同路径内容变化：大小与修改时间进入 key，Synchronize 会重建发射器。
    WriteFile(texture, "much longer content");
    const std::string changed =
        BuildParticleTextureSourceKey(reference, texture);
    EXPECT_NE(first, changed);

    // 空路径固定为空 key，无纹理发射器不会因为其它文件变化而重建。
    EXPECT_TRUE(
        BuildParticleTextureSourceKey("", texture).empty());

    std::filesystem::remove(texture);
    const std::string missing =
        BuildParticleTextureSourceKey(reference, texture);
    EXPECT_NE(missing.find("missing"), std::string::npos);
    EXPECT_NE(missing, changed);
}

TEST_F(ParticleTextureReferenceTest, SourceKeyIncludesMipmapConfiguration)
{
    const std::filesystem::path texture =
        root / "Textures" / "Fire.png";
    const std::string reference = "Textures/Fire.png";
    WriteFile(texture, "same content");

    const std::string mipmapped =
        BuildParticleTextureSourceKey(reference, texture, true);
    const std::string atlas =
        BuildParticleTextureSourceKey(reference, texture, false);
    EXPECT_NE(mipmapped, atlas);
    EXPECT_EQ(mipmapped,
        BuildParticleTextureSourceKey(reference, texture, true));
    EXPECT_EQ(atlas,
        BuildParticleTextureSourceKey(reference, texture, false));
}
