#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "editor/command/editorCommand.h"
#include "editor/command/editorCommandJsonCodec.h"
#include "editor/service/assetDocumentService.h"

namespace
{

using Json = nlohmann::json;
using namespace VL;
using namespace VL::Editor;

Json ParticleDocument()
{
    return Json{
        {"name", "PS_history"},
        {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{{"name", "fire"}, {"maxParticles", 128},
                {"spawnRate", 120.0}},
            Json{{"name", "smoke"}, {"maxParticles", 64},
                {"spawnRate", 30.0}}
        })}
    };
}

const EditorAssetDocumentResultPayload& Payload(
    const EditorCommandResult& result)
{
    return std::get<EditorAssetDocumentResultPayload>(result.payload);
}

class ParticleDocumentHistoryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto stamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
            ("vl-particle-history-" + std::to_string(stamp));
        std::filesystem::create_directories(root / "Maps");
        baseline = ParticleDocument();
        Write(baseline);
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void Write(const Json& value)
    {
        std::ofstream file(root / path, std::ios::binary);
        file << value.dump(4);
    }

    EditorCommandResult Execute(
        AssetDocumentService& service,
        EditorCommandType type,
        std::optional<uint64_t> revision = std::nullopt,
        EditorCommandPayload payload = {})
    {
        EditorCommandEnvelope command;
        command.commandId = nextCommandId++;
        command.source = EditorCommandSource::RuntimeTest;
        command.type = type;
        command.expectedDocumentRevision = revision;
        if (std::holds_alternative<EditorNoPayload>(payload))
            payload = MaterialInstanceAssetPathPayload{path};
        command.payload = std::move(payload);
        return service.Execute(command);
    }

    EditorCommandResult Modify(
        AssetDocumentService& service, uint64_t revision,
        std::vector<EditorAssetFieldChange> changes)
    {
        return Execute(service, EditorCommandType::ModifyParticleDocument,
            revision, ModifyAssetDocumentPayload{path, std::move(changes)});
    }

    EditorAssetDocumentResultPayload Snapshot(
        const AssetDocumentService& service) const
    {
        return service.DocumentSnapshots().front();
    }

    std::filesystem::path root;
    std::string path = "Maps/PS_history.json";
    Json baseline;
    uint64_t nextCommandId = 1;
};

} // namespace

TEST_F(ParticleDocumentHistoryTest, UndoRedoRestoresFieldsAndEmitterStructure)
{
    AssetDocumentService service(
        root, EditorAssetDocumentKind::ParticleSystem);
    auto opened = Execute(
        service, EditorCommandType::GetParticleDocument);
    ASSERT_EQ(opened.status, EditorCommandStatus::Succeeded)
        << opened.message;
    EXPECT_EQ(opened.documentRevision, 1u);
    EXPECT_FALSE(Payload(opened).canUndo);
    EXPECT_FALSE(Payload(opened).canRedo);

    EditorAssetFieldChange rate;
    rate.field = "emitters.0.spawnRate";
    rate.value = 200.0;
    auto modified = Modify(service, 1, {rate});
    ASSERT_EQ(modified.status, EditorCommandStatus::Succeeded)
        << modified.message;
    EXPECT_EQ(modified.documentRevision, 2u);
    EXPECT_TRUE(Payload(modified).canUndo);
    EXPECT_DOUBLE_EQ(Payload(modified).document["emitters"][0]
        ["spawnRate"].get<double>(), 200.0);

    Json emitters = baseline["emitters"];
    Json copy = emitters[0];
    copy["name"] = "fire_copy";
    emitters.push_back(std::move(copy));
    EditorAssetFieldChange structure;
    structure.field = "emitters";
    structure.value = emitters;
    auto added = Modify(service, 2, {structure});
    ASSERT_EQ(added.status, EditorCommandStatus::Succeeded)
        << added.message;
    EXPECT_EQ(added.documentRevision, 3u);
    EXPECT_EQ(Payload(added).document["emitters"].size(), 3u);

    auto undoStructure = Execute(
        service, EditorCommandType::UndoParticleDocument, 3);
    ASSERT_EQ(undoStructure.status, EditorCommandStatus::Succeeded)
        << undoStructure.message;
    EXPECT_EQ(undoStructure.documentRevision, 4u);
    EXPECT_EQ(Payload(undoStructure).document["emitters"].size(), 2u);
    EXPECT_DOUBLE_EQ(Payload(undoStructure).document["emitters"][0]
        ["spawnRate"].get<double>(), 200.0);
    EXPECT_TRUE(Payload(undoStructure).canRedo);

    auto undoField = Execute(
        service, EditorCommandType::UndoParticleDocument, 4);
    ASSERT_EQ(undoField.status, EditorCommandStatus::Succeeded)
        << undoField.message;
    EXPECT_EQ(undoField.documentRevision, 5u);
    EXPECT_FALSE(Payload(undoField).dirty);
    EXPECT_FALSE(Payload(undoField).canUndo);
    EXPECT_TRUE(Payload(undoField).canRedo);
    EXPECT_DOUBLE_EQ(Payload(undoField).document["emitters"][0]
        ["spawnRate"].get<double>(), 120.0);

    auto redoField = Execute(
        service, EditorCommandType::RedoParticleDocument, 5);
    ASSERT_EQ(redoField.status, EditorCommandStatus::Succeeded)
        << redoField.message;
    EXPECT_TRUE(Payload(redoField).dirty);
    EXPECT_DOUBLE_EQ(Payload(redoField).document["emitters"][0]
        ["spawnRate"].get<double>(), 200.0);
    auto redoStructure = Execute(
        service, EditorCommandType::RedoParticleDocument, 6);
    ASSERT_EQ(redoStructure.status, EditorCommandStatus::Succeeded)
        << redoStructure.message;
    EXPECT_EQ(Payload(redoStructure).document["emitters"].size(), 3u);

    auto reverted = Execute(
        service, EditorCommandType::RevertParticleDocument, 7);
    ASSERT_EQ(reverted.status, EditorCommandStatus::Succeeded)
        << reverted.message;
    EXPECT_EQ(reverted.documentRevision, 8u);
    EXPECT_FALSE(Payload(reverted).dirty);
    EXPECT_FALSE(Payload(reverted).canUndo);
    EXPECT_FALSE(Payload(reverted).canRedo);
    EXPECT_EQ(Payload(reverted).document["emitters"].size(), 2u);
    EXPECT_DOUBLE_EQ(Payload(reverted).document["emitters"][0]
        ["spawnRate"].get<double>(), 120.0);
}

