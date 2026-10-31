#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
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

TEST(EditorJsonAssetParsing, TressfxSkinningIsSummarizedForTheHairViewportGuard)
{
    // 毛发视口靠这两个摘要决定“是不是毛发、要不要挡预览”，不在 UI 里读文件。
    Document skinnedHair;
    ASSERT_TRUE(ParseEditorJsonAsset("Maps/SC_x/Meshes/SM_hair.json",
        R"({"type":"tressfx","modelDataPath":"Maps/SC_x/Source/hair.tfx","strandRadius":0.012,)"
        R"("simulation":{"backend":"gpu","skinning":{"source":"FemaleCharacter","rootBone":"Head_M"}}})",
        skinnedHair));
    EXPECT_EQ(skinnedHair.type, "tressfx");
    EXPECT_EQ(skinnedHair.kind, Kind::Tressfx);
    EXPECT_TRUE(skinnedHair.tressfxSkinning);

    Document staticHair;
    ASSERT_TRUE(ParseEditorJsonAsset("Maps/SC_x/Meshes/SM_hair_static.json",
        R"({"type":"tressfx","modelDataPath":"Maps/SC_x/Source/hair.tfx","strandRadius":0.012})",
        staticHair));
    EXPECT_EQ(staticHair.type, "tressfx");
    EXPECT_FALSE(staticHair.tressfxSkinning);

    // 顶层 skinning（骨骼网格自己的字段）和别处的同名键都不能冒充 tressfx 绑定。
    Document skeletalMesh;
    ASSERT_TRUE(ParseEditorJsonAsset("Maps/SC_x/Meshes/SM_body.json",
        R"({"type":"mesh","skinning":{"clips":[]}})", skeletalMesh));
    EXPECT_FALSE(skeletalMesh.tressfxSkinning);

    Document nestedElsewhere;
    ASSERT_TRUE(ParseEditorJsonAsset("Maps/SC_x/Meshes/SM_other.json",
        R"({"type":"tressfx","collision":{"simulation":{"skinning":{}}}})", nestedElsewhere));
    EXPECT_FALSE(nestedElsewhere.tressfxSkinning);
}

TEST(EditorJsonAssetParsing, TressfxSkinningSurvivesJSONSyntaxErrorsAsACandidate)
{
    // 损坏的候选仍要能按类型呈现；标记过宽只会让视口挡住预览，不会加载错误资产。
    Document document;
    ParseEditorJsonAsset("SM_broken.json",
        R"({"type":"tressfx","simulation":{"skinning":{})", document);
    EXPECT_EQ(document.type, "tressfx");
    EXPECT_TRUE(document.tressfxSkinning);
    EXPECT_FALSE(document.diagnostic.empty());
}

TEST(EditorJsonAssetParsing, RecognizesAllFilenamePrefixesAndMixedCaseExtensions)
{
    const std::vector<std::pair<std::string, Kind>> cases{
        {"Common/MI_existing.json", Kind::MaterialInstance},
        {"Common/sM_model.JsOn", Kind::Mesh},
        {"Common/tF_guides.JsOn", Kind::Tressfx},
        {"Maps/SC_level.JSON", Kind::Scene},
        {"Maps/scene01.json", Kind::Scene},
        {"Common/T_albedo.jSoN", Kind::Texture},
        {"Maps/TR_ground.json", Kind::Terrain},
        {"Common/Blueprints/BP_rotator.json", Kind::Blueprint},
        {"Common/Blueprints/bP_rotator.JsOn", Kind::Blueprint},
        {"Maps/Retarget/RT_profile.json", Kind::Retarget}
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
        {"MESH", Kind::Mesh}, {"speedtree", Kind::Mesh}, {"tressfx", Kind::Tressfx}, {"blueprint", Kind::Blueprint},
        {"BLUEPRINT", Kind::Blueprint}
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
    EXPECT_STREQ(EditorAssetKindName(Kind::Retarget), "Retarget");
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

TEST_F(JsonAssetLibraryTest, RetargetPreviewRejectsOutsideRootReferences)
{
    Write("RT_test.json", R"({"format":"vl.retarget.v1","preview":{
        "sourceSkeletonPath":"../SK_outside.json","targetSkeletonPath":"SK_inside.json"}})");
    Start();
    library.RequestOpen("RT_test.json");
    ASSERT_TRUE(WaitDocument());
    const auto document = library.GetDocument();
    EXPECT_EQ(document->kind, Kind::Retarget);
    EXPECT_EQ(document->retargetPreview, nullptr);
    EXPECT_NE(document->previewDiagnostic.find("Parent traversal"), std::string::npos);
    EXPECT_TRUE(document->diagnostic.empty());
}

