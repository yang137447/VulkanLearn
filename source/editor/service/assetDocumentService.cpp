#include "editor/service/assetDocumentService.h"

#include <fstream>
#include <cctype>
#include "editor/command/strictJson.h"
#include "mesh/validation/meshAssetValidator.h"
#include "resource/image/textureAssetValidation.h"
#include "scene/validation/sceneJsonValidator.h"
#include "script/scriptBlueprintAsset.h"
#include "script/scriptPropertySchema.h"

namespace VL::Editor
{
namespace
{
using Json = nlohmann::json;
using K = EditorAssetDocumentKind;
using O = EditorAssetOperation;

std::string PortablePath(std::string path)
{
    for (char& character : path) if (character == '\\') character = '/';
    return path;
}

void ValidateRelativePath(const std::string& path)
{
    const std::filesystem::path parsed(PortablePath(path));
    if (path.empty() || path.find('\0') != std::string::npos || parsed.is_absolute() ||
        parsed.has_root_name() || parsed.has_root_directory())
        throw std::invalid_argument("Asset path must be resource-root-relative");
    for (const auto& part : parsed)
        if (part == "..") throw std::invalid_argument("Asset path must not contain traversal");
}

Json ReadJson(const std::filesystem::path& path)
{
    // 标准接口不把 inspector 的截断文本当作者数据；输入预算限制解析内存。
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 4 * 1024 * 1024)
        throw std::runtime_error("Asset must be a regular JSON file of at most 4 MiB");
    std::ifstream file(path, std::ios::binary);
    std::string text;
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount())
    {
        text.append(buffer, static_cast<size_t>(file.gcount()));
        if (text.size() > 4 * 1024 * 1024) throw std::invalid_argument("Asset exceeds 4 MiB");
    }
    if (file.bad()) throw std::runtime_error("Failed to read asset JSON");
    return ParseStrictJson(text, 4 * 1024 * 1024);
}

void ValidateKind(K kind, const Json& json, const std::string& path)
{
    if (!json.is_object()) throw std::invalid_argument("Asset must be a JSON object");
    const std::string type = json.value("type", "");
    if ((kind == K::Scene && type != "scene") ||
        (kind == K::Mesh && type != "mesh" && type != "speedtree") ||
        (kind == K::Tressfx && type != "tressfx") ||
        (kind == K::Texture && type != "texture") ||
        (kind == K::Blueprint && type != "blueprint"))
        throw std::invalid_argument("Wrong asset type: " + path);
    const auto filename = std::filesystem::path(path).filename().string();
    std::string foldedFilename = filename;
    for (char& character : foldedFilename)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (kind == K::Scene && foldedFilename == "sc_model_preview.json")
        throw std::invalid_argument("Preview scenes are not editable asset documents");
    if ((kind == K::Mesh && filename.rfind("SM_", 0) != 0) ||
        // 迁移期旧 SM_ 毛发只归 TressFX owner，避免同一资产出现两份独立草稿。
        (kind == K::Tressfx && filename.rfind("TF_", 0) != 0 && filename.rfind("SM_", 0) != 0) ||
        (kind == K::Texture && filename.rfind("T_", 0) != 0) ||
        (kind == K::Blueprint && filename.rfind("BP_", 0) != 0))
        throw std::invalid_argument("Mesh/TressFX/Texture/Blueprint descriptors require SM_/TF_/T_/BP_ JSON names");
}

void ValidateLegacyScene(const Json& scene)
{
    // 旧 UI 只管理已有 actor 参数；不因保存一个光源重新导入未修改的模型。
    ValidateWorldUnit(scene);
    if (scene.at("description").get<std::string>().empty() || !scene.at("objects").is_array())
        throw std::invalid_argument("Invalid scene document");
    for (const auto& object : scene.at("objects"))
    {
        const std::string type = object.at("type");
        if (type == "directionalLight" || type == "pointLight" ||
            type == "spotLight" || type == "squareAreaLight") ReadCanonicalLightIntensity(object);
        if (type == "camera")
        {
            ReadCameraExposure(object);
            const double fov = object.at("fov"), nearClip = object.at("near_clip"), farClip = object.at("far_clip");
            if (!std::isfinite(fov) || !std::isfinite(nearClip) || !std::isfinite(farClip) ||
                fov <= 0 || fov >= 179 || nearClip <= 0 || farClip <= nearClip)
                throw std::invalid_argument("Invalid camera projection");
        }
    }
}
} // namespace

