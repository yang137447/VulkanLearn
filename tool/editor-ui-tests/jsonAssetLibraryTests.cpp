#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "editor/assets/editorJsonAssetLibrary.h"

namespace
{
using namespace VL::Editor;
using namespace VL::EditorUi;
using Kind = EditorAssetKind;
using ValueKind = EditorJsonValueKind;
using Document = EditorJsonDocumentSnapshot;
using Limits = EditorJsonAssetLimits;

TEST(EditorJsonAssetParsing, RecognizesAllFilenamePrefixesAndMixedCaseExtensions)
{
    const std::vector<std::pair<std::string, Kind>> cases{
        {"Common/MI_existing.json", Kind::MaterialInstance},
        {"Common/sM_model.JsOn", Kind::Mesh},
        {"Maps/SC_level.JSON", Kind::Scene},
        {"Maps/scene01.json", Kind::Scene},
        {"Common/T_albedo.jSoN", Kind::Texture},
        {"Maps/TR_ground.json", Kind::Terrain}
    };
    for (const auto& item : cases)
    {
        SCOPED_TRACE(item.first);
        Document document;
        ASSERT_TRUE(ParseEditorJsonAsset(item.first, "{}", document));
        EXPECT_EQ(document.kind, item.second);
        EXPECT_TRUE(document.diagnostic.empty());
    }
}

TEST(EditorJsonAssetParsing, RecognizesTopLevelTypeWithoutFilenameConvention)
{
    const std::vector<std::pair<std::string, Kind>> cases{
        {"materialInstance", Kind::MaterialInstance}, {"mesh", Kind::Mesh},
        {"scene", Kind::Scene}, {"texture", Kind::Texture}, {"terrain", Kind::Terrain},
        {"MESH", Kind::Mesh}, {"speedtree", Kind::Mesh}
    };
    for (const auto& item : cases)
    {
        Document document;
        ASSERT_TRUE(ParseEditorJsonAsset("Common/arbitrary.json",
            "{\"type\":\"" + item.first + "\"}", document));
        EXPECT_EQ(document.kind, item.second);
    }
}

TEST(EditorJsonAssetParsing, ExplicitTypeOverridesConflictingFilenameWithDiagnostic)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("SM_conflict.json", R"({"type":"texture"})", document));
    EXPECT_EQ(document.kind, Kind::Texture);
    EXPECT_NE(document.diagnostic.find("takes precedence"), std::string::npos);
}

TEST(EditorJsonAssetParsing, ExcludesUnsupportedExplicitTypesEvenWithRecognizedFilename)
{
    Document document;
    EXPECT_FALSE(ParseEditorJsonAsset("MI_shader.json", R"({"type":"material"})", document));
    EXPECT_FALSE(ParseEditorJsonAsset("profile.json", R"({"type":"offlineModelProfile"})", document));
    EXPECT_FALSE(ParseEditorJsonAsset("settings.json", R"({"windowWidth":1280})", document));
}

TEST(EditorJsonAssetParsing, NestedTypeAndParentDirectoryDoNotClassifyTheAsset)
{
    Document document;
    EXPECT_FALSE(ParseEditorJsonAsset("Maps/SC_level/settings.json",
        R"({"objects":[{"type":"mesh"}]})", document));
}

TEST(EditorJsonAssetParsing, WrongTypeFieldRetainsNamedCandidateWithDiagnostic)
{
    for (const char* type : {"17", "null", "[]", "{}"})
    {
        Document document;
        ASSERT_TRUE(ParseEditorJsonAsset("SM_wrong.json",
            std::string("{\"type\":") + type + "}", document));
        EXPECT_EQ(document.kind, Kind::Mesh);
        EXPECT_NE(document.diagnostic.find("must be a string"), std::string::npos);
    }
}

TEST(EditorJsonAssetParsing, InvalidNamedJsonRemainsVisibleWithoutPartialFields)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("MI_broken.json", R"({"name":"Broken",)", document));
    EXPECT_EQ(document.kind, Kind::MaterialInstance);
    EXPECT_EQ(document.name, "Broken");
    EXPECT_NE(document.diagnostic.find("Invalid JSON"), std::string::npos);
    EXPECT_TRUE(document.fields.empty());
}