TEST_F(JsonAssetLibraryTest, RetargetPreviewMissingMetadataDoesNotRetainPreviousValues)
{
    Write("RT_test.json", R"({"format":"vl.retarget.v1"})");
    Start();
    library.RequestOpen("RT_test.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_EQ(library.GetDocument()->retargetPreview, nullptr);
    EXPECT_NE(library.GetDocument()->previewDiagnostic.find("requires preview"), std::string::npos);
    EXPECT_FALSE(library.GetDocument()->fields.empty());
}

TEST_F(JsonAssetLibraryTest, RetargetPreviewPublishesFrozenValuesAndClearsOnFailedReload)
{
    const std::string skeleton = R"({"format":"vl.skeleton.v1","skeleton":"Shared",
        "root":"root","slots":["DefaultSlot"],"joints":[{"name":"root","parent":-1,
        "restLocal":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}]})";
    const std::string profile = R"({"format":"vl.retarget.v1","sourceSkeleton":"Shared",
        "targetSkeleton":"Shared","sourceRoot":"root","targetRoot":"root",
        "joints":[{"source":"root","target":"root"}],
        "preview":{"sourceSkeletonPath":"SK_source.json","targetSkeletonPath":"SK_target.json",
        "targetModelPath":"SM_target.json"}})";
    Write("SK_source.json", skeleton);
    Write("SK_target.json", skeleton);
    Write("RT_test.json", profile);
    Write("SM_target.json", R"({"type":"mesh"})");
    Start();
    library.RequestOpen("RT_test.json");
    ASSERT_TRUE(WaitDocument());
    const auto frozen = library.GetDocument()->retargetPreview;
    ASSERT_NE(frozen, nullptr) << library.GetDocument()->previewDiagnostic;
    EXPECT_EQ(frozen->sourceSkeleton, "Shared");
    EXPECT_EQ(frozen->targetModelPath, "SM_target.json");
    EXPECT_EQ(frozen->mappings.size(), 1u);
    Write("SK_target.json", "{}");
    library.RequestOpen("RT_test.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_EQ(library.GetDocument()->retargetPreview, nullptr);
    EXPECT_FALSE(library.GetDocument()->previewDiagnostic.empty());
    EXPECT_EQ(frozen->target[0].name, "root");
}

TEST_F(JsonAssetLibraryTest, TexturePreviewDecodesRgbWithTopLeftOrientation)
{
    std::string tga(18, '\0');
    tga[2] = 2; tga[12] = 2; tga[14] = 1; tga[16] = 32; tga[17] = 40;
    for (unsigned char byte : {0, 0, 255, 0, 0, 128, 0, 0}) tga += char(byte);
    Write("source.tga", tga);
    Write("T_test.json", R"({"type":"texture","source":"source.tga","colorSpace":"srgb"})");
    Start();
    library.RequestOpen("T_test.json");
    ASSERT_TRUE(WaitDocument());
    const auto doc = library.GetDocument();
    ASSERT_TRUE(doc->texturePreview) << doc->previewDiagnostic;
    EXPECT_EQ(doc->texturePreview->width, 2u);
    EXPECT_EQ(doc->texturePreview->height, 1u);
    EXPECT_EQ(doc->texturePreview->pixels, (std::vector<uint8_t>{255, 0, 0, 255, 0, 128, 0, 255}));
}

TEST_F(JsonAssetLibraryTest, LinearTexturePreviewEncodesDisplayRgb)
{
    std::string tga(18, '\0');
    tga[2] = 2; tga[12] = 1; tga[14] = 1; tga[16] = 24; tga[17] = 32;
    tga.append(3, char(128));
    Write("source.tga", tga);
    Write("T_test.json", R"({"type":"texture","source":"source.tga","colorSpace":"linear"})");
    Start();
    library.RequestOpen("T_test.json");
    ASSERT_TRUE(WaitDocument());
    ASSERT_TRUE(library.GetDocument()->texturePreview);
    EXPECT_NEAR(library.GetDocument()->texturePreview->pixels[0], 188, 1);
}

