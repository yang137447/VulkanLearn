#include "editor/service/sceneDocumentService.h"

#include <fstream>
#include <stdexcept>
#include "render/lighting/physicalLighting.h"
#include "render/camera/cameraExposure.h"

namespace VL::Editor
{
namespace
{
void ValidateManagedFields(const nlohmann::json& scene)
{
    // 这里只复核编辑器管理的作者参数；模型引用未被编辑，不在保存时重新导入整个关卡。
    ValidateWorldUnit(scene);
    if (scene.value("type", "") != "scene" || scene.at("description").get<std::string>().empty() ||
        !scene.at("objects").is_array())
        throw std::runtime_error("Invalid scene document.");
    for (const auto& object : scene.at("objects"))
    {
        const auto type = object.at("type").get<std::string>();
        if (type == "directionalLight" || type == "pointLight" ||
            type == "spotLight" || type == "squareAreaLight")
            ReadCanonicalLightIntensity(object);
        if (type == "camera")
        {
            ReadCameraExposure(object);
            const double fov = object.at("fov"), nearClip = object.at("near_clip"),
                farClip = object.at("far_clip");
            if (!std::isfinite(fov) || !std::isfinite(nearClip) || !std::isfinite(farClip) ||
                fov <= 0 || fov >= 179 || nearClip <= 0 || farClip <= nearClip)
                throw std::runtime_error("Invalid camera projection.");
        }
    }
}

nlohmann::json Vector(const Eigen::Vector3f& value)
{
    return nlohmann::json::array({value.x(), value.y(), value.z()});
}

void RestoreEquivalentNumbers(nlohmann::json& draft, const nlohmann::json& baseline)
{
    // World 使用 float；回到相同作者值时恢复原始 JSON 精度，不用 epsilon 擦掉小修改。
    if (draft.is_number() && baseline.is_number() &&
        draft.get<float>() == baseline.get<float>()) draft = baseline;
    else if (draft.is_array() && baseline.is_array() && draft.size() == baseline.size())
        for (size_t index = 0; index < draft.size(); ++index)
            RestoreEquivalentNumbers(draft[index], baseline[index]);
    else if (draft.is_object() && baseline.is_object())
        for (auto& field : draft.items())
            if (baseline.contains(field.key()))
                RestoreEquivalentNumbers(field.value(), baseline.at(field.key()));
}

void WriteIntensity(nlohmann::json& object, float canonical)
{
    // 保留作者的 lux/cd/lm 单位。面光 World 值为辐亮度，spot 的 lm 依赖当前锥角。
    const std::string type = object.at("type");
    const std::string unit = object.at("intensityUnit");
    double factor = 1.0;
    if (type == "squareAreaLight")
    {
        const double sideMeters = object.at("size").get<double>() / WorldUnitsPerMeter;
        factor = sideMeters * sideMeters;
        if (unit == "lm") factor *= LightingPi;
    }
    else if (unit == "lm")
    {
        factor = type == "spotLight" ? SpotEffectiveSolidAngle(
            object.at("cone_angle_inner").get<double>() * LightingPi / 180.0,
            object.at("cone_angle_outer").get<double>() * LightingPi / 180.0)
            : 4.0 * LightingPi;
    }
    object["intensity"] = static_cast<double>(canonical) * factor;
}
} // namespace

SceneDocumentService::SceneDocumentService(std::filesystem::path root)
    : resourceRoot(std::filesystem::weakly_canonical(std::move(root)))
{
}

bool SceneDocumentService::IsPreviewScene(const std::string& path)
{
    return std::filesystem::path(path).filename() == "SC_model_preview.json";
}

std::string SceneDocumentService::Normalize(const std::string& path) const
{
    const auto absolute = std::filesystem::weakly_canonical(
        std::filesystem::path(path).is_absolute() ? std::filesystem::path(path)
        : resourceRoot / std::filesystem::path(path));
    const auto relative = absolute.lexically_relative(resourceRoot);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
        relative.extension() != ".json")
        throw std::runtime_error("Scene path must remain inside the resource root.");
    return relative.generic_string();
}

std::filesystem::path SceneDocumentService::Resolve(const std::string& path) const
{
    return resourceRoot / Normalize(path);
}