TEST(EditorJsonAssetParsing, RecognizedTypeBeforeSyntaxErrorKeepsUnnamedCandidate)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("broken.json", R"({"type":"scene","objects":[)", document));
    EXPECT_EQ(document.kind, Kind::Scene);
    EXPECT_FALSE(document.diagnostic.empty());
    EXPECT_TRUE(document.fields.empty());
}

TEST(EditorJsonAssetParsing, InvalidUnknownJsonIsExcluded)
{
    Document document;
    EXPECT_FALSE(ParseEditorJsonAsset("settings.json", "{", document));
    EXPECT_FALSE(document.diagnostic.empty());
}

TEST(EditorJsonAssetParsing, OmitsDataAtEveryDepthAndDoesNotExposeReferencedPayloads)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("SM_mesh.json", R"({
        "type":"mesh",
        "data":{"secret":"DO_NOT_DISPLAY","nested":[1,2,3]},
        "Data":"DO_NOT_DISPLAY",
        "modelDataPath":"Data/mesh.bin",
        "children":[{"data":[{"type":"scene","secret":"DO_NOT_DISPLAY"}],"visible":true}],
        "tail":7
    })", document));
    EXPECT_EQ(document.kind, Kind::Mesh);
    bool modelReference = false;
    bool tail = false;
    for (const auto& field : document.fields)
    {
        EXPECT_NE(field.label, "data");
        EXPECT_NE(field.label, "Data");
        EXPECT_EQ(field.value.find("DO_NOT_DISPLAY"), std::string::npos);
        EXPECT_EQ(field.path.find("/data"), std::string::npos);
        if (field.label == "modelDataPath")
        {
            modelReference = true;
            EXPECT_EQ(field.value, "Data/mesh.bin");
        }
        if (field.path == "/tail") tail = true;
    }
    EXPECT_TRUE(modelReference);
    EXPECT_TRUE(tail);
}

TEST(EditorJsonAssetParsing, ValidatesSyntaxInsideOmittedDataWithoutEchoingTokens)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("T_broken.json", R"({"data":DO_NOT_DISPLAY})", document));
    EXPECT_FALSE(document.diagnostic.empty());
    EXPECT_EQ(document.diagnostic.find("DO_NOT_DISPLAY"), std::string::npos);
    EXPECT_TRUE(document.fields.empty());
}

TEST(EditorJsonAssetParsing, ProducesPreorderParentDepthPointerAndValueKinds)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("TR_ground.json",
        R"({"nested":[{"x/y~z":true},null,2.5],"name":"Ground"})", document));
    ASSERT_EQ(document.fields.size(), 7u);
    EXPECT_EQ(document.fields[0].parent, -1);
    EXPECT_EQ(document.fields[0].depth, 0u);
    EXPECT_EQ(document.fields[0].path, "");
    EXPECT_EQ(document.fields[0].label, "$");
    EXPECT_EQ(document.fields[0].kind, ValueKind::Object);
    EXPECT_EQ(document.fields[1].parent, 0);
    EXPECT_EQ(document.fields[1].kind, ValueKind::Array);
    EXPECT_EQ(document.fields[2].parent, 1);
    EXPECT_EQ(document.fields[2].path, "/nested/0");
    EXPECT_EQ(document.fields[3].parent, 2);
    EXPECT_EQ(document.fields[3].depth, 3u);
    EXPECT_EQ(document.fields[3].path, "/nested/0/x~1y~0z");
    EXPECT_EQ(document.fields[3].label, "x/y~z");
    EXPECT_EQ(document.fields[3].value, "true");
    EXPECT_EQ(document.fields[3].kind, ValueKind::Boolean);
    EXPECT_EQ(document.fields[4].kind, ValueKind::Null);
    EXPECT_EQ(document.fields[5].kind, ValueKind::Number);
    EXPECT_EQ(document.fields[5].value, "2.5");
    EXPECT_EQ(document.fields[6].kind, ValueKind::String);
    EXPECT_EQ(document.name, "Ground");
}

TEST(EditorJsonAssetParsing, CatalogModeKeepsOnlyMetadata)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("any.json",
        R"({"type":"scene","name":"Level","description":"Purpose","objects":[{}]})",
        document, false));
    EXPECT_EQ(document.name, "Level");
    EXPECT_EQ(document.description, "Purpose");
    EXPECT_TRUE(document.fields.empty());
}

TEST(EditorJsonAssetParsing, ByteLimitProducesVisibleNamedCandidate)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("T_large.json",
        std::string(Limits::MaxFileBytes + 1, ' '), document));
    EXPECT_NE(document.diagnostic.find("byte limit"), std::string::npos);
    EXPECT_TRUE(document.fields.empty());
}