TEST_F(JsonAssetLibraryTest, TexturePreviewRejectsOutsideRoot)
{
    Write("T_test.json", R"({"type":"texture","source":"../outside.tga","colorSpace":"srgb"})");
    Start();
    library.RequestOpen("T_test.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_FALSE(library.GetDocument()->texturePreview);
    EXPECT_NE(library.GetDocument()->previewDiagnostic.find("escapes"), std::string::npos);
}

TEST_F(JsonAssetLibraryTest, TexturePreviewIsBoundedAndReloadDoesNotKeepOldPixels)
{
    std::string tga(18, '\0');
    tga[2] = 2; tga[13] = 8; tga[14] = 1; tga[16] = 24; tga[17] = 32;
    tga.append(2048 * 3, char(128));
    Write("source.tga", tga);
    Write("T_test.json", R"({"type":"texture","source":"source.tga","colorSpace":"srgb"})");
    Start();
    library.RequestOpen("T_test.json");
    ASSERT_TRUE(WaitDocument());
    ASSERT_TRUE(library.GetDocument()->texturePreview);
    EXPECT_EQ(library.GetDocument()->texturePreview->sourceWidth, 2048u);
    EXPECT_EQ(library.GetDocument()->texturePreview->width, 1024u);
    const auto revision = library.GetDocument()->revision;
    Write("source.tga", "corrupt");
    library.RequestOpen("T_test.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_GT(library.GetDocument()->revision, revision);
    EXPECT_FALSE(library.GetDocument()->texturePreview);
    EXPECT_FALSE(library.GetDocument()->previewDiagnostic.empty());
}

TEST_F(JsonAssetLibraryTest, TexturePreviewReportsMissingSource)
{
    Write("T_test.json", R"({"type":"texture","source":"missing.png","colorSpace":"srgb"})");
    Start();
    library.RequestOpen("T_test.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_FALSE(library.GetDocument()->texturePreview);
    EXPECT_NE(library.GetDocument()->previewDiagnostic.find("missing"), std::string::npos);
}

TEST_F(JsonAssetLibraryTest, TexturePreviewReportsUnsupportedFormat)
{
    Write("source.dds", "not decoded");
    Write("T_test.json", R"({"type":"texture","source":"source.dds","colorSpace":"linear"})");
    Start();
    library.RequestOpen("T_test.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_FALSE(library.GetDocument()->texturePreview);
    EXPECT_NE(library.GetDocument()->previewDiagnostic.find("unsupported format"), std::string::npos);
}

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

TEST_F(JsonAssetLibraryTest, TressfxGuideCountIsReadFromTheTfxHeader)
{
    // Groups 编辑器需要 guideCount 才能在保存前拦截 end > guideCount；
    // 目录扫描与显式打开都必须带上同一份头信息。
    // 12 字节是 editor worker 读取的最小头：version、guideCount、pointsPerStrand。
    std::string tfx(12, '\0');
    const float version = 4.0f;
    std::memcpy(&tfx[0], &version, sizeof(version));
    const uint32_t guideCount = 40334;
    const uint32_t pointsPerStrand = 16;
    std::memcpy(&tfx[8], &pointsPerStrand, sizeof(pointsPerStrand));
    tfx[4] = static_cast<char>(guideCount & 0xff);
    tfx[5] = static_cast<char>((guideCount >> 8) & 0xff);
    tfx[6] = static_cast<char>((guideCount >> 16) & 0xff);
    tfx[7] = static_cast<char>((guideCount >> 24) & 0xff);
    Write("Maps/SC_x/Source/hair.tfx", tfx);
    Write("Maps/SC_x/Meshes/TF_hair.json",
        R"({"type":"tressfx","modelDataPath":"Maps/SC_x/Source/hair.tfx","strandRadius":0.05})");
    Start();
    ASSERT_TRUE(WaitCatalog());
    ASSERT_EQ(library.GetCatalog()->assets.size(), 1u);
    EXPECT_EQ(library.GetCatalog()->assets[0].tressfxGuideCount, guideCount);

    library.RequestOpen("Maps/SC_x/Meshes/TF_hair.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_EQ(library.GetDocument()->tressfxGuideCount, guideCount);
    EXPECT_TRUE(library.GetDocument()->diagnostic.empty());
}

TEST_F(JsonAssetLibraryTest, TressfxGuideCountStaysUnknownOnMissingOrShortSource)
{
    // 头不足 12 字节时不能越界读；任何失败都只把 guideCount 留 0。
    Write("Maps/SC_y/Source/short.tfx", std::string(5, '\0'));
    Write("Maps/SC_y/Meshes/TF_short.json",
        R"({"type":"tressfx","modelDataPath":"Maps/SC_y/Source/short.tfx","strandRadius":0.05})");
    Write("Maps/SC_y/Meshes/TF_missing.json",
        R"({"type":"tressfx","modelDataPath":"Maps/SC_y/Source/missing.tfx","strandRadius":0.05})");
    // 未拉取的 Git LFS 指针或任意文本都不是合法 TFX 头，不能把 ASCII 字节拼成伪 count。
    Write("Maps/SC_y/Source/lfs.tfx", "version https://git-lfs.github.com/spec/v1\n");
    Write("Maps/SC_y/Meshes/TF_lfs.json",
        R"({"type":"tressfx","modelDataPath":"Maps/SC_y/Source/lfs.tfx","strandRadius":0.05})");
    Start();
    ASSERT_TRUE(WaitCatalog());
    for (const auto& entry : library.GetCatalog()->assets)
        EXPECT_EQ(entry.tressfxGuideCount, 0u) << entry.assetPath;

    library.RequestOpen("Maps/SC_y/Meshes/TF_missing.json");
    ASSERT_TRUE(WaitDocument());
    EXPECT_EQ(library.GetDocument()->tressfxGuideCount, 0u);
    EXPECT_TRUE(library.GetDocument()->diagnostic.empty());
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

// TressFX 的 .tfx 头读取是显式例外；其他资产的引用路径仍只作为元数据。
TEST_F(JsonAssetLibraryTest, OpeningMetadataDoesNotFollowNonTressfxReferences)
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
