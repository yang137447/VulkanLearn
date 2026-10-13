#include "editor/service/sceneDocumentService.h"

#include <fstream>
#include <cctype>
#include <stdexcept>
#include "render/lighting/physicalLighting.h"
#include "render/camera/cameraExposure.h"

namespace VL::Editor
{
namespace
{
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
    : AssetDocumentService(std::move(root), EditorAssetDocumentKind::Scene)
{
}

bool SceneDocumentService::IsPreviewScene(const std::string& path)
{
    std::string filename = std::filesystem::path(path).filename().string();
    for (char& character : filename)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return filename == "sc_model_preview.json";
}

std::string SceneDocumentService::Normalize(const std::string& path) const
{
    // 已加载 World 的内部地址是绝对路径；外部协议仍由基类只接受资源根相对路径。
    const auto input = std::filesystem::path(path);
    return AssetDocumentService::Normalize(input.is_absolute()
        ? input.lexically_relative(resourceRoot).generic_string() : path);
}

std::filesystem::path SceneDocumentService::Resolve(const std::string& path) const
{
    return AssetDocumentService::Resolve(Normalize(path));
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
        if (existing == documents.end()) return AssetDocumentService::Open(key, error);
        EditorCommandEnvelope reload;
        reload.commandId = 1;
        reload.type = EditorCommandType::ReloadSceneDocument;
        reload.expectedDocumentRevision = existing->second.revision;
        reload.payload = ReloadMaterialInstanceDocumentPayload{key, EditorDirtyDocumentPolicy::RequireClean};
        const bool awaitingWorldLoad = existing->second.awaitingWorldLoad;
        const auto result = Execute(reload);
        if (result.status == EditorCommandStatus::Succeeded) documents.at(key).awaitingWorldLoad = awaitingWorldLoad;
        if (result.status != EditorCommandStatus::Succeeded) error = result.message;
        return result.status == EditorCommandStatus::Succeeded;
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
        if (documents.at(key).awaitingWorldLoad)
        {
            error = "Scene asset edits are staged; reload the scene before editing its live actors.";
            return false;
        }
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
    auto previous = document.draft;
    using P = EditorActorProperty;
    const auto* baselineActor = FindActor(document.baseline, actor.identity);
    if (const char* field = CameraExposureField(edit.property))
    {
        // 只补丁已接受的字段，保留物理相机参数与未写入 JSON 的自动默认值。
        auto& exposure = object["exposure"];
        exposure[field] = DescribeCameraExposure(actor.camera.exposure).at(field);
        if (baselineActor && !baselineActor->at("exposure").contains(field))
        {
            const auto baselineValue = DescribeCameraExposure(
                ReadCameraExposure(*baselineActor)).at(field);
            if (exposure[field].get<float>() == baselineValue.get<float>())
                exposure.erase(field);
        }
    }
    else switch (edit.property)
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
    default: break; // 曝光字段已由上方具名映射处理。
    }
    if (actor.type == EditorUi::EditorSceneActorType::Camera &&
        (edit.property == P::Position || edit.property == P::Rotation))
    {
        // look_at 在加载时优先于 rotation；变换后以实际朝向写 TRS，不能留下旧 target 覆盖新旋转。
        object["rotation"] = Vector(actor.rotation);
        object.erase("look_at");
    }
    if (baselineActor) RestoreEquivalentNumbers(object, *baselineActor);
    if (edit.property == P::CastShadows && actor.castShadows && baselineActor &&
        (!baselineActor->contains("shadow") || !baselineActor->at("shadow").contains("castShadows")))
    {
        object["shadow"].erase("castShadows");
        if (object["shadow"].empty()) object.erase("shadow");
    }
    if (document.standardEdited)
    {
        // API 草稿可能尚未应用到 live World；UI 单分量编辑不能用旧 World 的其它分量覆盖它。
        const char* field = edit.property == P::Position ? "position" : edit.property == P::Rotation ? "rotation"
            : edit.property == P::Scale ? "scale" : edit.property == P::Color ? "color" : nullptr;
        if (field)
        {
            const auto component = object.at(field).at(edit.component);
            object[field] = FindActor(previous, actor.identity)->at(field);
            object[field][edit.component] = component;
        }
    }
    if (document.draft == previous) return;
    document.edits.push_back(edit);
    auto candidate = std::move(document.draft);
    document.draft = previous;
    PublishDraft(document, std::move(candidate));
}

EditorCommandResult SceneDocumentService::Save(const EditorCommandEnvelope& command)
{
    return Execute(command);
}

std::vector<EditorActorPropertyEdit> SceneDocumentService::ReplayEdits(
    const std::string& path, uint64_t generation, std::string& error)
{
    error.clear();
    try
    {
        if (IsPreviewScene(path)) return {};
        const auto found = documents.find(Normalize(path));
        if (found != documents.end() && found->second.replayViaDraft) return {};
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

nlohmann::json SceneDocumentService::DraftForWorldLoad(const std::string& path, std::string& error)
{
    error.clear();
    if (IsPreviewScene(path)) return nullptr;
    try
    {
        const auto key = Normalize(path);
        const auto found = documents.find(key);
        if (found == documents.end() || !found->second.replayViaDraft) return nullptr;
        const auto source = Persistence::MaterialInstancePersistence::ReadSnapshot(found->second.absolutePath);
        if (!source.Succeeded() || source.digest != found->second.digest ||
            Resolve(key) != found->second.absolutePath)
        {
            found->second.status = EditorUi::EditorDocumentStatus::SourceChanged;
            found->second.message = "Scene source changed; retained draft cannot be loaded";
            error = found->second.message;
            return nullptr;
        }
        return found->second.draft;
    }
    catch (const std::exception& exception) { error = exception.what(); return nullptr; }
}

void SceneDocumentService::NotifyWorldLoaded(const std::string& path)
{
    if (IsPreviewScene(path)) return;
    const auto found = documents.find(Normalize(path));
    if (found != documents.end()) found->second.awaitingWorldLoad = false;
}
} // namespace VL::Editor