TEST(EditorJsonAssetParsing, SourceLineLimitIsEnforcedBeforeParsing)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("SC_lines.json",
        std::string(Limits::MaxSourceLines, '\n') + "{}", document));
    EXPECT_NE(document.diagnostic.find("line limit"), std::string::npos);
}

TEST(EditorJsonAssetParsing, CrOnlySourceLinesCannotBypassTheLimit)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("SC_lines.json",
        std::string(Limits::MaxSourceLines, '\r') + "{}", document));
    EXPECT_NE(document.diagnostic.find("line limit"), std::string::npos);
}

TEST(EditorJsonAssetParsing, DepthLimitAlsoAppliesToOmittedData)
{
    Document document;
    const std::string text = "{\"data\":" + std::string(Limits::MaxDepth, '[') +
        "0" + std::string(Limits::MaxDepth, ']') + "}";
    ASSERT_TRUE(ParseEditorJsonAsset("MI_deep.json", text, document));
    EXPECT_NE(document.diagnostic.find("depth limit"), std::string::npos);
    EXPECT_TRUE(document.fields.empty());
}

TEST(EditorJsonAssetParsing, FieldRowsAreBoundedButRemainingInputIsValidated)
{
    std::string text = "{\"values\":[0";
    for (size_t index = 0; index < Limits::MaxFields; ++index) text += ",0";
    text += "]}";
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("SM_wide.json", text, document));
    EXPECT_EQ(document.fields.size(), Limits::MaxFields);
    EXPECT_NE(document.diagnostic.find("Field row limit"), std::string::npos);
    for (const auto& field : document.fields)
        EXPECT_LT(field.parent, static_cast<int>(document.fields.size()));
    text.pop_back();
    ASSERT_TRUE(ParseEditorJsonAsset("SM_wide.json", text, document));
    EXPECT_TRUE(document.fields.empty());
    EXPECT_NE(document.diagnostic.find("Invalid JSON"), std::string::npos);
}

TEST(EditorJsonAssetParsing, LongPointersCannotMultiplyUnboundedPerRowMemory)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("SM_key.json",
        "{\"" + std::string(Limits::MaxFieldPathBytes, 'x') + "\":[0,1]}", document));
    EXPECT_NE(document.diagnostic.find("field path byte limit"), std::string::npos);
    EXPECT_TRUE(document.fields.empty());
}

TEST(EditorJsonAssetParsing, LongScalarDisplayIsBounded)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("T_text.json",
        "{\"value\":\"" + std::string(Limits::MaxTextBytes + 20, 'x') + "\"}", document));
    ASSERT_EQ(document.fields.size(), 2u);
    EXPECT_LE(document.fields[1].value.size(), Limits::MaxTextBytes + 3);
    EXPECT_EQ(document.fields[1].value.substr(document.fields[1].value.size() - 3), "...");
}

TEST(EditorJsonAssetPaths, RejectsAbsoluteTraversalAndAlternateStreams)
{
    const std::vector<std::string> rejected{
        "../SM_escape.json", "Maps/../../SM_escape.json", "Maps\\..\\SM_escape.json",
        "/SM_absolute.json", "C:/SM_absolute.json", "C:SM_drive.json",
        "\\\\server\\share\\SM_unc.json", "Maps/SM_mesh.json:payload",
        "Common/mesh.bin", ""
    };
    for (const auto& path : rejected)
    {
        SCOPED_TRACE(path);
        std::string error;
        EXPECT_FALSE(ValidateEditorJsonAssetPath(path, error));
        EXPECT_FALSE(error.empty());
    }
    std::string error;
    EXPECT_FALSE(ValidateEditorJsonAssetPath(std::string("T_x.json\0suffix", 15), error));
}

TEST(EditorJsonAssetPaths, RejectsExcludedDirectoriesCaseInsensitively)
{
    for (const char* path : {".git/MI_x.json", "Generated/SM_x.json",
        "Maps/Data/T_x.json", "Common/data/SC_x.json", "Maps/gEnErAtEd/TR_x.json"})
    {
        std::string error;
        EXPECT_FALSE(ValidateEditorJsonAssetPath(path, error)) << path;
    }
}

