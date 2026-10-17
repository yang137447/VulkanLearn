#include "editor/service/sceneDocumentService.h"

#include <fstream>
#include <cctype>
#include <stdexcept>
#include "render/lighting/physicalLighting.h"
#include "render/camera/cameraExposure.h"
#include "script/scriptBlueprintAsset.h"
#include "script/scriptSourceFiles.h"
#include "shader/build/atomicFile.h"
#include <atomic>
#include <chrono>
#include <set>

namespace VL::Editor
{
namespace
{
std::filesystem::path BlueprintCreationTarget(const std::filesystem::path& root, const std::string& path)
{
    const auto target = ResolveScriptAssetPath(root, path);
    const auto relative = target.lexically_relative(std::filesystem::weakly_canonical(root));
    std::vector<std::string> parts;
    for (const auto& part : relative) parts.push_back(part.string());
    const bool common = parts.size() >= 3 && parts[0] == "Common" && parts[1] == "Blueprints";
    const bool scene = parts.size() >= 4 && parts[0] == "Maps" &&
        parts[1].size() > 3 && parts[1].rfind("SC_", 0) == 0 && parts[2] == "Blueprints";
    if ((!common && !scene) || target.filename().string().rfind("BP_", 0) != 0)
        throw std::invalid_argument("Use Common/Blueprints/BP_*.json or Maps/SC_*/Blueprints/BP_*.json");
    return target;
}

void ValidateBlueprintSourceClass(const std::string& className)
{
    // 创建模板只接受普通 ASCII 标识符；转义、泛型及程序集限定名留给已有类选择入口。
    static const std::set<std::string> keywords{
        "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked",
        "class", "const", "continue", "decimal", "default", "delegate", "do", "double", "else",
        "enum", "event", "explicit", "extern", "false", "finally", "fixed", "float", "for",
        "foreach", "goto", "if", "implicit", "in", "int", "interface", "internal", "is", "lock",
        "long", "namespace", "new", "null", "object", "operator", "out", "override", "params",
        "private", "protected", "public", "readonly", "ref", "return", "sbyte", "sealed", "short",
        "sizeof", "stackalloc", "static", "string", "struct", "switch", "this", "throw", "true",
        "try", "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort", "using", "virtual",
        "void", "volatile", "while", "record", "required", "file", "scoped", "var", "dynamic"
    };
    if (className.empty() || className.size() > 256 || className == "VulkanLearn.Blueprint")
        throw std::invalid_argument("New Blueprint class requires a C# type name of at most 256 ASCII characters");
    size_t begin = 0;
    while (true)
    {
        const auto end = className.find('.', begin);
        const auto token = className.substr(begin, end == std::string::npos ? end : end - begin);
        if (token.empty() || keywords.count(token))
            throw std::invalid_argument("New Blueprint class contains an empty or reserved C# identifier");
        for (size_t index = 0; index < token.size(); ++index)
        {
            const char value = token[index];
            if (value != '_' && !(value >= 'A' && value <= 'Z') &&
                !(value >= 'a' && value <= 'z') && !(index > 0 && value >= '0' && value <= '9'))
                throw std::invalid_argument("New Blueprint class requires dot-separated ASCII C# identifiers");
        }
        if (end == std::string::npos) return;
        begin = end + 1;
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
        if (object.value("type", "") != type ||
            object.value(type == "blueprintInstance" ? "id" : "name", "") != name) continue;
        if (match != nullptr) return nullptr;
        match = &object;
    }
    return match;
}

nlohmann::json SceneDocumentService::ScriptBlueprintDraft(const std::string& path,
    const std::string& identity, std::string& error)
{
    const auto key = Normalize(path);
    const auto document = documents.find(key);
    const auto history = histories.find(key);
    // 连续脚本作者请求只读最新作者 JSON，不依赖旧 World 的参数。
    // 标准 API 改写会使历史值不符，仍走下方的 staged Scene 门禁。
    if (document != documents.end() && history != histories.end() &&
        history->second.current == document->second.draft)
        if (const auto* actor = FindActor(document->second.draft, identity))
        {
            error.clear();
            return *actor;
        }
    if (!PrepareActorEdit(path, identity, error)) return nullptr;
    return *FindActor(documents.at(Normalize(path)).draft, identity);
}

nlohmann::json SceneDocumentService::ResolveBlueprint(const nlohmann::json& actor,
    const std::string& scenePath) const
{
    return ResolveScriptBlueprint(resourceRoot, actor, scenePath);
}

nlohmann::json SceneDocumentService::BlueprintAssetDefaults(const std::string& path,
    const std::string& scenePath) const
{
    return ReadScriptBlueprintAsset(resourceRoot, path, scenePath);
}

bool SceneDocumentService::StageScriptBlueprint(const std::string& path,
    const nlohmann::json& actor, bool create, std::string& error)
{
    error.clear();
    try
    {
        if (IsPreviewScene(path)) throw std::invalid_argument("Cannot place Actors in a preview scene");
        if (!Open(path, error)) return false;
        const auto key = Normalize(path);
        auto& document = documents.at(key);
        auto candidate = document.draft;
        auto* existing = FindActor(candidate, "blueprintInstance|" + actor.at("id").get<std::string>());
        if (create)
        {
            if (existing) throw std::invalid_argument("Blueprint ID already exists");
            candidate.at("objects").push_back(actor);
        }
        else
        {
            if (!existing) throw std::invalid_argument("Actor no longer exists");
            *existing = actor;
        }
        Validate(candidate, key);
        PublishSceneDraft(key, document, std::move(candidate));
        document.replayViaDraft = document.awaitingWorldLoad = document.standardEdited = true;
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}

void SceneDocumentService::PublishSceneDraft(const std::string& key, Document& document,
    nlohmann::json candidate)
{
    if (candidate == document.draft) return;
    auto& history = histories[key];
    if (history.current != document.draft) history = {};
    history.undo.push_back(document.draft);
    history.redo.clear();
    // 大场景有界保留：最多 32 步、16 MiB 序列化历史，不随长时间编辑无限增长。
    size_t bytes = 0;
    for (const auto& snapshot : history.undo) bytes += snapshot.dump().size();
    while (history.undo.size() > 32 || (!history.undo.empty() && bytes > 16 * 1024 * 1024))
    {
        bytes -= history.undo.front().dump().size();
        history.undo.erase(history.undo.begin());
    }
    PublishDraft(document, std::move(candidate));
    history.current = document.draft;
}

bool SceneDocumentService::RemoveScriptBlueprint(const std::string& path,
    const std::string& identity, std::string& error)
{
    try
    {
        if (IsPreviewScene(path)) throw std::invalid_argument("Cannot edit preview scenes");
        if (identity.rfind("blueprintInstance|", 0) != 0)
            throw std::invalid_argument("Delete requires a script Actor identity");
        if (!PrepareActorEdit(path, identity, error)) return false;
        const auto key = Normalize(path);
        auto& document = documents.at(key);
        auto candidate = document.draft;
        auto* actor = FindActor(candidate, identity);
        auto& objects = candidate.at("objects");
        for (auto item = objects.begin(); item != objects.end(); ++item)
            if (&*item == actor) { objects.erase(item); break; }
        Validate(candidate, key);
        PublishSceneDraft(key, document, std::move(candidate));
        document.replayViaDraft = document.awaitingWorldLoad = document.standardEdited = true;
        error.clear();
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool SceneDocumentService::CanUndo(const std::string& path, bool redo) const
{
    try
    {
        const auto key = Normalize(path);
        const auto document = documents.find(key);
        const auto history = histories.find(key);
        return document != documents.end() && history != histories.end() &&
            history->second.current == document->second.draft &&
            !(redo ? history->second.redo : history->second.undo).empty();
    }
    catch (...) { return false; }
}

bool SceneDocumentService::Undo(const std::string& path, bool redo, std::string& error)
{
    try
    {
        if (!CanUndo(path, redo)) throw std::invalid_argument("No scene edit available");
        const auto key = Normalize(path);
        auto& document = documents.at(key);
        auto& history = histories.at(key);
        auto& source = redo ? history.redo : history.undo;
        auto& destination = redo ? history.undo : history.redo;
        auto candidate = source.back();
        Validate(candidate, key);
        destination.push_back(document.draft);
        PublishDraft(document, std::move(candidate));
        source.pop_back();
        history.current = document.draft;
        document.edits.clear();
        document.replayViaDraft = document.awaitingWorldLoad = document.standardEdited = true;
        error.clear();
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool SceneDocumentService::CreateScriptBlueprintAsset(const std::string& path,
    const nlohmann::json& actor, std::string& error)
{
    std::filesystem::path temporary;
    bool ownsTemporary = false;
    try
    {
        const auto target = BlueprintCreationTarget(resourceRoot, path);
        nlohmann::json asset{{"type", "blueprint"}, {"name", actor.at("name")},
            {"class", actor.at("class")}, {"properties", actor.at("properties")},
            {"components", actor.value("components", nlohmann::json::array())},
            {"rotation", actor.at("rotation")}, {"scale", actor.at("scale")}};
        if (actor.contains("physics")) asset["physics"] = actor.at("physics");
        if (actor.contains("scriptComponents")) asset["scriptComponents"] = actor.at("scriptComponents");
        asset["instanceEditable"] = actor.value("__instanceEditable",
            actor.value("instanceEditable", nlohmann::json::array()));
        asset["componentInstanceEditable"] = actor.value("__componentInstanceEditable",
            actor.value("componentInstanceEditable", nlohmann::json::object()));
        ValidateScriptBlueprintAsset(asset);
        // 与正式读取共用依赖边界；创建只发布新文件，绝不覆盖已有资产。
        std::filesystem::create_directories(target.parent_path());
        static std::atomic<uint64_t> next{0};
        temporary = target.parent_path() / (target.stem().string() + ".create-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++next) + ".json");
        if (std::filesystem::exists(temporary))
            throw std::runtime_error("Actor asset temporary file already exists");
        WriteTextFileAtomically(temporary, asset.dump(4) + "\n");
        ownsTemporary = true;
        // 先完整验证，再利用硬链接的 create-new 语义原子发布；失败保留原目标。
        ReadScriptBlueprintAsset(resourceRoot, temporary.lexically_relative(resourceRoot).generic_string());
        std::filesystem::create_hard_link(temporary, target);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        if (ownsTemporary)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        error = exception.what();
        return false;
    }
}

bool SceneDocumentService::CreateScriptBlueprintSource(const std::string& path,
    const std::string& className, const std::string& name,
    const nlohmann::json& registeredTypes, std::string& error)
{
    std::filesystem::path temporary, source;
    bool ownsTemporary = false, publishedSource = false;
    try
    {
        ValidateBlueprintSourceClass(className);
        if (name.empty() || name.find('\0') != std::string::npos)
            throw std::invalid_argument("New Blueprint requires a display name");
        for (const auto& type : registeredTypes)
            if (ScriptTypeNameMatches(type, className))
                throw std::invalid_argument("The C# type is already registered: " + className);
        // 文件发布负责拒绝覆盖；其它源码中的完整 C# 类型冲突由编译器统一诊断。
        const auto target = BlueprintCreationTarget(resourceRoot, path);
        // 创建和编译共用排除规则，不能发布一个后续永远不进入 Gameplay 的源码。
        for (const auto& part : target.parent_path().lexically_relative(
            std::filesystem::weakly_canonical(resourceRoot)))
            if (ScriptSourceDirectoryExcluded(part.string()))
                throw std::invalid_argument("Blueprint source cannot be created in an excluded C# directory");
        source = target.parent_path() / (target.stem().string() + ".blueprint.cs");
        if (std::filesystem::exists(target) || std::filesystem::exists(source))
            throw std::invalid_argument("The Blueprint asset or paired source already exists");
        const auto separator = className.rfind('.');
        const auto typeName = separator == std::string::npos ? className : className.substr(separator + 1);
        std::string text = "using VulkanLearn;\n\n";
        if (separator != std::string::npos) text += "namespace " + className.substr(0, separator) + ";\n\n";
        text += "public sealed class " + typeName + " : global::VulkanLearn.Blueprint\n{\n}\n";
        std::filesystem::create_directories(target.parent_path());
        // .tmp 不进入资源源码收集；完整写入后才以 create-new 硬链接发布。
        static std::atomic<uint64_t> nextSource{0};
        temporary = target.parent_path() / (target.stem().string() + ".create-source-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++nextSource) + ".tmp");
        if (std::filesystem::exists(temporary)) throw std::runtime_error("Blueprint source temporary already exists");
        WriteTextFileAtomically(temporary, text);
        ownsTemporary = true;
        std::filesystem::create_hard_link(temporary, source);
        publishedSource = true;
        const nlohmann::json declaration{{"name", name}, {"class", className},
            {"properties", nlohmann::json::object()}, {"components", nlohmann::json::array()},
            {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}};
        if (!CreateScriptBlueprintAsset(path, declaration, error)) throw std::runtime_error(error);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        // 仅回收本次发布且仍指向暂存 inode 的源码，不删除并发替换的外部文件。
        std::error_code ignored;
        if (publishedSource && std::filesystem::equivalent(temporary, source, ignored))
            std::filesystem::remove(source, ignored);
        if (ownsTemporary) std::filesystem::remove(temporary, ignored);
        error = exception.what();
        return false;
    }
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
    PublishSceneDraft(Normalize(actor.scenePath), document, std::move(candidate));
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