AssetDocumentService::AssetDocumentService(std::filesystem::path root, K documentKind)
    : resourceRoot(std::filesystem::weakly_canonical(std::move(root))), kind(documentKind)
{
}

std::string AssetDocumentService::Normalize(const std::string& path) const
{
    ValidateRelativePath(path);
    const std::filesystem::path logical(PortablePath(path));
    const auto physical = std::filesystem::weakly_canonical(resourceRoot / logical);
    const auto relative = physical.lexically_relative(resourceRoot);
    std::string extension = logical.extension().string();
    for (char& character : extension) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." || extension != ".json")
        throw std::invalid_argument("Asset path must remain inside resource root and use .json");
    const auto normalized = logical.lexically_normal().generic_string();
    // 已打开的逻辑地址优先，保存时仍复核它的链接目标；别名和 Windows 大小写共用一份草稿。
    if (documents.count(normalized)) return normalized;
    for (const auto& entry : documents)
    {
        std::error_code error;
        if (std::filesystem::equivalent(physical, entry.second.absolutePath, error) && !error)
            return entry.first;
    }
    return normalized;
}

std::filesystem::path AssetDocumentService::Resolve(const std::string& path) const
{
    return std::filesystem::weakly_canonical(resourceRoot / Normalize(path));
}

std::filesystem::path AssetDocumentService::ResolveReference(const std::string& path) const
{
    ValidateRelativePath(path);
    const auto absolute = std::filesystem::weakly_canonical(resourceRoot / PortablePath(path));
    const auto relative = absolute.lexically_relative(resourceRoot);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
        !std::filesystem::is_regular_file(absolute))
        throw std::invalid_argument("Missing or escaped asset reference: " + path);
    return absolute;
}