TEST(EditorJsonAssetPaths, NormalizesSeparatorsWithoutShorteningRelativeAssetPath)
{
    Document document;
    ASSERT_TRUE(ParseEditorJsonAsset("Maps\\Level\\Meshes\\SM_mesh.JSON", "{}", document));
    EXPECT_EQ(document.assetPath, "Maps/Level/Meshes/SM_mesh.JSON");
}

TEST(EditorJsonAssetPaths, RejectsExcessiveDirectoryDepth)
{
    std::string path;
    for (size_t index = 0; index < Limits::MaxDepth; ++index) path += "sub/";
    std::string error;
    EXPECT_FALSE(ValidateEditorJsonAssetPath(path + "T_deep.json", error));
    EXPECT_NE(error.find("depth limit"), std::string::npos);
}

TEST(EditorJsonAssetTypes, UsesExpectedNamesAndGeneratedIconLocations)
{
    EXPECT_STREQ(EditorAssetKindName(Kind::MaterialInstance), "Material Instance");
    EXPECT_STREQ(EditorAssetKindName(Kind::Mesh), "Mesh");
    EXPECT_STREQ(EditorAssetKindName(Kind::Scene), "Scene");
    EXPECT_STREQ(EditorAssetKindName(Kind::Texture), "Texture");
    EXPECT_STREQ(EditorAssetKindName(Kind::Terrain), "Terrain");
    EXPECT_STREQ(EditorAssetKindIcon(Kind::MaterialInstance), "icons/material-instance.png");
    EXPECT_STREQ(EditorAssetKindIcon(Kind::Mesh), "icons/mesh.png");
    EXPECT_STREQ(EditorAssetKindIcon(Kind::Scene), "icons/scene.png");
    EXPECT_STREQ(EditorAssetKindIcon(Kind::Texture), "icons/texture.png");
    EXPECT_STREQ(EditorAssetKindIcon(Kind::Terrain), "icons/terrain.png");
}

// 仅在测试运行时创建自己拥有的临时根；不依赖实际资源、World、GPU 或测试执行顺序。
class JsonAssetLibraryTest : public testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<uint64_t> serial{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        sandbox = std::filesystem::temp_directory_path() /
            ("vl-json-assets-" + std::to_string(tick) + "-" + std::to_string(++serial));
        root = sandbox / "resources";
        ASSERT_TRUE(std::filesystem::create_directories(root));
    }

    void TearDown() override
    {
        library.Shutdown();
        std::error_code error;
        // 所有路径都来自本 fixture 的独占目录；不遍历或删除用户真实资源。
        std::filesystem::remove_all(sandbox, error);
    }

    void Write(const std::string& path, const std::string& text)
    {
        const auto file = root / std::filesystem::u8path(path);
        std::filesystem::create_directories(file.parent_path());
        std::ofstream output(file, std::ios::binary);
        ASSERT_TRUE(output.good());
        output << text;
        ASSERT_TRUE(output.good());
    }

    void Start()
    {
        std::string error;
        ASSERT_TRUE(library.Initialize(root, error)) << error;
    }

    bool WaitCatalog()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (library.IsCatalogLoading() && std::chrono::steady_clock::now() < deadline)
        {
            library.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !library.IsCatalogLoading() && library.GetCatalog() != nullptr;
    }

    bool WaitDocument()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (library.IsDocumentLoading() && std::chrono::steady_clock::now() < deadline)
        {
            library.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !library.IsDocumentLoading() && library.GetDocument() != nullptr;
    }

    std::filesystem::path sandbox;
    std::filesystem::path root;
    EditorJsonAssetLibrary library;
};

TEST_F(JsonAssetLibraryTest, PublishesCatalogOnlyThroughGtPoll)
{
    static_assert(std::is_same_v<decltype(library.GetCatalog()),
        std::shared_ptr<const EditorJsonAssetCatalogSnapshot>>);
    Write("Common/SM_mesh.JSON", R"({"type":"mesh"})");
    Start();
    EXPECT_TRUE(library.IsCatalogLoading());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(library.GetCatalog(), nullptr);
    EXPECT_TRUE(library.IsCatalogLoading());
    ASSERT_TRUE(WaitCatalog());
    ASSERT_EQ(library.GetCatalog()->assets.size(), 1u);
    EXPECT_EQ(library.GetCatalog()->assets[0].assetPath, "Common/SM_mesh.JSON");
    EXPECT_EQ(library.GetCatalog()->assets[0].kind, Kind::Mesh);
}