TEST_F(ParticleDocumentHistoryTest, HistoryKeepsAtMostThirtyTwoSteps)
{
    AssetDocumentService service(
        root, EditorAssetDocumentKind::ParticleSystem);
    ASSERT_EQ(Execute(service,
        EditorCommandType::GetParticleDocument).status,
        EditorCommandStatus::Succeeded);
    uint64_t revision = 1;
    for (int index = 1; index <= 33; ++index)
    {
        EditorAssetFieldChange change;
        change.field = "emitters.0.spawnRate";
        change.value = 120.0 + index;
        const auto result = Modify(service, revision++, {change});
        ASSERT_EQ(result.status, EditorCommandStatus::Succeeded)
            << result.message;
    }
    for (int index = 0; index < 32; ++index)
        ASSERT_EQ(Execute(service,
            EditorCommandType::UndoParticleDocument,
            revision++).status, EditorCommandStatus::Succeeded);

    const auto oldest = Snapshot(service);
    EXPECT_FALSE(oldest.canUndo);
    EXPECT_TRUE(oldest.canRedo);
    EXPECT_DOUBLE_EQ(oldest.document["emitters"][0]
        ["spawnRate"].get<double>(), 121.0);

    const auto rejected = Execute(service,
        EditorCommandType::UndoParticleDocument, revision);
    EXPECT_EQ(rejected.status, EditorCommandStatus::Rejected);
    EXPECT_EQ(rejected.documentRevision, revision);
    EXPECT_TRUE(Payload(rejected).canRedo);
}

TEST(ParticleHistoryProtocol, UndoRedoRoundTripAsPathOnlyCommands)
{
    for (const auto& testCase : std::vector<std::pair<
             const char*, EditorAssetOperation>>{
             {"particle.undo", EditorAssetOperation::Undo},
             {"particle.redo", EditorAssetOperation::Redo}})
    {
        const Json wire{
            {"protocolVersion", 1},
            {"commandId", 7},
            {"source", "ai"},
            {"command", testCase.first},
            {"expectedDocumentRevision", 4},
            {"payload", {{"assetPath", "Maps/PS_history.json"}}}
        };
        const EditorCommandEnvelope decoded =
            EditorCommandJsonCodec::DecodeCommand(wire);
        EXPECT_EQ(GetEditorAssetDocumentKind(decoded.type),
            EditorAssetDocumentKind::ParticleSystem);
        EXPECT_EQ(GetEditorAssetOperation(decoded.type), testCase.second);
        EXPECT_EQ(EditorCommandJsonCodec::Encode(decoded), wire);
    }
}
