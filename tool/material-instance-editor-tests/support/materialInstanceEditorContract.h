#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "material/materialDescriptorSchema.h"
#include "shader/build/contentHash.h"

namespace material_instance_editor_test
{

using Json = nlohmann::json;

struct CommandEnvelope
{
    uint32_t protocolVersion = 0;
    uint64_t commandId = 0;
    std::optional<uint64_t> correlationId;
    std::string source;
    std::string command;
    std::optional<uint64_t> expectedDocumentRevision;
    Json payload = Json::object();
};

struct CommandResult
{
    uint32_t protocolVersion = 0;
    uint64_t commandId = 0;
    std::string status;
    std::string errorCode;
    std::string message;
    std::optional<uint64_t> documentRevision;
    Json payload = Json::object();
};

struct NavigationReference
{
    std::string scenePath;
    std::string objectId;
    std::string objectType;
    std::string assetPath;
    std::string selector;
    std::string materialInstancePath;
};

enum class ClosePolicy
{
    Cancel,
    Discard
};

std::string NormalizeRelativeAssetPath(std::string_view path);

CommandEnvelope DecodeCommandEnvelope(
    const Json& wire,
    const Json& schema);

Json EncodeCommandEnvelope(const CommandEnvelope& envelope);

CommandResult DecodeCommandResult(
    const Json& wire,
    const Json& schema);

NavigationReference ResolveNavigationReference(
    const Json& scene,
    std::string_view objectId,
    std::string_view selector);

Json MergeEffectiveValues(
    const Json& defaults,
    const Json& overrides);

Json BuildSparseCandidate(
    const Json& originalMaterialInstance,
    const Json& materialDefinition,
    const Json& workingParameters,
    const Json& workingTextures);

void ValidateTextureAssetDocument(
    const std::filesystem::path& fixtureRoot,
    std::string_view textureAssetPath);

struct CommitResult
{
    bool succeeded = false;
    std::string errorCode;
};

CommitResult TryCommitCandidate(
    const std::filesystem::path& materialInstancePath,
    const VL::ContentDigest& expectedSourceDigest,
    const Json& candidate);

class DocumentContractRegistry
{
public:
    struct Snapshot
    {
        std::string normalizedPath;
        uint64_t revision = 0;
        bool dirty = false;
        bool previewConnected = false;
        std::string state;
        std::string schemaDigest;
        size_t originCount = 0;
    };

    void Open(
        std::string_view materialInstancePath,
        std::string_view navigationOrigin,
        std::string_view schemaDigest,
        const Json& baseline);

    bool Contains(std::string_view materialInstancePath) const;

    bool SetWorking(
        std::string_view materialInstancePath,
        uint64_t expectedRevision,
        const Json& working);

    bool Revert(
        std::string_view materialInstancePath,
        uint64_t expectedRevision);

    bool Reload(
        std::string_view materialInstancePath,
        uint64_t expectedRevision,
        std::string_view schemaDigest,
        const Json& baseline);

    bool Save(
        std::string_view materialInstancePath,
        uint64_t expectedRevision,
        std::string_view newSchemaDigest);

    bool Close(
        std::string_view materialInstancePath,
        ClosePolicy policy);

    void ConnectPreview(std::string_view materialInstancePath);
    void WorldChanged(std::string_view worldIdentity);
    void MarkSchemaChanged(
        std::string_view materialInstancePath,
        std::string_view schemaDigest);

    Snapshot GetSnapshot(std::string_view materialInstancePath) const;

private:
    struct Document
    {
        std::string normalizedPath;
        Json baseline = Json::object();
        Json working = Json::object();
        uint64_t revision = 1;
        bool dirty = false;
        bool previewConnected = false;
        std::string state = "clean";
        std::string schemaDigest;
        std::set<std::string> origins;
    };

    std::map<std::string, Document> documents;
    std::string activeWorldIdentity;
};

} // namespace material_instance_editor_test