TEST_F(JsonAssetLibraryTest, PublishesDocumentOnlyThroughGtPoll)
{
    static_assert(std::is_same_v<decltype(library.GetDocument()),
        std::shared_ptr<const EditorJsonDocumentSnapshot>>);
    Write("Common/MI_existing.json", R"({"type":"materialInstance","name":"Existing"})");
    Start();
    ASSERT_TRUE(WaitCatalog());
    library.RequestOpen("Common/MI_existing.json");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(library.GetDocument(), nullptr);
    EXPECT_TRUE(library.IsDocumentLoading());
    ASSERT_TRUE(WaitDocument());
    EXPECT_EQ(library.GetDocument()->name, "Existing");
    EXPECT_FALSE(library.GetDocument()->fields.empty());
}

TEST_F(JsonAssetLibraryTest, CatalogExcludesPayloadAndUnsupportedJsonButKeepsBrokenCandidate)
{
    Write("Common/SM_mesh.json", R"({"type":"mesh","modelDataPath":"Data/missing.bin"})");
    Write("Maps/SC_level.JsOn", R"({"type":"scene"})");
    Write("Common/T_texture.JSON", R"({"type":"texture","source":"missing.png"})");
    Write("Common/MI_existing.json", R"({"type":"materialInstance"})");
    Write("Maps/TR_ground.json", R"({"type":"terrain"})");
    Write("Common/MI_broken.json", "{");
    Write("Common/profile.json", R"({"type":"offlineModelProfile"})");
    Write("Common/settings.json", "{}");
    Write("Common/data/T_payload.json", R"({"type":"texture"})");
    Write("Maps/Data/SM_payload.json", R"({"type":"mesh"})");
    Write("Generated/MI_generated.json", "{}");
    Write(".git/SC_history.json", "{}");
    Write("Common/image.png", "not an image");
    Start();
    ASSERT_TRUE(WaitCatalog());
    const auto snapshot = library.GetCatalog();
    ASSERT_EQ(snapshot->assets.size(), 6u);
    size_t broken = 0;
    for (const auto& entry : snapshot->assets)
    {
        if (entry.assetPath == "Common/MI_broken.json")
        {
            ++broken;
            EXPECT_FALSE(entry.diagnostic.empty());
        }
        else EXPECT_TRUE(entry.diagnostic.empty()) << entry.assetPath;
    }
    EXPECT_EQ(broken, 1u);
}

TEST_F(JsonAssetLibraryTest, OpeningMetadataNeverFollowsAnyResourceReferences)
{
    Write("SM_mesh.json", R"({
        "type":"mesh","modelDataPath":"../outside.bin",
        "materialSlots":[{"materialInstancePath":"C:/private/MI_x.json"}],
        "data":{"hidden":"payload"}
    })");
    Start();
    library.RequestOpen("SM_mesh.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_TRUE(library.GetDocument()->diagnostic.empty());
    EXPECT_EQ(library.GetDocument()->kind, Kind::Mesh);
    for (const auto& field : library.GetDocument()->fields)
        EXPECT_NE(field.label, "data");
}

TEST_F(JsonAssetLibraryTest, RejectsSymlinkEscapesBeforeReadingTarget)
{
    const auto outside = sandbox / "T_outside.json";
    {
        std::ofstream output(outside);
        output << R"({"type":"texture","name":"DO_NOT_READ"})";
    }
    std::error_code error;
    std::filesystem::create_symlink(outside, root / "T_alias.json", error);
    if (error) GTEST_SKIP() << "File symlinks are unavailable: " << error.message();
    Start();
    library.RequestOpen("T_alias.json");
    ASSERT_TRUE(WaitDocument());
    const auto document = library.GetDocument();
    EXPECT_NE(document->diagnostic.find("escapes"), std::string::npos);
    EXPECT_NE(document->name, "DO_NOT_READ");
    EXPECT_TRUE(document->fields.empty());
}