bool SceneDocumentService::Open(const std::string& path, std::string& error)
{
    try
    {
        if (IsPreviewScene(path)) return true;
        const auto key = Normalize(path);
        const auto existing = documents.find(key);
        if (existing != documents.end() && existing->second.draft != existing->second.baseline)
            return true;
        Document document;
        document.absolutePath = Resolve(key);
        const auto before = Persistence::MaterialInstancePersistence::ReadSnapshot(document.absolutePath);
        if (!before.Succeeded()) throw std::runtime_error(before.errorMessage);
        std::ifstream file(document.absolutePath);
        document.baseline = nlohmann::json::parse(file);
        if (document.baseline.value("type", "") != "scene")
            throw std::runtime_error("The asset is not a scene document.");
        ValidateManagedFields(document.baseline);
        const auto after = Persistence::MaterialInstancePersistence::ReadSnapshot(document.absolutePath);
        if (!after.Succeeded() || before.digest != after.digest)
            throw std::runtime_error("Scene source changed while opening.");
        document.digest = before.digest;
        document.draft = document.baseline;
        if (existing != documents.end()) document.revision = existing->second.revision + 1;
        documents[key] = std::move(document);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

nlohmann::json* SceneDocumentService::FindActor(nlohmann::json& json,
    const std::string& identity)
{
    const auto separator = identity.find('|');
    if (separator == std::string::npos || separator == 0 || separator + 1 == identity.size())
        return nullptr;
    const std::string type = identity.substr(0, separator);
    const std::string name = identity.substr(separator + 1);
    nlohmann::json* match = nullptr;
    for (auto& object : json.at("objects"))
    {
        if (object.value("type", "") != type || object.value("name", "") != name) continue;
        if (match != nullptr) return nullptr;
        match = &object;
    }
    return match;
}

bool SceneDocumentService::PrepareActorEdit(const std::string& path,
    const std::string& identity, std::string& error)
{
    try
    {
        if (IsPreviewScene(path)) return true;
        const auto key = Normalize(path);
        if (documents.count(key) == 0 && !Open(path, error)) return false;
        if (FindActor(documents.at(key).draft, identity)) return true;
        error = "Scene actor identity is missing or ambiguous in the document.";
    }
    catch (const std::exception& exception) { error = exception.what(); }
    return false;
}

void SceneDocumentService::RecordActorEdit(const EditorUi::EditorSceneActorSnapshot& actor,
    const EditorActorPropertyEdit& edit)
{
    if (IsPreviewScene(actor.scenePath)) return;
    auto& document = documents.at(Normalize(actor.scenePath));
    auto& object = *FindActor(document.draft, actor.identity);
    const auto previous = document.draft;
    using P = EditorActorProperty;
    switch (edit.property)
    {
    case P::Position: object["position"] = Vector(actor.position); break;
    case P::Rotation: object["rotation"] = Vector(actor.rotation); break;
    case P::Scale: object["scale"] = Vector(actor.scale); break;
    case P::Normal: object["normal"] = Vector(actor.light.direction); break;
    case P::Color: object["color"] = Vector(actor.light.color); break;
    case P::Intensity: WriteIntensity(object, actor.light.intensity); break;
    case P::InnerCone:
        object["cone_angle_inner"] = actor.light.coneAngleInner * 180.0 / LightingPi;
        WriteIntensity(object, actor.light.intensity);
        break;
    case P::OuterCone:
        object["cone_angle_outer"] = actor.light.coneAngleOuter * 180.0 / LightingPi;
        WriteIntensity(object, actor.light.intensity);
        break;
    case P::AreaSize:
        object["size"] = actor.light.squareSize;
        WriteIntensity(object, actor.light.intensity);
        break;
    case P::CameraFov: object["fov"] = actor.camera.horizontalFovDegrees; break;
    case P::CameraNear: object["near_clip"] = actor.camera.clipNear; break;
    case P::CameraFar: object["far_clip"] = actor.camera.clipFar; break;
    case P::CastShadows: object["shadow"]["castShadows"] = actor.castShadows; break;
    }
    if (actor.type == EditorUi::EditorSceneActorType::Camera &&
        (edit.property == P::Position || edit.property == P::Rotation))
    {
        // look_at 在加载时优先于 rotation；变换后以实际朝向写 TRS，不能留下旧 target 覆盖新旋转。
        object["rotation"] = Vector(actor.rotation);
        object.erase("look_at");
    }
    const auto* baselineActor = FindActor(document.baseline, actor.identity);
    if (baselineActor) RestoreEquivalentNumbers(object, *baselineActor);
    if (edit.property == P::CastShadows && actor.castShadows && baselineActor &&
        (!baselineActor->contains("shadow") || !baselineActor->at("shadow").contains("castShadows")))
    {
        object["shadow"].erase("castShadows");
        if (object["shadow"].empty()) object.erase("shadow");
    }
    if (document.draft == previous) return;
    document.edits.push_back(edit);
    ++document.revision;
    document.status = document.draft == document.baseline
        ? EditorUi::EditorDocumentStatus::Clean : EditorUi::EditorDocumentStatus::Dirty;
    document.message.clear();
    if (document.status == EditorUi::EditorDocumentStatus::Clean) document.edits.clear();
}

EditorCommandResult SceneDocumentService::Save(const EditorCommandEnvelope& command)
{
    EditorCommandResult result;
    result.commandId = command.commandId;
    result.status = EditorCommandStatus::Failed;
    try
    {
        const auto path = std::get<MaterialInstanceAssetPathPayload>(command.payload).assetPath;
        auto& document = documents.at(Normalize(path));
        result.documentRevision = document.revision;
        if (command.expectedDocumentRevision != document.revision)
        {
            result.status = EditorCommandStatus::Rejected;
            result.errorCode = EditorErrorCode::StaleDocumentRevision;
            result.message = "Scene changed after the save confirmation was opened.";
            return result;
        }
        if (Resolve(path) != document.absolutePath)
            throw std::runtime_error("Scene source path changed after opening.");
        ValidateManagedFields(document.draft);
        const auto saved = Persistence::MaterialInstancePersistence::SaveTextIfUnchanged(
            document.absolutePath, document.digest, document.draft.dump(4) + "\n");
        if (!saved.Succeeded())
        {
            document.status = saved.HasSourceConflict() ? EditorUi::EditorDocumentStatus::SourceChanged
                : EditorUi::EditorDocumentStatus::SaveFailed;
            document.message = saved.errorMessage;
            result.errorCode = saved.HasSourceConflict() ? EditorErrorCode::SourceChanged
                : EditorErrorCode::AtomicWriteFailed;
            result.message = document.message;
            return result;
        }
        document.baseline = document.draft;
        document.digest = saved.newDigest;
        document.edits.clear();
        document.status = EditorUi::EditorDocumentStatus::Clean;
        document.message.clear();
        result.documentRevision = ++document.revision;
        result.status = EditorCommandStatus::Succeeded;
        result.message = "Saved " + path;
    }
    catch (const std::exception& exception)
    {
        result.errorCode = EditorErrorCode::ValidationFailed;
        result.message = exception.what();
        const auto path = GetEditorCommandAssetPath(command);
        if (path)
        {
            auto found = documents.find(*path);
            if (found != documents.end())
            {
                found->second.status = EditorUi::EditorDocumentStatus::SaveFailed;
                found->second.message = result.message;
            }
        }
    }
    return result;
}

std::vector<EditorUi::EditorModifiedDocumentSnapshot> SceneDocumentService::ModifiedDocuments() const
{
    std::vector<EditorUi::EditorModifiedDocumentSnapshot> result;
    for (const auto& entry : documents)
        if (entry.second.draft != entry.second.baseline)
            result.push_back({entry.first, EditorUi::EditorAssetKind::Scene, entry.second.revision,
                entry.second.status, entry.second.message});
    return result;
}

std::vector<EditorActorPropertyEdit> SceneDocumentService::ReplayEdits(
    const std::string& path, uint64_t generation, std::string& error)
{
    error.clear();
    try
    {
        if (IsPreviewScene(path)) return {};
        const auto found = documents.find(Normalize(path));
        if (found == documents.end() || found->second.edits.empty()) return {};
        const auto source = Persistence::MaterialInstancePersistence::ReadSnapshot(found->second.absolutePath);
        if (!source.Succeeded() || source.digest != found->second.digest ||
            Resolve(path) != found->second.absolutePath)
        {
            // 外部修改不能借 World 换代覆盖旧草稿；冻结状态让保存窗口明确显示冲突。
            error = "Scene source changed; the retained draft was not applied to the replacement World.";
            found->second.status = EditorUi::EditorDocumentStatus::SourceChanged;
            found->second.message = error;
            return {};
        }
        auto edits = found->second.edits;
        for (auto& edit : edits) edit.worldGeneration = generation;
        return edits;
    }
    catch (const std::exception& exception) { error = exception.what(); }
    return {};
}
} // namespace VL::Editor