bool AssetDocumentService::Open(const std::string& path, std::string& error)
{
    error.clear();
    try
    {
        const auto key = Normalize(path);
        if (documents.count(key)) return true;
        Document document;
        document.absolutePath = Resolve(key);
        const auto before = Persistence::MaterialInstancePersistence::ReadSnapshot(document.absolutePath);
        if (!before.Succeeded()) throw std::runtime_error(before.errorMessage);
        document.baseline = ReadJson(document.absolutePath);
        ValidateKind(kind, document.baseline, key);
        ValidateKind(kind, document.baseline, document.absolutePath.filename().generic_string());
        if (kind == K::Scene) ValidateLegacyScene(document.baseline);
        else if (kind == K::Mesh || kind == K::Tressfx) MeshAssetValidator::BuildLoadPlan(key, document.baseline);
        else if (kind == K::Blueprint) ValidateScriptBlueprintAsset(document.baseline);
        else ValidateTextureAssetJson(document.baseline);
        const auto after = Persistence::MaterialInstancePersistence::ReadSnapshot(document.absolutePath);
        if (!after.Succeeded() || before.digest != after.digest || Resolve(key) != document.absolutePath)
            throw std::runtime_error("Asset source changed while opening");
        document.digest = before.digest;
        document.draft = document.baseline;
        documents.emplace(key, std::move(document));
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}

void AssetDocumentService::Validate(const Json& candidate, const std::string& path) const
{
    ValidateKind(kind, candidate, path);
    if (kind == K::Scene)
    {
        ValidateSceneAssetJson(candidate);
        const auto effective = ResolveScriptBlueprintScene(resourceRoot, candidate, path);
        for (const auto& object : effective.at("objects"))
        {
            if (object.at("type") == "environment" && object.at("environment").at("type") == "hdri")
                ResolveReference(object.at("environment").at("hdrPath").get<std::string>());
            for (const char* field : {"modelPath", "materialOverride"})
                if (object.contains(field))
                {
                    const auto reference = ResolveReference(object.at(field).get<std::string>());
                    const auto asset = ReadJson(reference);
                    const std::string type = asset.value("type", "");
                    if ((std::string(field) == "modelPath" && type != "mesh" && type != "speedtree" && type != "tressfx") ||
                        (std::string(field) == "materialOverride" && type != "materialInstance"))
                        throw std::invalid_argument(std::string("Invalid reference type for ") + field);
                }
            if (object.at("type") == "blueprintInstance" && object.contains("components"))
                for (const auto& component : object.at("components"))
                    for (const char* field : {"modelPath", "materialOverride"})
                        if (component.contains(field))
                        {
                            const auto asset = ReadJson(ResolveReference(component.at(field).get<std::string>()));
                            const auto type = asset.value("type", "");
                            if ((std::string(field) == "modelPath" && type != "mesh" && type != "speedtree" && type != "tressfx") ||
                                (std::string(field) == "materialOverride" && type != "materialInstance"))
                                throw std::invalid_argument("Invalid Actor component reference type");
                        }
        }
    }
    else if (kind == K::Blueprint)
    {
        ValidateScriptBlueprintAsset(candidate);
        if (!blueprintClasses.is_null()) ValidateScriptPropertySchema(candidate, blueprintClasses);
        if (!blueprintComponentClasses.is_null()) ValidateScriptLogicPropertySchema(candidate, blueprintComponentClasses);
        const auto owner = Resolve(path).lexically_relative(resourceRoot);
        for (const auto& component : candidate.value("components", Json::array()))
            for (const char* field : {"modelPath", "materialOverride"})
                if (component.contains(field))
                {
                    const auto physical = ResolveReference(component.at(field).get<std::string>());
                    const auto reference = physical.lexically_relative(resourceRoot);
                    if ((ScriptContentDomain(owner) != "generated" && ScriptContentDomain(reference) == "generated") ||
                        (ScriptContentDomain(owner) == "common" && ScriptContentDomain(reference) == "maps"))
                        throw std::invalid_argument("Actor dependency violates content ownership");
                    const auto type = ReadJson(physical).value("type", "");
                    if ((std::string(field) == "modelPath" && type != "mesh" && type != "speedtree" && type != "tressfx") ||
                        (std::string(field) == "materialOverride" && type != "materialInstance"))
                        throw std::invalid_argument("Invalid Actor component reference type");
                }
    }
    else if (kind == K::Mesh || kind == K::Tressfx)
    {
        const auto plan = MeshAssetValidator::BuildLoadPlan(path, candidate);
        ResolveReference(plan.modelDataPath);
        if (!plan.skeletonPath.empty()) ResolveReference(plan.skeletonPath);
        if (!plan.animationSourceModelPath.empty())
        {
            const auto sourceModelReference = ResolveReference(plan.animationSourceModelPath);
            const auto sourceModel = ReadJson(sourceModelReference);
            if (sourceModel.value("type", "") != "mesh")
                throw std::invalid_argument(
                    "Mesh skinning.sourceModel must reference a skeletal mesh descriptor");
            // 让 sourceModel 自身的 descriptor 规则在编辑器边界先收口；
            // 正式导入仍会再次读取并验证骨架/FBX 内容。
            const auto sourcePlan = MeshAssetValidator::BuildLoadPlan(
                plan.animationSourceModelPath, sourceModel);
            if (!sourcePlan.IsSkeletal() || !sourcePlan.animationSourceModelPath.empty())
                throw std::invalid_argument(
                    "Mesh skinning.sourceModel must be a non-retargeted skeletal mesh descriptor");
        }
        if (!plan.retargetProfilePath.empty())
            ResolveReference(plan.retargetProfilePath);
        for (const auto& clip : plan.animationClips)
        {
            ResolveReference(clip.path);
            if (!clip.sequencePath.empty()) ResolveReference(clip.sequencePath);
        }
        for (const auto& montage : plan.animationMontages)
            ResolveReference(montage.path);
        for (const auto& slot : plan.materialSlots)
        {
            const auto reference = ResolveReference(slot.materialInstancePath);
            if (reference.extension() != ".json" || reference.filename().string().rfind("MI_", 0) != 0 ||
                ReadJson(reference).value("type", "") != "materialInstance")
                throw std::invalid_argument("Mesh slots require MI_*.json materialInstance references");
        }
    }
    else
    {
        ValidateTextureAssetJson(candidate);
        ResolveReference(candidate.at("source").get<std::string>());
    }
}

void AssetDocumentService::PublishDraft(Document& document, Json candidate)
{
    if (candidate == document.draft) return;
    document.draft.swap(candidate);
    ++document.revision;
    document.status = document.draft == document.baseline ? EditorUi::EditorDocumentStatus::Clean
        : EditorUi::EditorDocumentStatus::Dirty;
    document.message.clear();
    if (document.draft == document.baseline) document.edits.clear();
}

void AssetDocumentService::TrimBlueprintHistory(BlueprintHistory& history, bool redo)
{
    auto& oldest = redo ? history.redo : history.undo;
    // undo/redo 合计预算；回放时优先淘汰最远的同向历史，保留刚生成的反向操作。
    while (history.undo.size() + history.redo.size() > 32 || history.bytes > 16 * 1024 * 1024)
    {
        auto& entries = oldest.empty() ? (redo ? history.undo : history.redo) : oldest;
        history.bytes -= entries.front().bytes;
        entries.erase(entries.begin());
    }
}

void AssetDocumentService::PublishBlueprintDraft(Document& document, Json candidate)
{
    if (candidate == document.draft) return;
    // 先准备历史副本；分配或序列化失败不能留下半笔已提交的作者操作。
    auto history = document.blueprintHistory;
    for (const auto& entry : history.redo) history.bytes -= entry.bytes;
    history.redo.clear();
    const auto bytes = document.draft.dump().size();
    history.undo.push_back({document.draft, bytes});
    history.bytes += bytes;
    TrimBlueprintHistory(history, false);
    PublishDraft(document, std::move(candidate));
    document.blueprintHistory = std::move(history);
}

void AssetDocumentService::MoveBlueprintHistory(Document& document, const std::string& path, bool redo)
{
    const auto& currentSource = redo ? document.blueprintHistory.redo : document.blueprintHistory.undo;
    if (currentSource.empty()) throw std::invalid_argument("No Blueprint edit available");
    auto candidate = currentSource.back().value;
    // 引用文件与托管类目录可能已改变；历史也必须通过当前作者边界的完整候选校验。
    Validate(candidate, path);
    auto history = document.blueprintHistory;
    auto& source = redo ? history.redo : history.undo;
    auto& destination = redo ? history.undo : history.redo;
    history.bytes -= source.back().bytes;
    source.pop_back();
    const auto bytes = document.draft.dump().size();
    destination.push_back({document.draft, bytes});
    history.bytes += bytes;
    TrimBlueprintHistory(history, redo);
    PublishDraft(document, std::move(candidate));
    document.blueprintHistory = std::move(history);
}

EditorAssetDocumentResultPayload AssetDocumentService::Describe(const std::string& path, const Document& document) const
{
    EditorAssetDocumentResultPayload result;
    result.assetPath = path;
    result.documentRevision = document.revision;
    result.dirty = document.draft != document.baseline;
    result.sourceDigest = document.digest.ToHex();
    result.canUndo = kind == K::Blueprint && !document.blueprintHistory.undo.empty();
    result.canRedo = kind == K::Blueprint && !document.blueprintHistory.redo.empty();
    result.document = ProjectEditableAsset(kind, document.draft);
    result.capabilities = DescribeAssetCapabilities(kind, document.draft);
    return result;
}

void AssetDocumentService::DescribeFailure(const std::string& path, const Document& document,
    const EditorCommandEnvelope& command, EditorCommandResult& result) const
{
    auto description = Describe(path, document);
    Json targets = Json::array();
    if (const auto* modify = std::get_if<ModifyAssetDocumentPayload>(&command.payload))
        for (const auto& change : modify->changes)
        {
            Json target{{"field", change.field}};
            if (change.objectIdentity) target["objectIdentity"] = *change.objectIdentity;
            if (change.slotIndex) target["slotIndex"] = *change.slotIndex;
            if (change.expectedSlotName) target["expectedSlotName"] = *change.expectedSlotName;
            targets.push_back(std::move(target));
        }
    // 完整候选可因字段组合失败；targets 表示本次请求范围，不伪装成已定位的单字段错误。
    description.diagnostics.push_back({{"scope", "candidate"}, {"code", GetEditorErrorCodeName(result.errorCode)},
        {"message", result.message}, {"targets", std::move(targets)}});
    result.payload = std::move(description);
}

EditorCommandResult AssetDocumentService::Execute(const EditorCommandEnvelope& command)
{
    EditorCommandResult result;
    result.commandId = command.commandId;
    result.status = EditorCommandStatus::Rejected;
    std::string key;
    try
    {
        if (const auto error = ValidateEditorCommandEnvelope(command))
        {
            result.errorCode = *error;
            result.message = std::string(GetEditorErrorCodeName(*error));
            return result;
        }
        if (GetEditorAssetDocumentKind(command.type) != kind)
        {
            result.errorCode = EditorErrorCode::InvalidCommandType;
            return result;
        }
        key = Normalize(*GetEditorCommandAssetPath(command));
        const auto operation = *GetEditorAssetOperation(command.type);
        if (!documents.count(key))
        {
            if (operation != O::GetDocument && operation != O::GetCapabilities)
            {
                result.errorCode = EditorErrorCode::DocumentNotOpen;
                result.message = "Read the asset document before executing this command";
                return result;
            }
            std::string error;
            if (!Open(key, error))
            {
                result.status = EditorCommandStatus::Failed;
                result.errorCode = std::filesystem::exists(resourceRoot / key) ?
                    EditorErrorCode::ValidationFailed : EditorErrorCode::AssetNotFound;
                result.message = error;
                return result;
            }
        }
        auto& document = documents.at(key);
        result.documentRevision = document.revision;
        if (command.expectedDocumentRevision && *command.expectedDocumentRevision != document.revision)
        {
            result.errorCode = EditorErrorCode::StaleDocumentRevision;
            result.message = "Expected revision does not match the asset document";
            return result;
        }
        Json changed = Json::array();
        if (operation == O::Modify)
        {
            auto candidate = document.draft;
            changed = ApplyAssetChanges(candidate, kind,
                std::get<ModifyAssetDocumentPayload>(command.payload).changes);
            Validate(candidate, key);
            if (candidate != document.draft)
            {
                if (kind == K::Blueprint) PublishBlueprintDraft(document, std::move(candidate));
                else PublishDraft(document, std::move(candidate));
                document.standardEdited = true;
                if (kind == K::Scene) document.replayViaDraft = document.awaitingWorldLoad = true;
            }
            else changed.clear();
        }
        else if (operation == O::Validate) Validate(document.draft, key);
        else if (operation == O::Undo || operation == O::Redo)
            MoveBlueprintHistory(document, key, operation == O::Redo);
        else if (operation == O::Revert)
        {
            PublishDraft(document, document.baseline);
            document.blueprintHistory = {};
            document.edits.clear();
            document.replayViaDraft = kind == K::Scene;
            document.awaitingWorldLoad = kind == K::Scene;
        }
        else if (operation == O::Reload)
        {
            const auto policy = std::get<ReloadMaterialInstanceDocumentPayload>(command.payload).dirtyPolicy;
            if (policy != EditorDirtyDocumentPolicy::RequireClean && policy != EditorDirtyDocumentPolicy::DiscardChanges)
                throw std::invalid_argument("Invalid dirtyPolicy");
            if (document.draft != document.baseline && policy != EditorDirtyDocumentPolicy::DiscardChanges)
            {
                result.errorCode = EditorErrorCode::DocumentDirty;
                result.message = "Reload requires a clean document or discard_changes";
                return result;
            }
            const uint64_t nextRevision = document.revision + 1;
            // 先在独立 owner 中读取，失败保留旧草稿和基线；成功后才替换原文档。
            AssetDocumentService replacement(resourceRoot, kind);
            replacement.blueprintClasses = blueprintClasses;
            replacement.blueprintComponentClasses = blueprintComponentClasses;
            std::string error;
            if (!replacement.Open(key, error)) throw std::runtime_error(error);
            auto candidate = std::move(replacement.documents.at(key));
            candidate.revision = nextRevision;
            candidate.awaitingWorldLoad = kind == K::Scene;
            document = std::move(candidate);
        }
        else if (operation == O::Save)
        {
            if (document.standardEdited || kind != K::Scene) Validate(document.draft, key);
            else ValidateLegacyScene(document.draft);
            if (Resolve(key) != document.absolutePath)
            {
                document.status = EditorUi::EditorDocumentStatus::SourceChanged;
                document.message = "Asset physical target changed after opening";
                result.status = EditorCommandStatus::Failed;
                result.errorCode = EditorErrorCode::SourceChanged;
                result.message = document.message;
                DescribeFailure(key, document, command, result);
                return result;
            }
            const auto saved = Persistence::MaterialInstancePersistence::SaveTextIfUnchanged(
                document.absolutePath, document.digest, document.draft.dump(4) + "\n");
            if (!saved.Succeeded())
            {
                document.status = saved.HasSourceConflict() ? EditorUi::EditorDocumentStatus::SourceChanged
                    : EditorUi::EditorDocumentStatus::SaveFailed;
                document.message = saved.errorMessage;
                result.status = EditorCommandStatus::Failed;
                result.errorCode = saved.HasSourceConflict() ? EditorErrorCode::SourceChanged : EditorErrorCode::AtomicWriteFailed;
                result.message = saved.errorMessage;
                DescribeFailure(key, document, command, result);
                return result;
            }
            document.baseline = document.draft;
            document.digest = saved.newDigest;
            document.edits.clear();
            document.replayViaDraft = false;
            document.status = EditorUi::EditorDocumentStatus::Clean;
            document.message.clear();
            ++document.revision;
        }
        result.status = EditorCommandStatus::Succeeded;
        result.documentRevision = document.revision;
        auto description = Describe(key, document);
        description.changedFields = std::move(changed);
        result.payload = std::move(description);
        result.message = std::string(GetEditorCommandName(command.type)) + " succeeded";
    }
    catch (const std::exception& exception)
    {
        result.errorCode = EditorErrorCode::ValidationFailed;
        result.message = exception.what();
        if (GetEditorAssetOperation(command.type) == O::Save && documents.count(key))
        {
            documents.at(key).status = EditorUi::EditorDocumentStatus::SaveFailed;
            documents.at(key).message = result.message;
            result.status = EditorCommandStatus::Failed;
        }
        if (documents.count(key)) DescribeFailure(key, documents.at(key), command, result);
    }
    return result;
}

std::vector<EditorUi::EditorModifiedDocumentSnapshot> AssetDocumentService::ModifiedDocuments() const
{
    std::vector<EditorUi::EditorModifiedDocumentSnapshot> result;
    for (const auto& entry : documents)
        if (entry.second.draft != entry.second.baseline)
            result.push_back({entry.first, kind == K::Scene ? EditorUi::EditorAssetKind::Scene :
                kind == K::Mesh ? EditorUi::EditorAssetKind::Mesh :
                kind == K::Tressfx ? EditorUi::EditorAssetKind::Tressfx : kind == K::Blueprint ?
                EditorUi::EditorAssetKind::Blueprint : EditorUi::EditorAssetKind::Texture,
                entry.second.revision, entry.second.status, entry.second.message});
    return result;
}

std::vector<EditorAssetDocumentResultPayload> AssetDocumentService::DocumentSnapshots() const
{
    std::vector<EditorAssetDocumentResultPayload> result;
    for (const auto& entry : documents) result.push_back(Describe(entry.first, entry.second));
    return result;
}
} // namespace VL::Editor