TEST_F(JsonAssetLibraryTest, RejectsAliasesIntoExcludedPayloadDirectories)
{
    Write("Data/T_payload.json", R"({"type":"texture","name":"DO_NOT_READ"})");
    std::error_code error;
    std::filesystem::create_symlink(root / "Data/T_payload.json", root / "T_alias.json", error);
    if (error) GTEST_SKIP() << "File symlinks are unavailable: " << error.message();
    Start();
    library.RequestOpen("T_alias.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_FALSE(library.GetDocument()->diagnostic.empty());
    EXPECT_NE(library.GetDocument()->name, "DO_NOT_READ");
    EXPECT_TRUE(library.GetDocument()->fields.empty());
}

TEST_F(JsonAssetLibraryTest, RejectsAliasesToNonJsonFilesInsideRoot)
{
    Write("Common/payload.bin", "DO_NOT_READ");
    std::error_code error;
    std::filesystem::create_symlink(root / "Common/payload.bin", root / "T_alias.json", error);
    if (error) GTEST_SKIP() << "File symlinks are unavailable: " << error.message();
    Start();
    library.RequestOpen("T_alias.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_NE(library.GetDocument()->diagnostic.find("non-JSON"), std::string::npos);
    EXPECT_TRUE(library.GetDocument()->fields.empty());
}

TEST_F(JsonAssetLibraryTest, UnsupportedOpenReturnsDiagnosticInsteadOfVisualizingSettings)
{
    Write("settings.json", R"({"windowWidth":1280})");
    Start();
    library.RequestOpen("settings.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_FALSE(library.GetDocument()->diagnostic.empty());
    EXPECT_TRUE(library.GetDocument()->fields.empty());
}

TEST_F(JsonAssetLibraryTest, DirectorySymlinksDoNotRecurseOrLoop)
{
    Write("Common/SM_mesh.json", "{}");
    std::error_code error;
    std::filesystem::create_directory_symlink(root, root / "Common/loop", error);
    if (error) GTEST_SKIP() << "Directory symlinks are unavailable: " << error.message();
    Start();
    ASSERT_TRUE(WaitCatalog());
    ASSERT_EQ(library.GetCatalog()->assets.size(), 1u);
}

TEST_F(JsonAssetLibraryTest, LatestOpenWinsAcrossWorkerAndReadyMailboxResults)
{
    Write("SM_slow.json", "{\"type\":\"mesh\",\"data\":\"" +
        std::string(Limits::MaxFileBytes - 100, 'x') + "\"}");
    Write("T_latest.json", R"({"type":"texture","name":"Latest"})");
    Start();
    ASSERT_TRUE(WaitCatalog());
    for (int index = 0; index < 64; ++index)
    {
        library.RequestOpen("SM_slow.json");
        library.RequestOpen("T_latest.json");
    }
    ASSERT_TRUE(WaitDocument());
    EXPECT_EQ(library.GetDocument()->assetPath, "T_latest.json");
    EXPECT_EQ(library.GetDocument()->name, "Latest");
    const auto latest = library.GetDocument();
    for (int index = 0; index < 100; ++index)
    {
        library.Poll();
        EXPECT_EQ(library.GetDocument(), latest);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

TEST_F(JsonAssetLibraryTest, RejectedPathSupersedesAnyInflightOpenWithoutSynchronousIo)
{
    Write("SM_slow.json", "{\"data\":\"" + std::string(Limits::MaxFileBytes - 100, 'x') + "\"}");
    Start();
    ASSERT_TRUE(WaitCatalog());
    library.RequestOpen("SM_slow.json");
    library.RequestOpen("../T_escape.json");
    ASSERT_TRUE(WaitDocument());
    const auto rejected = library.GetDocument();
    EXPECT_EQ(rejected->assetPath, "../T_escape.json");
    EXPECT_FALSE(rejected->diagnostic.empty());
    for (int index = 0; index < 100; ++index)
    {
        library.Poll();
        EXPECT_EQ(library.GetDocument(), rejected);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

TEST_F(JsonAssetLibraryTest, RepeatedPendingOpenIsCoalesced)
{
    Write("T_one.json", R"({"type":"texture"})");
    Start();
    ASSERT_TRUE(WaitCatalog());
    library.RequestOpen("T_one.json");
    for (int index = 0; index < 100; ++index) library.RequestOpen("T_one.json");
    ASSERT_TRUE(WaitDocument());
    const auto firstRevision = library.GetDocument()->revision;
    library.RequestOpen("T_one.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_EQ(library.GetDocument()->revision, firstRevision + 1);
}

TEST_F(JsonAssetLibraryTest, SupersededReadyRejectionCannotPublishOverLatestRequest)
{
    Write("T_before.json", R"({"type":"texture","name":"Before"})");
    Write("T_after.json", R"({"type":"texture","name":"After"})");
    Start();
    library.RequestOpen("T_before.json");
    ASSERT_TRUE(WaitDocument());
    library.RequestOpen("../T_escape.json");
    // 非法路径的完成邮箱同步生成但尚未 Poll；新请求必须让该结果立刻失效。
    library.RequestOpen("T_after.json");
    library.Poll();
    ASSERT_NE(library.GetDocument(), nullptr);
    EXPECT_NE(library.GetDocument()->assetPath, "../T_escape.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_EQ(library.GetDocument()->name, "After");
}

TEST_F(JsonAssetLibraryTest, IdlePollingDoesNotRescanOrReopenFiles)
{
    Write("T_one.json", R"({"type":"texture","name":"Original"})");
    Start();
    ASSERT_TRUE(WaitCatalog());
    library.RequestOpen("T_one.json");
    ASSERT_TRUE(WaitDocument());
    const auto catalog = library.GetCatalog();
    const auto document = library.GetDocument();
    ASSERT_TRUE(std::filesystem::remove(root / "T_one.json"));
    for (int index = 0; index < 1000; ++index)
    {
        library.Poll();
        EXPECT_EQ(library.GetCatalog(), catalog);
        EXPECT_EQ(library.GetDocument(), document);
        EXPECT_FALSE(library.IsCatalogLoading());
        EXPECT_FALSE(library.IsDocumentLoading());
    }
    library.RequestRefresh();
    ASSERT_TRUE(WaitCatalog());
    EXPECT_TRUE(library.GetCatalog()->assets.empty());
    EXPECT_GT(library.GetCatalog()->revision, catalog->revision);
    EXPECT_EQ(catalog->assets.size(), 1u);
    EXPECT_EQ(document->name, "Original");
}

TEST_F(JsonAssetLibraryTest, ExplicitRefreshPublishesNewStateWithoutMutatingOldSnapshots)
{
    Write("T_one.json", R"({"type":"texture","name":"Before"})");
    Start();
    ASSERT_TRUE(WaitCatalog());
    const auto before = library.GetCatalog();
    Write("T_one.json", R"({"type":"texture","name":"After"})");
    library.RequestRefresh();
    for (int index = 0; index < 100; ++index) library.RequestRefresh();
    EXPECT_EQ(library.GetCatalog(), before);
    ASSERT_TRUE(WaitCatalog());
    ASSERT_EQ(library.GetCatalog()->assets.size(), 1u);
    EXPECT_EQ(library.GetCatalog()->assets[0].name, "After");
    EXPECT_EQ(before->assets[0].name, "Before");
    EXPECT_GT(library.GetCatalog()->revision, before->revision);
    const auto after = library.GetCatalog();
    for (int index = 0; index < 1000; ++index) library.Poll();
    EXPECT_EQ(library.GetCatalog(), after);
}

TEST_F(JsonAssetLibraryTest, ShutdownDropsPendingResultsAndAllowsCleanReinitialize)
{
    Write("T_old.json", R"({"type":"texture","name":"Old"})");
    Start();
    ASSERT_TRUE(WaitCatalog());
    const auto retained = library.GetCatalog();
    library.RequestOpen("T_old.json");
    library.Shutdown();
    library.Shutdown();
    EXPECT_EQ(library.GetCatalog(), nullptr);
    EXPECT_EQ(library.GetDocument(), nullptr);
    EXPECT_FALSE(library.IsCatalogLoading());
    EXPECT_FALSE(library.IsDocumentLoading());
    const auto secondRoot = sandbox / "second";
    ASSERT_TRUE(std::filesystem::create_directories(secondRoot));
    std::string error;
    ASSERT_TRUE(library.Initialize(secondRoot, error)) << error;
    ASSERT_TRUE(WaitCatalog());
    EXPECT_TRUE(library.GetCatalog()->assets.empty());
    EXPECT_EQ(library.GetDocument(), nullptr);
    EXPECT_EQ(retained->assets[0].name, "Old");
}

TEST_F(JsonAssetLibraryTest, InvalidRootDoesNotStartWorker)
{
    std::string error;
    EXPECT_FALSE(library.Initialize(root / "missing", error));
    EXPECT_FALSE(error.empty());
    library.RequestRefresh();
    library.RequestOpen("T_one.json");
    library.Poll();
    EXPECT_EQ(library.GetCatalog(), nullptr);
    EXPECT_EQ(library.GetDocument(), nullptr);
    EXPECT_FALSE(library.IsCatalogLoading());
}

} // namespace
