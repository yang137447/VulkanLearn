#include "ueProjectInventory.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <gtest/gtest.h>

namespace
{
class UnrealProjectInventory : public ::testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<unsigned int> next{0};
        root = std::filesystem::temp_directory_path() /
            ("vl-ue-inventory-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(next++));
        std::filesystem::create_directories(root / "Content");
        project = root / "Sample.uproject";
        Write(project, R"({"EngineAssociation":"5.8","Plugins":[{"Name":"PoseSearch","Enabled":true}]})");
    }

    void TearDown() override
    {
        std::filesystem::remove_all(root);
    }

    void Write(const std::filesystem::path& path, const std::string& bytes)
    {
        std::ofstream stream(path, std::ios::binary);
        stream << bytes;
        stream.close();
        ASSERT_TRUE(stream);
    }

    std::filesystem::path root;
    std::filesystem::path project;
};

TEST_F(UnrealProjectInventory, ReportsDeclaredVersionAndExplicitlyUnverifiedHints)
{
    Write(root / "Content/PSD_Walk.uasset", "fixture-not-an-actual-ue-asset");
    const auto report = VL::AnimationPreflight::InventoryUnrealProject(project);
    EXPECT_EQ(report["declaredEngineAssociation"], "5.8");
    EXPECT_EQ(report["inputFileCount"], 1);
    EXPECT_EQ(report["files"][0]["path"], "Content/PSD_Walk.uasset");
    EXPECT_EQ(report["files"][0]["nameHint"], "pose-search-database-name");
    EXPECT_FALSE(report.contains("isGasp"));
    EXPECT_GE(report["limitations"].size(), 5u);
}

TEST_F(UnrealProjectInventory, ContentBytesInvalidateIdentityWithoutPathChange)
{
    Write(root / "Content/A.uasset", "first");
    const auto before = VL::AnimationPreflight::InventoryUnrealProject(project);
    Write(root / "Content/A.uasset", "other");
    const auto after = VL::AnimationPreflight::InventoryUnrealProject(project);
    EXPECT_NE(before["inputBlake3"], after["inputBlake3"]);
}

TEST_F(UnrealProjectInventory, IgnoresGeneratedOutputs)
{
    const auto before = VL::AnimationPreflight::InventoryUnrealProject(project);
    std::filesystem::create_directories(root / "Saved");
    Write(root / "Saved/Generated.uasset", "generated");
    const auto after = VL::AnimationPreflight::InventoryUnrealProject(project);
    EXPECT_EQ(before["inputBlake3"], after["inputBlake3"]);
}

TEST_F(UnrealProjectInventory, SortsInputsForRepeatableReports)
{
    Write(root / "Content/Z.uasset", "z");
    Write(root / "Content/A.uasset", "a");
    const auto first = VL::AnimationPreflight::InventoryUnrealProject(project);
    const auto second = VL::AnimationPreflight::InventoryUnrealProject(project);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first["files"][0]["path"], "Content/A.uasset");
}

TEST_F(UnrealProjectInventory, RejectsMalformedProjectDescriptor)
{
    Write(project, "[]");
    EXPECT_THROW(VL::AnimationPreflight::InventoryUnrealProject(project), std::invalid_argument);
}

TEST_F(UnrealProjectInventory, RejectsOutputUnderSourceBeforeCreatingDirectories)
{
    const auto output = root / "Generated/Subdir/report.json";
    EXPECT_THROW(VL::AnimationPreflight::ValidateUnrealInventoryOutput(project, output),
        std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(root / "Generated"));
}

TEST_F(UnrealProjectInventory, AllowsOutputOutsideSource)
{
    EXPECT_NO_THROW(VL::AnimationPreflight::ValidateUnrealInventoryOutput(
        project, root.parent_path() / "vl-report-outside-project.json"));
}

TEST_F(UnrealProjectInventory, IncludesUnicodeAssetNames)
{
    const auto name = std::filesystem::u8path(u8"\u8d44\u4ea7.uasset");
    Write(root / "Content" / name, "unicode-fixture");
    const auto report = VL::AnimationPreflight::InventoryUnrealProject(project);
    EXPECT_EQ(report["files"][0]["path"], u8"Content/\u8d44\u4ea7.uasset");
}

#ifdef _WIN32
TEST_F(UnrealProjectInventory, RejectsOutputUsingAnUppercaseAliasOfSource)
{
    auto spelling = root.wstring();
    for (auto& character : spelling)
    {
        if (character >= L'a' && character <= L'z')
        {
            character -= L'a' - L'A';
        }
    }
    EXPECT_THROW(VL::AnimationPreflight::ValidateUnrealInventoryOutput(
        project, std::filesystem::path(spelling) / "alias-report.json"), std::invalid_argument);
}
#endif
}
