#include "editor/ui/materialInstanceAssetEditorPanel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include "editor/particles/particleCurveEdit.h"
#include "editor/particles/particleEmitterEdit.h"
#include "editor/particles/particleFlipbookEdit.h"
#include "editor/particles/particleModuleEdit.h"
#include "editor/ui/panel/editorPanelInternalHelpers.h"
#include "editor/ui/panel/particlePanelHelpers.h"
#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi
{

using namespace Panel::Detail;
using namespace ParticlePanelDetail;

const EditorAssetDocumentResultPayload*
MaterialInstanceAssetEditorPanel::ParticleDocument(
    const std::string& path) const
{
    for (const auto& entry : snapshot.particleDocuments)
        if (entry.assetPath == path) return &entry;
    return nullptr;
}

void MaterialInstanceAssetEditorPanel::QueueParticleAssetDraft(
    EditorAssetFieldChange change)
{
    const auto* tab = resourceEditor.ActiveTab();
    const auto* authored = tab ? ParticleDocument(tab->path) : nullptr;
    if (!authored) return;
    if (!pendingParticlePath.empty() &&
        pendingParticlePath != tab->path)
    {
        if (particleCommandAwaitingSnapshot ||
            particleHistoryCommandAwaitingSnapshot)
        {
            localStatus =
                "The previous particle edit is still being applied.";
            return;
        }
        FlushParticleAssetEdits();
        if (!pendingParticlePath.empty()) return;
    }
    pendingParticlePath = tab->path;
    pendingParticleRevision = authored->documentRevision;
    for (auto& pending : pendingParticleChanges)
    {
        if (pending.field == change.field)
        {
            pending = std::move(change);
            return;
        }
    }
    pendingParticleChanges.push_back(std::move(change));
}

void MaterialInstanceAssetEditorPanel::FlushParticleAssetEdits()
{
    if (pendingParticlePath.empty() ||
        pendingParticleChanges.empty())
    {
        return;
    }
    // 在途期间只累积 pending；上一笔终态到达后再提交，避免用旧 revision
    // 排队和下一条命令互相覆盖。
    if (particleCommandAwaitingSnapshot ||
        particleHistoryCommandAwaitingSnapshot)
    {
        return;
    }
    ModifyAssetDocumentPayload payload;
    payload.assetPath = pendingParticlePath;
    std::vector<EditorAssetFieldChange> changes =
        std::move(pendingParticleChanges);
    inFlightParticleChanges = changes;
    payload.changes = std::move(changes);
    EditorCommandEnvelope command;
    command.type = EditorCommandType::ModifyParticleDocument;
    command.expectedDocumentRevision = pendingParticleRevision;
    command.payload = std::move(payload);
    Submit(std::move(command));
    particleCommandPath = pendingParticlePath;
    particleCommandRevision = pendingParticleRevision;
    pendingParticlePath.clear();
    pendingParticleChanges.clear();
    particleCommandAwaitingSnapshot = true;
}

void MaterialInstanceAssetEditorPanel::MergeParticleInFlightChanges()
{
    for (auto& change : inFlightParticleChanges)
    {
        const bool newerPending = std::any_of(
            pendingParticleChanges.begin(), pendingParticleChanges.end(),
            [&change](const EditorAssetFieldChange& pending)
            {
                return pending.field == change.field;
            });
        // pending 代表用户在旧命令之后的新输入；同字段以新值为准。
        if (!newerPending)
            pendingParticleChanges.push_back(std::move(change));
    }
    inFlightParticleChanges.clear();
    if (pendingParticlePath.empty() && !particleCommandPath.empty())
        pendingParticlePath = particleCommandPath;
    if (!pendingParticlePath.empty() &&
        pendingParticlePath == particleCommandPath)
    {
        if (const auto* authored = ParticleDocument(pendingParticlePath))
            pendingParticleRevision = authored->documentRevision;
    }
}

void MaterialInstanceAssetEditorPanel::SubmitParticleAssetCommand(
    EditorCommandType type,
    const std::string& path)
{
    const auto* authored = ParticleDocument(path);
    if (!authored) return;
    EditorCommandEnvelope command;
    command.type = type;
    command.expectedDocumentRevision = authored->documentRevision;
    if (type == EditorCommandType::ReloadParticleDocument)
        command.payload = ReloadMaterialInstanceDocumentPayload{
            path, EditorDirtyDocumentPolicy::RequireClean};
    else
        command.payload = MaterialInstanceAssetPathPayload{path};
    Submit(std::move(command));
}

bool MaterialInstanceAssetEditorPanel::RequestParticleHistory(bool redo)
{
    const auto* tab = resourceEditor.ActiveTab();
    if (!tab || tab->kind != EditorAssetKind::ParticleSystem ||
        !IsResourceEditorVisible())
        return false;
    const auto* authored = ParticleDocument(tab->path);
    if (!authored || HasModal() || particleCommandAwaitingSnapshot ||
        particleHistoryCommandAwaitingSnapshot ||
        !pendingParticlePath.empty() ||
        !pendingParticleChanges.empty() ||
        !(redo ? authored->canRedo : authored->canUndo))
        return true;
    SubmitParticleAssetCommand(
        redo ? EditorCommandType::RedoParticleDocument
             : EditorCommandType::UndoParticleDocument,
        tab->path);
    particleHistoryCommandAwaitingSnapshot = true;
    particleHistoryCommandPath = tab->path;
    particleHistoryCommandRevision = authored->documentRevision;
    for (const auto& feedback : snapshot.particleCommandFeedback)
        particleCommandFeedbackSequence = std::max(
            particleCommandFeedbackSequence, feedback.sequence);
    RefreshParticleHistoryControls();
    return true;
}

void MaterialInstanceAssetEditorPanel::RefreshParticleHistoryControls()
{
    if (document == nullptr) return;
    const auto* tab = resourceEditor.ActiveTab();
    if (!tab || tab->kind != EditorAssetKind::ParticleSystem ||
        !IsResourceEditorVisible())
        return;
    const auto* authored = ParticleDocument(tab->path);
    const bool enabled = authored != nullptr &&
        !particleCommandAwaitingSnapshot &&
        !particleHistoryCommandAwaitingSnapshot &&
        pendingParticlePath.empty() &&
        pendingParticleChanges.empty();
    SetDisabled("particle-undo", !enabled || !authored->canUndo);
    SetDisabled("particle-redo", !enabled || !authored->canRedo);
    SetDisabled("menu-edit-undo", !enabled || !authored->canUndo);
    SetDisabled("menu-edit-redo", !enabled || !authored->canRedo);
}

std::string
MaterialInstanceAssetEditorPanel::ParticleEmitterNameForField(
    const std::string& field) const
{
    if (field.rfind("emitters.", 0) != 0)
        return {};
    const size_t indexStart = std::string("emitters.").size();
    const size_t separator = field.find('.', indexStart);
    if (separator == std::string::npos || separator == indexStart)
        return {};
    size_t index = 0;
    for (size_t cursor = indexStart; cursor < separator; ++cursor)
    {
        const char character = field[cursor];
        if (character < '0' || character > '9') return {};
        index = index * 10 + static_cast<size_t>(character - '0');
    }
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active ? ParticleDocument(active->path) : nullptr;
    if (authored == nullptr || !authored->document.is_object())
        return {};
    const auto& document = authored->document;
    if (!document.contains("emitters") ||
        !document.at("emitters").is_array() ||
        index >= document.at("emitters").size())
        return {};
    return document.at("emitters").at(index).value(
        "name", "emitter" + std::to_string(index));
}

void MaterialInstanceAssetEditorPanel::RequestParticlePreview(
    const std::string& path)
{
    if (runtimeActionSink == nullptr) return;
    const bool updateInPlace = HasParticlePreview();
    if (!updateInPlace &&
        !runtimeView.activeWorldPath.empty() &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() !=
            "SC_particle_preview.json" &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() !=
            "SC_model_preview.json")
    {
        levelWorldPath = runtimeView.activeWorldPath;
    }
    requestedPreviewPath = path;
    requestedPreviewMaterial = false;
    requestedPreviewBlueprint = false;
    requestedPreviewParticle = true;
    const auto* authored = ParticleDocument(path);
    UiAction request;
    request.type = updateInPlace
        ? UiActionType::UpdateParticlePreview
        : UiActionType::PreviewParticle;
    request.stringValue = path;
    if (authored != nullptr && authored->document.is_object())
        request.particleDocument = authored->document;
    runtimeActionSink->RequestRuntimeAction(std::move(request));
}

bool MaterialInstanceAssetEditorPanel::HasParticlePreview() const
{
    const auto* active = resourceEditor.ActiveTab();
    return requestedPreviewParticle &&
        active != nullptr &&
        active->kind == EditorAssetKind::ParticleSystem &&
        active->path == requestedPreviewPath &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() ==
            "SC_particle_preview.json";
}

std::string
MaterialInstanceAssetEditorPanel::ParticleModuleStackMarkup() const
{
    using UiWidgets::Text;
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active
        ? ParticleDocument(active->path) : nullptr;
    if (authored == nullptr || !authored->document.is_object())
        return std::string();
    const nlohmann::json& document = authored->document;
    const bool v2 = document.value("version", 1) == 2;
    if (!v2)
    {
        return "<div class=\"particle-module-stack\">"
            "<div class=\"particle-field-row\"><span>Module stack</span>"
            "<button data-action=\"particle-upgrade-v2\">"
            "Upgrade to v2 module stack</button></div></div>";
    }

    const bool multiEmitter =
        document.contains("emitters") &&
        document.at("emitters").is_array() &&
        !document.at("emitters").empty();
    const size_t emitterIndex = multiEmitter
        ? std::min(particleEmitterIndex,
            document.at("emitters").size() - 1)
        : 0;
    const nlohmann::json& emitter = multiEmitter
        ? document.at("emitters").at(emitterIndex)
        : document;
    const nlohmann::json modules = emitter.contains("modules") &&
        emitter.at("modules").is_array()
        ? emitter.at("modules") : nlohmann::json::array();

    std::string markup = "<div class=\"particle-module-stack\">";
    markup += "<div class=\"particle-field-row\"><span>Module stack</span>"
        "<span class=\"particle-module-count\">" +
        std::to_string(modules.size()) + " modules</span></div>";
    if (modules.empty())
    {
        markup += "<p class=\"muted\">No modules. Add one below.</p>";
    }
    const size_t selected = modules.empty() ? 0 :
        std::min(particleModuleStackIndex, modules.size() - 1);
    for (size_t index = 0; index < modules.size(); ++index)
    {
        const nlohmann::json& module = modules.at(index);
        const std::string type = module.value(
            "type", std::string());
        const ParticleModuleSpec* spec =
            FindParticleModuleSpec(type);
        const std::string label = spec != nullptr
            ? spec->label : type;
        const bool enabled = module.value("enabled", true);
        markup += "<div class=\"particle-field-row particle-module-row" +
            std::string(index == selected ? " selected" : "") + "\">";
        markup += "<button data-action=\"particle-module-stack-select\""
            " data-index=\"" + std::to_string(index) + "\">" +
            Text(std::to_string(index) + ". " + label) + "</button>";
        markup += "<span>" +
            std::string(enabled ? "enabled" : "disabled") + "</span>";
        markup += "<button data-action=\"particle-module-stack-toggle\""
            " data-index=\"" + std::to_string(index) + "\">" +
            std::string(enabled ? "Disable" : "Enable") + "</button>";
        markup += "<button data-action=\"particle-module-stack-up\""
            " data-index=\"" + std::to_string(index) + "\">Up</button>";
        markup += "<button data-action=\"particle-module-stack-down\""
            " data-index=\"" + std::to_string(index) + "\">Down</button>";
        markup += "<button data-action=\"particle-module-stack-remove\""
            " data-index=\"" + std::to_string(index) + "\">Remove</button>";
        markup += "</div>";
    }

    std::string options;
    for (const ParticleModuleSpec& spec :
        ParticleModuleSpecs())
    {
        options += "<option value=\"" + Text(spec.type) + "\">" +
            Text(spec.label) + "</option>";
    }
    markup += "<div class=\"particle-field-row\"><span>Add module</span>"
        "<select id=\"particle-module-type\">" + options + "</select>"
        "<button data-action=\"particle-module-stack-add\">Add</button>"
        "</div></div>";
    return markup;
}

std::string MaterialInstanceAssetEditorPanel::ParticleFieldsMarkup() const
{
    using UiWidgets::Text;
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active
        ? ParticleDocument(active->path) : nullptr;
    if (authored == nullptr)
    {
        return "<p class=\"muted\">Reading particle asset...</p>";
    }
    std::string markup;
    const nlohmann::json& document = authored->document;
    const bool multiEmitter =
        document.contains("emitters") &&
        document.at("emitters").is_array() &&
        !document.at("emitters").empty();
    const size_t emitterIndex = multiEmitter
        ? std::min(particleEmitterIndex,
            document.at("emitters").size() - 1)
        : 0;
    const nlohmann::json& emitter = multiEmitter
        ? document.at("emitters").at(emitterIndex)
        : document;
    const nlohmann::json flattenedEmitter =
        ParticleEmitterForEdit(emitter);
    if (particleModule == "system")
    {
        std::string systemMarkup =
            "<div class=\"particle-field-row\"><span>Name</span>"
            "<input type=\"text\" data-action=\"particle-field\""
            " data-field=\"name\" data-value-type=\"string\" value=\"" +
            Text(document.value("name", std::string())) +
            "\"/></div>";
        if (multiEmitter)
            systemMarkup += "<p class=\"particle-system-summary\">" +
                std::to_string(document.at("emitters").size()) +
                " emitters</p>";
        if (document.value("version", 1) == 2)
        {
            // PS 根 userParameters 是 v2 用户参数表（最多 64 个有限数值）。
            // 当前面板没有通用键值行控件，用单个 JSON 对象文本框显式编辑，
            // 不把该字段静默隐藏；提交边界仍由 DecodeValue 校验。
            const nlohmann::json parameters = document.value(
                "userParameters", nlohmann::json::object());
            systemMarkup +=
                "<div class=\"particle-field-row\"><span>userParameters"
                "</span><input type=\"text\" data-action=\"particle-field\""
                " data-field=\"userParameters\" data-value-type=\"object\""
                " value=\"" + Text(parameters.dump()) + "\"/>"
                "<button class=\"particle-field-reset\""
                " data-action=\"particle-field-reset\""
                " data-field=\"userParameters\">Reset</button></div>"
                "<p class=\"muted\">JSON object, numeric finite values, "
                "max 64 entries.</p>";
        }
        systemMarkup += ParticleModuleStackMarkup();
        return systemMarkup;
    }
    if (particleModule == "curves")
    {
        // Curves 模块只编辑两条生命周期曲线；结构化控件替代 JSON 文本框，
        // 但提交时仍把整条 key 数组当作一个字段写回同一份作者文档。
        const std::string prefix = multiEmitter
            ? "emitters." + std::to_string(emitterIndex) + "."
            : std::string();
        std::string curveMarkup = ParticleModuleStackMarkup();
        const ParticleCurveInterpolation interpolation =
            ParticleCurveEdit::ReadInterpolation(flattenedEmitter);
        curveMarkup += ParticleCurveMarkup(
            ParticleCurveKeys(flattenedEmitter, "colorKeys",
                prefix + "colorKeys"),
            "colorKeys",
            interpolation);
        curveMarkup += ParticleCurveMarkup(
            ParticleCurveKeys(flattenedEmitter, "sizeKeys",
                prefix + "sizeKeys"),
            "sizeKeys",
            interpolation);
        const std::string interpolationField =
            prefix + "curveInterpolation";
        curveMarkup +=
            "<div class=\"particle-field-row\"><span>"
            "curveInterpolation</span>"
            "<input type=\"text\" data-action=\"particle-field\""
            " data-field=\"" + Text(interpolationField) +
            "\" data-value-type=\"string\" value=\"" +
            Text(ParticleCurveInterpolationName(interpolation)) +
            "\"/><button class=\"particle-field-reset\""
            " data-action=\"particle-field-reset\" data-field=\"" +
            Text(interpolationField) +
            "\">Reset</button></div>"
            "<p class=\"muted\">linear | hermite | bezier; tangentIn/"
            "tangentOut are saved inside each key.</p>";
        return curveMarkup;
    }
    // 模块栈放在字段表单最上方：v1 显示升级入口，v2 显示结构操作。
    markup += ParticleModuleStackMarkup();
    nlohmann::json fields = authored->capabilities.value(
        "fields", nlohmann::json::array());
    if (multiEmitter)
    {
        const auto& emitters = authored->capabilities.value(
            "emitters", nlohmann::json::array());
        if (emitterIndex < emitters.size())
            fields = emitters.at(emitterIndex).value(
                "fields", nlohmann::json::array());
    }
    for (const auto& field : fields)
    {
        const std::string name = field.value("field", std::string());
        if (!ParticleFieldInModule(particleModule, name)) continue;
        // 路径只在对应碰撞类型下有作者意义；保留能力表字段以便切换
        // 类型后立即出现，但默认不把无关路径显示成可编辑行。
        if (name == "collision.sdf.path" ||
            name == "collision.mesh.path")
        {
            std::string collisionType = "none";
            if (flattenedEmitter.contains("collision") &&
                flattenedEmitter.at("collision").is_object())
            {
                collisionType = flattenedEmitter.at("collision").value(
                    "type", std::string("none"));
            }
            const bool meshPath = name == "collision.mesh.path";
            if (collisionType != (meshPath ? "mesh" : "sdf")) continue;
        }
        // 逐帧表由专用表格编辑；跳过 JSON 文本框，避免两套控件写同一字段。
        if (particleModule == "flipbook" &&
            (name == "flipbook.frameDurations" ||
             name == "flipbook.framePivots" ||
             name == "flipbook.frameUVs" ||
             name == "flipbook.frameRotations"))
        {
            continue;
        }
        // 隐式发射器没有 localPosition；不要把无法写回的 position 暴露给旧资产。
        if (name == "position" && !multiEmitter) continue;
        const std::string type = field.value("type", std::string());
        nlohmann::json current = flattenedEmitter;
        size_t start = 0;
        while (!name.empty())
        {
            const size_t separator = name.find('.', start);
            const bool last = separator == std::string::npos;
            const std::string segment = name.substr(
                start, last ? std::string::npos : separator - start);
            if (!current.is_object() || !current.contains(segment))
            {
                current = nlohmann::json();
                break;
            }
            current = current.at(segment);
            if (last) break;
            start = separator + 1;
        }
        // fixedBounds 存在但 useFixedBounds 省略时，加载器会自动启用。
        // 表单必须显示生效值，否则作者会误以为固定包围球处于关闭状态。
        if (name == "useFixedBounds" &&
            !flattenedEmitter.contains("useFixedBounds") &&
            flattenedEmitter.contains("fixedBounds"))
        {
            current = true;
        }
        if (name == "mesh.materialInstancePath" && current.is_null() &&
            flattenedEmitter.contains("materialInstancePath"))
        {
            current = flattenedEmitter.at("materialInstancePath");
        }
        const std::string fieldPath = multiEmitter
            ? "emitters." + std::to_string(emitterIndex) + "." + name
            : name;
        const std::string escapedName = Text(fieldPath);
        markup += "<div class=\"particle-field-row\"><span>" +
            Text(name) + "</span>";
        if (type == "boolean")
        {
            const bool checked = current.is_boolean() &&
                current.get<bool>();
            markup += "<select data-action=\"particle-field\" data-field=\"" +
                escapedName + "\" data-value-type=\"boolean\">" +
                std::string("<option value=\"false\"") +
                (checked ? "" : " selected") + ">false</option>" +
                std::string("<option value=\"true\"") +
                (checked ? " selected" : "") + ">true</option></select>";
        }
        else
        {
            const std::string text = current.is_string()
                ? current.get<std::string>()
                : current.is_null() ? std::string() : current.dump();
            markup += "<input type=\"text\" data-action=\"particle-field\" data-field=\"" +
                escapedName + "\" data-value-type=\"" + Text(type) +
                "\" value=\"" + Text(text) + "\"/>";
        }
        if (field.value("resettable", false))
        {
            markup += "<button class=\"particle-field-reset\""
                " data-action=\"particle-field-reset\""
                " data-field=\"" + escapedName +
                "\">Reset</button>";
        }
        markup += "</div>";
    }
    if (particleModule == "flipbook")
        markup += ParticleFlipbookTableMarkup();
    if (markup.empty())
    {
        markup = "<p class=\"muted\">No editable fields in this module.</p>";
    }
    return markup;
}

std::string
MaterialInstanceAssetEditorPanel::ParticleFlipbookTableMarkup() const
{
    using namespace ParticleFlipbookEdit;
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active
        ? ParticleDocument(active->path) : nullptr;
    if (authored == nullptr) return std::string();

    // 表格要立即反映同一帧里刚进入 pending 或仍在途的数组，
    // 不能等命令终态后才显示用户刚才输入的值。
    nlohmann::json working = authored->document;
    if (particleCommandPath == active->path)
    {
        for (const auto& change : inFlightParticleChanges)
            ApplyParticleDraftChange(working, change);
    }
    if (pendingParticlePath == active->path)
    {
        for (const auto& change : pendingParticleChanges)
            ApplyParticleDraftChange(working, change);
    }

    const bool multiEmitter =
        working.contains("emitters") &&
        working.at("emitters").is_array() &&
        !working.at("emitters").empty();
    const size_t emitterIndex = multiEmitter
        ? std::min(particleEmitterIndex,
            working.at("emitters").size() - 1)
        : 0;
    const nlohmann::json& rawEmitter = multiEmitter
        ? working.at("emitters").at(emitterIndex)
        : working;
    // v2 的 flipbook 字段位于模块内，表格必须读取展平后的投影。
    const nlohmann::json emitter = ParticleEmitterForEdit(rawEmitter);

    const size_t frameCount = FlipbookFrameCount(emitter);
    std::string markup =
        "<div class=\"particle-flipbook-table\">"
        "<div class=\"particle-flipbook-header\"><span>Per-frame timing "
        "and pivot</span><span class=\"particle-flipbook-count\">" +
        std::to_string(frameCount) + " frames</span></div>"
        "<p class=\"particle-flipbook-hint\">Duration is a relative weight "
        "&gt; 0; pivot is the sprite anchor in [0,1]; UV rect is the "
        "sprite-space sub-rectangle (u0,v0,u1,v1).</p>";

    const bool validFrameCount =
        frameCount > 0 && frameCount <= kMaxFrames;
    if (!validFrameCount)
    {
        markup += "<p class=\"particle-flipbook-warning\">Flipbook table "
            "requires a frame range of 1..256 frames; current range is " +
            std::to_string(frameCount) + ".</p></div>";
        return markup;
    }

    const nlohmann::json durations =
        EnsureFlipbookDurations(emitter);
    const nlohmann::json pivots =
        EnsureFlipbookPivots(emitter);
    // UV rect 在 UBO 中每帧一个 vec4，只支持 64 帧以内的区间；
    // 更长区间继续显示时长/轴心表，但隐藏 UV 列以免写出会被拒绝的数据。
    const bool uvEditable = frameCount <= kMaxUvFrames;
    const nlohmann::json uvs = uvEditable
        ? EnsureFlipbookUVs(emitter)
        : nlohmann::json();
    // 逐帧旋转每 vec4 打包 4 帧，同样是 64 帧上限。
    const bool rotationEditable =
        frameCount <= kMaxRotationFrames;
    const nlohmann::json rotations = rotationEditable
        ? EnsureFlipbookRotations(emitter)
        : nlohmann::json();
    if (!durations.is_array() || !pivots.is_array() ||
        (uvEditable && !uvs.is_array()) ||
        (rotationEditable && !rotations.is_array()))
    {
        markup += "<p class=\"particle-flipbook-warning\">The current "
            "frame range cannot be edited here.</p></div>";
        return markup;
    }

    markup += "<div class=\"particle-flipbook-actions\">"
        "<button data-action=\"particle-flipbook-seed-durations\">"
        "Seed durations</button>"
        "<button data-action=\"particle-flipbook-uniform-pivots\">"
        "Uniform pivots</button>"
        "<button data-action=\"particle-flipbook-reset-durations\">"
        "Reset durations</button>"
        "<button data-action=\"particle-flipbook-reset-pivots\">"
        "Reset pivots</button>";
    if (uvEditable)
    {
        markup +=
            "<button data-action=\"particle-flipbook-seed-uvs\">"
            "Seed UV rects</button>"
            "<button data-action=\"particle-flipbook-reset-uvs\">"
            "Reset UV rects</button>";
    }
    if (rotationEditable)
    {
        markup +=
            "<button data-action=\"particle-flipbook-seed-rotations\">"
            "Seed rotations</button>"
            "<button data-action=\"particle-flipbook-reset-rotations\">"
            "Reset rotations</button>";
    }
    markup += "</div>";
    if (!uvEditable || !rotationEditable)
    {
        markup += "<p class=\"particle-flipbook-hint\">UV rects and "
            "per-frame rotations are available for frame ranges of at "
            "most 64 frames.</p>";
    }
    markup += "<div class=\"particle-flipbook-scroll\">"
        "<div class=\"particle-flipbook-columns\">"
        "<span>Frame</span><span>Duration</span><span>Pivot U</span>"
        "<span>Pivot V</span>";
    if (uvEditable)
    {
        markup += "<span>U0</span><span>V0</span>"
            "<span>U1</span><span>V1</span>";
    }
    if (rotationEditable)
        markup += "<span>Rot</span>";
    markup += "</div>";

    size_t startFrame = 0;
    if (emitter.contains("flipbook") &&
        emitter.at("flipbook").is_object())
    {
        const nlohmann::json& flipbook = emitter.at("flipbook");
        if (flipbook.contains("startFrame") &&
            flipbook.at("startFrame").is_number())
        {
            const double value =
                flipbook.at("startFrame").get<double>();
            if (std::isfinite(value) && value >= 0.0 &&
                std::floor(value) == value)
            {
                startFrame = static_cast<size_t>(value);
            }
        }
    }
    for (size_t index = 0; index < frameCount; ++index)
    {
        const float duration =
            durations.at(index).get<float>();
        const float u = pivots.at(index).at(0).get<float>();
        const float v = pivots.at(index).at(1).get<float>();
        const float u0 = uvEditable
            ? uvs.at(index).at(0).get<float>() : 0.0f;
        const float v0 = uvEditable
            ? uvs.at(index).at(1).get<float>() : 0.0f;
        const float u1 = uvEditable
            ? uvs.at(index).at(2).get<float>() : 1.0f;
        const float v1 = uvEditable
            ? uvs.at(index).at(3).get<float>() : 1.0f;
        const float rotation = rotationEditable
            ? rotations.at(index).get<float>() : 0.0f;
        const std::string rowIndex = std::to_string(index);
        markup += "<div class=\"particle-flipbook-row\">"
            "<span class=\"particle-flipbook-frame\">" +
            std::to_string(startFrame + index) + "</span>"
            "<input type=\"text\" data-action=\"particle-flipbook-duration\""
            " data-index=\"" + rowIndex + "\" data-value-type=\"number\""
            " value=\"" + ParticleNumber(duration) + "\"/>"
            "<input type=\"text\" data-action=\"particle-flipbook-pivot\""
            " data-index=\"" + rowIndex + "\" data-component=\"u\""
            " data-value-type=\"number\" value=\"" + ParticleNumber(u) + "\"/>"
            "<input type=\"text\" data-action=\"particle-flipbook-pivot\""
            " data-index=\"" + rowIndex + "\" data-component=\"v\""
            " data-value-type=\"number\" value=\"" + ParticleNumber(v) + "\"/>";
        if (uvEditable)
        {
            markup +=
                "<input type=\"text\" data-action=\"particle-flipbook-uv\""
                " data-index=\"" + rowIndex + "\" data-component=\"u0\""
                " data-value-type=\"number\" value=\"" + ParticleNumber(u0) + "\"/>"
                "<input type=\"text\" data-action=\"particle-flipbook-uv\""
                " data-index=\"" + rowIndex + "\" data-component=\"v0\""
                " data-value-type=\"number\" value=\"" + ParticleNumber(v0) + "\"/>"
                "<input type=\"text\" data-action=\"particle-flipbook-uv\""
                " data-index=\"" + rowIndex + "\" data-component=\"u1\""
                " data-value-type=\"number\" value=\"" + ParticleNumber(u1) + "\"/>"
                "<input type=\"text\" data-action=\"particle-flipbook-uv\""
                " data-index=\"" + rowIndex + "\" data-component=\"v1\""
                " data-value-type=\"number\" value=\"" + ParticleNumber(v1) + "\"/>";
        }
        if (rotationEditable)
        {
            markup +=
                "<input type=\"text\""
                " data-action=\"particle-flipbook-rotation\""
                " data-index=\"" + rowIndex +
                "\" data-value-type=\"number\" value=\"" +
                ParticleNumber(rotation) + "\"/>";
        }
        markup += "</div>";
    }
    markup += "</div></div>";
    return markup;
}

void MaterialInstanceAssetEditorPanel::HandleParticleFlipbookTableEdit(
    Rml::Element& element)
{
    using namespace ParticleFlipbookEdit;
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active
        ? ParticleDocument(active->path) : nullptr;
    if (authored == nullptr) return;

    nlohmann::json working = authored->document;
    if (particleCommandPath == active->path)
    {
        for (const auto& change : inFlightParticleChanges)
            ApplyParticleDraftChange(working, change);
    }
    if (pendingParticlePath == active->path)
    {
        for (const auto& change : pendingParticleChanges)
            ApplyParticleDraftChange(working, change);
    }
    const bool multiEmitter =
        working.contains("emitters") &&
        working.at("emitters").is_array() &&
        !working.at("emitters").empty();
    const size_t emitterIndex = multiEmitter
        ? std::min(particleEmitterIndex,
            working.at("emitters").size() - 1)
        : 0;
    const nlohmann::json& rawEmitter = multiEmitter
        ? working.at("emitters").at(emitterIndex)
        : working;
    const nlohmann::json emitter =
        ParticleEmitterForEdit(rawEmitter);
    const size_t frameCount = FlipbookFrameCount(emitter);
    if (frameCount == 0 || frameCount > kMaxFrames)
    {
        localStatus =
            "A flipbook table requires a frame range of 1..256 frames.";
        return;
    }

    const int indexValue =
        element.GetAttribute<int>("data-index", -1);
    if (indexValue < 0 ||
        static_cast<size_t>(indexValue) >= frameCount)
    {
        return;
    }
    const size_t index = static_cast<size_t>(indexValue);
    float value = 0.0f;
    if (!ParseNumber(ControlValue(element), value))
    {
        localStatus = "A complete finite number is required.";
        return;
    }

    const std::string prefix = multiEmitter
        ? "emitters." + std::to_string(emitterIndex) + "."
        : std::string();
    const std::string action = element.GetAttribute<std::string>(
        "data-action", "");
    EditorAssetFieldChange change;
    if (action == "particle-flipbook-duration")
    {
        nlohmann::json durations =
            EnsureFlipbookDurations(emitter);
        if (!SetFlipbookDuration(durations, index, value))
        {
            localStatus = "Frame duration must be finite and greater than 0.";
            return;
        }
        change.field = prefix + "flipbook.frameDurations";
        change.value = std::move(durations);
    }
    else if (action == "particle-flipbook-pivot")
    {
        nlohmann::json pivots =
            EnsureFlipbookPivots(emitter);
        const std::string component =
            element.GetAttribute<std::string>("data-component", "");
        if (component.size() != 1 ||
            !SetFlipbookPivot(pivots, index, component[0], value))
        {
            localStatus = "Pivot components must be finite numbers.";
            return;
        }
        change.field = prefix + "flipbook.framePivots";
        change.value = std::move(pivots);
    }
    else if (action == "particle-flipbook-uv")
    {
        nlohmann::json uvs =
            EnsureFlipbookUVs(emitter);
        const std::string component =
            element.GetAttribute<std::string>("data-component", "");
        if (component.empty() ||
            !SetFlipbookUV(uvs, index, component, value))
        {
            localStatus =
                "UV rect requires 0 <= u0 < u1 <= 1 and "
                "0 <= v0 < v1 <= 1.";
            return;
        }
        change.field = prefix + "flipbook.frameUVs";
        change.value = std::move(uvs);
    }
    else if (action == "particle-flipbook-rotation")
    {
        nlohmann::json rotations =
            EnsureFlipbookRotations(emitter);
        if (!SetFlipbookRotation(rotations, index, value))
        {
            localStatus =
                "Frame rotation must be a complete finite number.";
            return;
        }
        change.field = prefix + "flipbook.frameRotations";
        change.value = std::move(rotations);
    }
    else
    {
        return;
    }
    const std::string emitterName =
        ParticleEmitterNameForField(change.field);
    if (!emitterName.empty())
        change.expectedEmitterName = emitterName;
    QueueParticleAssetDraft(std::move(change));
    localStatus.clear();
}

void MaterialInstanceAssetEditorPanel::HandleParticleFlipbookTableAction(
    const std::string& action)
{
    using namespace ParticleFlipbookEdit;
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active
        ? ParticleDocument(active->path) : nullptr;
    if (authored == nullptr) return;

    nlohmann::json working = authored->document;
    if (particleCommandPath == active->path)
    {
        for (const auto& change : inFlightParticleChanges)
            ApplyParticleDraftChange(working, change);
    }
    if (pendingParticlePath == active->path)
    {
        for (const auto& change : pendingParticleChanges)
            ApplyParticleDraftChange(working, change);
    }
    const bool multiEmitter =
        working.contains("emitters") &&
        working.at("emitters").is_array() &&
        !working.at("emitters").empty();
    const size_t emitterIndex = multiEmitter
        ? std::min(particleEmitterIndex,
            working.at("emitters").size() - 1)
        : 0;
    const nlohmann::json& rawEmitter = multiEmitter
        ? working.at("emitters").at(emitterIndex)
        : working;
    const nlohmann::json emitter =
        ParticleEmitterForEdit(rawEmitter);
    const size_t frameCount = FlipbookFrameCount(emitter);
    if (frameCount == 0 || frameCount > kMaxFrames)
    {
        localStatus =
            "A flipbook table requires a frame range of 1..256 frames.";
        return;
    }

    const std::string prefix = multiEmitter
        ? "emitters." + std::to_string(emitterIndex) + "."
        : std::string();
    EditorAssetFieldChange change;
    if (action == "particle-flipbook-seed-durations")
    {
        change.field = prefix + "flipbook.frameDurations";
        change.value = UniformFlipbookDurations(frameCount, 1.0f);
        localStatus = "Uniform frame durations queued.";
    }
    else if (action == "particle-flipbook-uniform-pivots")
    {
        change.field = prefix + "flipbook.framePivots";
        change.value = UniformFlipbookPivots(
            frameCount, 0.5f, 0.5f);
        localStatus = "Uniform frame pivots queued.";
    }
    else if (action == "particle-flipbook-reset-durations")
    {
        change.field = prefix + "flipbook.frameDurations";
        change.reset = true;
        localStatus = "Frame durations reset queued.";
    }
    else if (action == "particle-flipbook-reset-pivots")
    {
        change.field = prefix + "flipbook.framePivots";
        change.reset = true;
        localStatus = "Frame pivots reset queued.";
    }
    else if (action == "particle-flipbook-seed-uvs")
    {
        change.field = prefix + "flipbook.frameUVs";
        change.value = UniformFlipbookUVs(
            frameCount, 0.0f, 0.0f, 1.0f, 1.0f);
        localStatus = "UV rects queued.";
    }
    else if (action == "particle-flipbook-reset-uvs")
    {
        change.field = prefix + "flipbook.frameUVs";
        change.reset = true;
        localStatus = "UV rects reset queued.";
    }
    else if (action == "particle-flipbook-seed-rotations")
    {
        change.field = prefix + "flipbook.frameRotations";
        change.value = UniformFlipbookRotations(frameCount, 0.0f);
        localStatus = "Frame rotations queued.";
    }
    else if (action == "particle-flipbook-reset-rotations")
    {
        change.field = prefix + "flipbook.frameRotations";
        change.reset = true;
        localStatus = "Frame rotations reset queued.";
    }
    else
    {
        return;
    }
    const auto* value =
        std::get_if<nlohmann::json>(&change.value);
    if (!change.reset &&
        (value == nullptr || !value->is_array() || value->empty()))
    {
        localStatus = "The flipbook frame table could not be created.";
        return;
    }
    const std::string emitterName =
        ParticleEmitterNameForField(change.field);
    if (!emitterName.empty())
        change.expectedEmitterName = emitterName;
    QueueParticleAssetDraft(std::move(change));
}

std::string MaterialInstanceAssetEditorPanel::ParticleCurveMarkup(
    const nlohmann::json& keys,
    const std::string& curve,
    ParticleCurveInterpolation interpolation) const
{
    using namespace ParticleCurveEdit;
    const bool colorCurve = curve == "colorKeys";
    std::string markup = "<div class=\"particle-curve\">";
    markup += "<div class=\"particle-curve-header\"><span>" +
        std::string(colorCurve ? "Color over life" : "Size over life") +
        "</span><button data-action=\"particle-curve-add\""
        " data-curve=\"" + curve + "\">+ Key</button></div>";
    markup += "<p class=\"particle-curve-hint\">Time is normalized "
        "lifetime [0,1]; 1-8 keys.</p>";

    // 轨道只把 key 的时间投影成百分比位置；容器宽度固定，标记点自身宽度固定，
    // 因此不需要百分比 max-width，也不会在收缩布局里退化成 0 宽。
    markup += "<div class=\"particle-curve-track\">";
    for (size_t index = 0; index < keys.size(); ++index)
    {
        const nlohmann::json& key = keys.at(index);
        float time = key.is_object()
            ? key.value("time", 0.0f) : 0.0f;
        if (!std::isfinite(time)) time = 0.0f;
        time = std::min(1.0f, std::max(0.0f, time));
        // 2%..98% 的内缩映射给 8px marker 留出左右半宽，
        // 避免 time=1 时整个标记落到轨道之外；映射仍对 time 单调。
        std::string style = "left: " +
            ParticleNumber(2.0f + time * 96.0f) + "%;";
        if (colorCurve)
        {
            style += " background-color: " +
                ColorHex(SampleColor(keys, time, interpolation)) + ";";
        }
        markup += "<span class=\"particle-curve-marker\" style=\"" +
            style + "\"></span>";
    }
    markup += "</div>";

    if (colorCurve)
    {
        markup += "<div class=\"particle-curve-swatches\">";
        const float divisor = kPreviewSamples > 1
            ? static_cast<float>(kPreviewSamples - 1) : 1.0f;
        for (size_t sample = 0; sample < kPreviewSamples; ++sample)
        {
            const float time = kPreviewSamples > 1
                ? static_cast<float>(sample) / divisor : 0.0f;
            markup += "<span class=\"particle-curve-swatch\""
                " style=\"background-color: " +
                ColorHex(SampleColor(keys, time, interpolation)) +
                ";\"></span>";
        }
        markup += "</div>";
    }
    else
    {
        std::array<float, kPreviewSamples> samples{};
        float maximum = 0.0f;
        const float divisor = kPreviewSamples > 1
            ? static_cast<float>(kPreviewSamples - 1) : 1.0f;
        for (size_t sample = 0; sample < kPreviewSamples; ++sample)
        {
            const float time = kPreviewSamples > 1
                ? static_cast<float>(sample) / divisor : 0.0f;
            samples[sample] = SampleSize(keys, time, interpolation);
            maximum = std::max(maximum, samples[sample]);
        }
        markup += "<div class=\"particle-curve-bars\">";
        for (float sample : samples)
        {
            const float height = maximum > 0.0f
                ? sample / maximum * 100.0f : 0.0f;
            markup += "<span class=\"particle-curve-bar\""
                " style=\"height: " + ParticleNumber(height) +
                "%;\"></span>";
        }
        markup += "</div>";
    }

    static constexpr const char* channelNames[] = {"R", "G", "B", "A"};
    for (size_t index = 0; index < keys.size(); ++index)
    {
        const nlohmann::json& key = keys.at(index);
        float time = key.is_object()
            ? key.value("time", 0.0f) : 0.0f;
        if (!std::isfinite(time)) time = 0.0f;
        time = std::min(1.0f, std::max(0.0f, time));
        markup += "<div class=\"particle-curve-key-row\">";
        markup += "<span class=\"particle-curve-key-label\">Key " +
            std::to_string(index + 1) + "</span>";
        markup += ParticleCurveInput(
            curve, index, "time", time);
        if (colorCurve)
        {
            const std::array<float, 4> color =
                SampleColor(keys, time, interpolation);
            for (size_t channel = 0; channel < 4; ++channel)
            {
                markup += "<span class=\"particle-curve-channel\">" +
                    std::string(channelNames[channel]) + "</span>";
                markup += ParticleCurveInput(
                    curve, index, channelNames[channel],
                    color[channel]);
            }
        }
        else
        {
            const float size = key.is_object()
                ? key.value("size", 0.0f) : 0.0f;
            markup += ParticleCurveInput(
                curve, index, "size",
                std::isfinite(size) ? std::max(0.0f, size) : 0.0f);
        }
        markup += "<button class=\"particle-curve-remove\""
            " data-action=\"particle-curve-remove\" data-curve=\"" +
            curve + "\" data-index=\"" + std::to_string(index) +
            "\">Remove</button>";
        markup += "</div>";
    }
    markup += "</div>";
    return markup;
}

nlohmann::json MaterialInstanceAssetEditorPanel::ParticleCurveKeys(
    const nlohmann::json& emitter,
    const std::string& curve,
    const std::string& fieldPath) const
{
    const bool colorCurve = curve == "colorKeys";
    // v2 的曲线字段在模块内；先展平，pending 分支与端点回退都读同一投影。
    const nlohmann::json editEmitter =
        ParticleEmitterForEdit(emitter);
    const auto* active = resourceEditor.ActiveTab();
    if (active != nullptr && pendingParticlePath == active->path)
    {
        for (const auto& pending : pendingParticleChanges)
        {
            if (pending.field != fieldPath) continue;
            if (pending.reset)
            {
                // 删除到空数组时字段写 reset；预览必须回到端点值，
                // 不能继续显示当前文档里即将被移除的那条曲线。
                nlohmann::json fallback = editEmitter;
                if (fallback.is_object())
                    fallback.erase(colorCurve
                        ? "colorKeys" : "sizeKeys");
                return colorCurve
                    ? ParticleCurveEdit::ReadColorKeys(fallback)
                    : ParticleCurveEdit::ReadSizeKeys(fallback);
            }
            const auto* value = std::get_if<nlohmann::json>(
                &pending.value);
            if (value != nullptr && value->is_array())
                return *value;
        }
    }
    return colorCurve
        ? ParticleCurveEdit::ReadColorKeys(editEmitter)
        : ParticleCurveEdit::ReadSizeKeys(editEmitter);
}

const nlohmann::json*
MaterialInstanceAssetEditorPanel::ActiveParticleEmitter(
    std::string& fieldPrefix) const
{
    fieldPrefix.clear();
    const auto* active = resourceEditor.ActiveTab();
    if (active == nullptr ||
        active->kind != EditorAssetKind::ParticleSystem)
    {
        return nullptr;
    }
    const auto* authored = ParticleDocument(active->path);
    if (authored == nullptr ||
        !authored->document.is_object())
    {
        return nullptr;
    }
    const nlohmann::json& document = authored->document;
    const bool multiEmitter =
        document.contains("emitters") &&
        document.at("emitters").is_array() &&
        !document.at("emitters").empty();
    if (!multiEmitter) return &document;
    const size_t index = std::min(
        particleEmitterIndex,
        document.at("emitters").size() - 1);
    fieldPrefix = "emitters." + std::to_string(index) + ".";
    return &document.at("emitters").at(index);
}

void MaterialInstanceAssetEditorPanel::HandleParticleCurveKeyEvent(
    Rml::Element& element)
{
    const std::string curve = element.GetAttribute<std::string>(
        "data-curve", "");
    if (curve != "colorKeys" && curve != "sizeKeys") return;
    const int indexValue = element.GetAttribute<int>(
        "data-index", -1);
    if (indexValue < 0) return;
    std::string prefix;
    const nlohmann::json* emitter = ActiveParticleEmitter(prefix);
    if (emitter == nullptr) return;
    const std::string fieldPath = prefix + curve;
    nlohmann::json keys = ParticleCurveKeys(
        *emitter, curve, fieldPath);
    const size_t index = static_cast<size_t>(indexValue);
    if (index >= keys.size()) return;
    float value = 0.0f;
    if (!ParseNumber(ControlValue(element), value))
    {
        localStatus = "A complete finite number is required.";
        return;
    }
    const std::string component = element.GetAttribute<std::string>(
        "data-component", "");
    if (component == "time")
    {
        if (!ParticleCurveEdit::SetKeyTime(keys, index, value))
        {
            // 失败时不提交任何值；保留旧数组，避免把时间冲突写进文档。
            localStatus = "Curve times must stay at least 0.001 apart "
                "and inside [0,1].";
            return;
        }
    }
    else if (curve == "colorKeys" &&
        (component == "r" || component == "g" ||
         component == "b" || component == "a"))
    {
        const size_t channel = component == "r" ? 0 :
            component == "g" ? 1 : component == "b" ? 2 : 3;
        ParticleCurveEdit::SetColorComponent(
            keys, index, channel, value);
    }
    else if (curve == "sizeKeys" && component == "size")
    {
        ParticleCurveEdit::SetSize(keys, index, value);
    }
    else
    {
        return;
    }
    EditorAssetFieldChange change;
    change.field = fieldPath;
    const std::string emitterName =
        ParticleEmitterNameForField(fieldPath);
    if (!emitterName.empty())
        change.expectedEmitterName = emitterName;
    change.value = std::move(keys);
    QueueParticleAssetDraft(std::move(change));
    localStatus.clear();
}

void MaterialInstanceAssetEditorPanel::HandleParticleCurveAdd(
    const std::string& curve)
{
    if (curve != "colorKeys" && curve != "sizeKeys") return;
    std::string prefix;
    const nlohmann::json* emitter = ActiveParticleEmitter(prefix);
    if (emitter == nullptr) return;
    const std::string fieldPath = prefix + curve;
    nlohmann::json keys = ParticleCurveKeys(
        *emitter, curve, fieldPath);
    const ParticleCurveInterpolation interpolation =
        ParticleCurveEdit::ReadInterpolation(
            ParticleEmitterForEdit(*emitter));
    if (!ParticleCurveEdit::AddKey(
        keys, curve == "colorKeys", interpolation))
    {
        localStatus = keys.size() >= ParticleCurveEdit::kMaxKeys
            ? std::string("A curve holds at most 8 keys.")
            : std::string("No gap large enough to insert another key.");
        return;
    }
    EditorAssetFieldChange change;
    change.field = fieldPath;
    const std::string emitterName =
        ParticleEmitterNameForField(fieldPath);
    if (!emitterName.empty())
        change.expectedEmitterName = emitterName;
    change.value = std::move(keys);
    QueueParticleAssetDraft(std::move(change));
    localStatus.clear();
}

void MaterialInstanceAssetEditorPanel::HandleParticleCurveRemove(
    const std::string& curve,
    size_t index)
{
    if (curve != "colorKeys" && curve != "sizeKeys") return;
    std::string prefix;
    const nlohmann::json* emitter = ActiveParticleEmitter(prefix);
    if (emitter == nullptr) return;
    const std::string fieldPath = prefix + curve;
    nlohmann::json keys = ParticleCurveKeys(
        *emitter, curve, fieldPath);
    if (index >= keys.size()) return;
    EditorAssetFieldChange change;
    change.field = fieldPath;
    const std::string emitterName =
        ParticleEmitterNameForField(fieldPath);
    if (!emitterName.empty())
        change.expectedEmitterName = emitterName;
    if (keys.size() <= 1)
    {
        // 单 key 曲线删除后应回到端点回退：显式 key 数组不能为空，
        // 因此提交 reset 移除字段，而不是留下一个被解析器拒绝的空数组。
        change.reset = true;
    }
    else
    {
        if (!ParticleCurveEdit::RemoveKey(keys, index))
        {
            localStatus = "A curve keeps at least one key.";
            return;
        }
        if (keys.empty())
        {
            // 正常路径不会走到这里；保留防御，数组为空时同样回退端点值。
            change.reset = true;
        }
        else
        {
            change.value = std::move(keys);
        }
    }
    QueueParticleAssetDraft(std::move(change));
    localStatus.clear();
}

void MaterialInstanceAssetEditorPanel::HandleParticleEmitterStructure(
    const std::string& action)
{
    const auto* active = resourceEditor.ActiveTab();
    if (active == nullptr ||
        active->kind != EditorAssetKind::ParticleSystem)
    {
        return;
    }
    const auto* authored = ParticleDocument(active->path);
    if (authored == nullptr) return;

    nlohmann::json working = authored->document;
    // 结构命令提交整条数组；先把尚未 flush 的字段草稿合并进工作副本，
    // 否则两帧内连续操作会用旧数组覆盖用户刚输入的字段。
    const bool hasPendingDraft = pendingParticlePath == active->path;
    if (hasPendingDraft)
    {
        // 先整体替换结构数组，再应用字段路径；反序会让字段变更被旧数组覆盖。
        for (const auto& pending : pendingParticleChanges)
        {
            if (pending.field != "emitters" || pending.reset) continue;
            const auto* value =
                std::get_if<nlohmann::json>(&pending.value);
            if (value != nullptr && value->is_array())
                working["emitters"] = *value;
        }
        for (const auto& pending : pendingParticleChanges)
        {
            if (pending.field != "emitters")
                ApplyParticleDraftChange(working, pending);
        }
    }

    size_t selected = particleEmitterIndex;
    ParticleEmitterEditStatus status =
        ParticleEmitterEditStatus::InvalidDocument;
    std::string feedback;
    if (action == "particle-emitter-add")
    {
        status = AddParticleEmitter(working, selected);
        feedback = "Emitter added.";
    }
    else if (action == "particle-emitter-remove")
    {
        status = RemoveParticleEmitter(working, selected);
        feedback = "Emitter removed.";
    }
    else if (action == "particle-emitter-up")
    {
        status = MoveParticleEmitter(working, selected, -1);
        feedback = "Emitter moved up.";
    }
    else if (action == "particle-emitter-down")
    {
        status = MoveParticleEmitter(working, selected, 1);
        feedback = "Emitter moved down.";
    }
    else
    {
        return;
    }

    if (status != ParticleEmitterEditStatus::Ok)
    {
        // 失败时保留 pending 字段草稿，交给下一帧 Build 正常 flush。
        switch (status)
        {
        case ParticleEmitterEditStatus::MaxEmitters:
            localStatus = "A particle system holds at most 16 emitters.";
            break;
        case ParticleEmitterEditStatus::MinEmitters:
            localStatus = "A particle system keeps at least one emitter.";
            break;
        case ParticleEmitterEditStatus::NoMove:
            localStatus = "Emitter is already at the edge.";
            break;
        case ParticleEmitterEditStatus::InvalidIndex:
            localStatus = "Emitter selection is out of date.";
            break;
        default:
            localStatus = "Particle document is invalid.";
            break;
        }
        RefreshResourceEditor();
        return;
    }

    // 新数组要到下一帧快照才可见；先记录目标索引，避免旧 count 把它夹回。
    particleEmitterIndex = selected;
    pendingParticleEmitterSelection = selected;
    pendingParticleEmitterSelectionPath = active->path;
    particleEmitterMarkupCache.clear();
    particleFieldsMarkupCache.clear();

    if (hasPendingDraft)
    {
        pendingParticleChanges.clear();
        pendingParticlePath.clear();
    }
    EditorAssetFieldChange change;
    change.field = "emitters";
    change.value = working.at("emitters");
    QueueParticleAssetDraft(std::move(change));
    localStatus = feedback;
    RefreshResourceEditor();
}

void MaterialInstanceAssetEditorPanel::HandleParticleModuleStack(
    const std::string& action,
    Rml::Element& element)
{
    const auto* active = resourceEditor.ActiveTab();
    if (active == nullptr ||
        active->kind != EditorAssetKind::ParticleSystem)
    {
        return;
    }
    const auto* authored = ParticleDocument(active->path);
    if (authored == nullptr || !authored->document.is_object())
        return;

    nlohmann::json working = authored->document;
    // 与发射器结构命令一致：先把未 flush 的字段草稿合并进工作副本，
    // 再用最新数组执行模块操作，避免旧快照覆盖用户刚输入的值。
    const bool hasPendingDraft = pendingParticlePath == active->path;
    if (hasPendingDraft)
    {
        for (const auto& pending : pendingParticleChanges)
        {
            if (pending.field != "emitters" || pending.reset) continue;
            const auto* value =
                std::get_if<nlohmann::json>(&pending.value);
            if (value != nullptr && value->is_array())
                working["emitters"] = *value;
        }
        for (const auto& pending : pendingParticleChanges)
        {
            if (pending.field != "emitters")
                ApplyParticleDraftChange(working, pending);
        }
    }

    const bool v2 = working.value("version", 1) == 2;
    const bool hasEmittersArray =
        working.contains("emitters") &&
        working.at("emitters").is_array() &&
        !working.at("emitters").empty();
    const size_t emitterIndex = hasEmittersArray
        ? std::min(particleEmitterIndex,
            working.at("emitters").size() - 1)
        : 0;
    nlohmann::json& emitter = hasEmittersArray
        ? working["emitters"].at(emitterIndex)
        : working;
    const size_t moduleCount = emitter.contains("modules") &&
        emitter.at("modules").is_array()
        ? emitter.at("modules").size() : 0;

    if (action == "particle-module-stack-select")
    {
        const int requested = element.GetAttribute<int>(
            "data-index", -1);
        particleModuleStackIndex = requested < 0 || moduleCount == 0
            ? 0
            : std::min(static_cast<size_t>(requested),
                moduleCount - 1);
        RefreshResourceEditor();
        return;
    }

    if (action == "particle-upgrade-v2")
    {
        if (v2)
        {
            localStatus = "Document already uses the v2 module stack.";
            RefreshResourceEditor();
            return;
        }
        working["version"] = 2;
        bool upgraded = true;
        if (hasEmittersArray)
        {
            for (auto& entry : working["emitters"])
            {
                if (!EnsureParticleModules(entry))
                {
                    upgraded = false;
                    break;
                }
            }
        }
        else
        {
            upgraded = EnsureParticleModules(working);
        }
        if (!upgraded)
        {
            // 失败不提交任何命令；pending 字段草稿原样保留。
            localStatus = "Could not upgrade the particle document to v2.";
            RefreshResourceEditor();
            return;
        }

        particleModuleStackIndex = 0;
        particleEmitterMarkupCache.clear();
        particleFieldsMarkupCache.clear();
        if (hasPendingDraft)
        {
            pendingParticleChanges.clear();
            pendingParticlePath.clear();
        }
        // version 必须先落盘：后续 emitters/modules 结构的解析语义由它决定。
        EditorAssetFieldChange versionChange;
        versionChange.field = "version";
        versionChange.value = 2.0;
        QueueParticleAssetDraft(std::move(versionChange));
        EditorAssetFieldChange structureChange;
        structureChange.field = hasEmittersArray ? "emitters" : "modules";
        structureChange.value = hasEmittersArray
            ? working.at("emitters") : working.at("modules");
        QueueParticleAssetDraft(std::move(structureChange));
        localStatus = "Particle document upgraded to v2 modules.";
        RefreshResourceEditor();
        return;
    }

    if (!v2)
    {
        localStatus = "Upgrade the document to v2 before editing modules.";
        RefreshResourceEditor();
        return;
    }

    const size_t selected = moduleCount == 0 ? 0 :
        std::min(particleModuleStackIndex, moduleCount - 1);
    // 行按钮自带 data-index：即使该行尚未被选中，也必须操作被点击的模块。
    const int rowValue = element.GetAttribute<int>(
        "data-index", static_cast<int>(selected));
    const size_t rowIndex = moduleCount == 0 ? 0 :
        std::min(static_cast<size_t>(std::max(rowValue, 0)),
            moduleCount - 1);
    size_t nextSelection = selected;
    bool success = false;
    std::string feedback;
    if (action == "particle-module-stack-add")
    {
        std::string type;
        if (document != nullptr)
        {
            if (auto* control = document->GetElementById(
                "particle-module-type"))
                type = ControlValue(*control);
        }
        success = AddParticleModule(
            emitter, type, nextSelection);
        feedback = "Module added.";
    }
    else if (action == "particle-module-stack-remove")
    {
        success = RemoveParticleModule(
            emitter, rowIndex);
        feedback = "Module removed.";
        nextSelection = rowIndex;
    }
    else if (action == "particle-module-stack-up" ||
        action == "particle-module-stack-down")
    {
        const int delta =
            action == "particle-module-stack-up" ? -1 : 1;
        success = MoveParticleModule(
            emitter, rowIndex, delta);
        feedback = "Module moved.";
        nextSelection = success
            ? static_cast<size_t>(
                static_cast<std::ptrdiff_t>(rowIndex) + delta)
            : rowIndex;
    }
    else if (action == "particle-module-stack-toggle")
    {
        const bool enabled = rowIndex < moduleCount &&
            emitter.at("modules").at(rowIndex).value(
                "enabled", true);
        success = SetParticleModuleEnabled(
            emitter, rowIndex, !enabled);
        feedback = enabled ? "Module disabled." : "Module enabled.";
        nextSelection = rowIndex;
    }
    else
    {
        return;
    }

    if (!success)
    {
        localStatus = action == "particle-module-stack-add"
            ? std::string("The selected module cannot be added.")
            : std::string("Module operation is not valid for the current stack.");
        RefreshResourceEditor();
        return;
    }

    const size_t updatedCount = emitter.contains("modules") &&
        emitter.at("modules").is_array()
        ? emitter.at("modules").size() : 0;
    particleModuleStackIndex = updatedCount == 0 ? 0 :
        std::min(nextSelection, updatedCount - 1);
    particleEmitterMarkupCache.clear();
    particleFieldsMarkupCache.clear();
    if (hasPendingDraft)
    {
        pendingParticleChanges.clear();
        pendingParticlePath.clear();
    }
    EditorAssetFieldChange change;
    change.field = hasEmittersArray ? "emitters" : "modules";
    change.value = hasEmittersArray
        ? working.at("emitters") : working.at("modules");
    QueueParticleAssetDraft(std::move(change));
    localStatus = feedback;
    RefreshResourceEditor();
}

void MaterialInstanceAssetEditorPanel::HandleParticleFieldEvent(
    Rml::Element& element)
{
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active
        ? ParticleDocument(active->path) : nullptr;
    if (authored == nullptr) return;
    const std::string field = element.GetAttribute<std::string>(
        "data-field", "");
    const std::string type = element.GetAttribute<std::string>(
        "data-value-type", "string");
    if (field.empty()) return;
    const std::string value = ControlValue(element);
    EditorAssetFieldChange change;
    change.field = field;
    const std::string emitterName =
        ParticleEmitterNameForField(field);
    if (!emitterName.empty())
        change.expectedEmitterName = emitterName;
    try
    {
        if (type == "number")
        {
            change.value = std::stod(value);
        }
        else if (type == "boolean")
        {
            change.value = value == "true";
        }
        else if (type == "vec3")
        {
            const auto parsed = nlohmann::json::parse(value);
            if (!parsed.is_array() || parsed.size() != 3)
                throw std::invalid_argument("vec3 requires three numbers");
            change.value = parsed.get<std::array<double, 3>>();
        }
        else if (type == "array" || type == "object")
        {
            change.value = nlohmann::json::parse(value);
        }
        else
        {
            change.value = value;
        }
    }
    catch (const std::exception& error)
    {
        localStatus = std::string("Invalid particle field value: ") +
            error.what();
        return;
    }
    QueueParticleAssetDraft(std::move(change));
}

void MaterialInstanceAssetEditorPanel::HandleParticleFieldReset(
    Rml::Element& element)
{
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active
        ? ParticleDocument(active->path) : nullptr;
    if (authored == nullptr) return;
    const std::string field = element.GetAttribute<std::string>(
        "data-field", "");
    if (field.empty()) return;
    EditorAssetFieldChange change;
    change.field = field;
    change.reset = true;
    const std::string emitterName =
        ParticleEmitterNameForField(field);
    if (!emitterName.empty())
        change.expectedEmitterName = emitterName;
    QueueParticleAssetDraft(std::move(change));
    localStatus = "Field reset queued.";
}

bool MaterialInstanceAssetEditorPanel::ProcessParticleKeyboard(
    Rml::Event& event)
{
    if (event.GetType() != "keydown") return false;
    const auto* active = resourceEditor.ActiveTab();
    if (!resourceEditorVisible || HasModal() || active == nullptr ||
        active->kind != EditorAssetKind::ParticleSystem)
        return false;
    // 文本输入优先保留原生 Delete/方向键/撤销语义；只在编辑器空白或
    // 非输入控件聚焦时接管粒子快捷键。
    for (Rml::Element* target = event.GetTargetElement();
         target != nullptr; target = target->GetParentNode())
    {
        if (dynamic_cast<Rml::ElementFormControl*>(target) != nullptr)
            return false;
        const std::string tag = target->GetTagName();
        if (tag == "input" || tag == "textarea" || tag == "select")
            return false;
    }
    const int key = event.GetParameter<int>("key_identifier", 0);
    const bool ctrl =
        event.GetParameter<int>("ctrl_key", 0) != 0;
    const bool alt =
        event.GetParameter<int>("alt_key", 0) != 0;
    if (ctrl && !alt &&
        (key == Rml::Input::KI_Z || key == Rml::Input::KI_Y))
    {
        if (!RequestParticleHistory(key == Rml::Input::KI_Y))
            return false;
        RefreshResourceEditor();
        return true;
    }
    if (!ctrl && !alt && key == Rml::Input::KI_DELETE)
    {
        HandleParticleEmitterStructure("particle-emitter-remove");
        RefreshResourceEditor();
        return true;
    }
    if (alt && !ctrl &&
        (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN))
    {
        HandleParticleEmitterStructure(
            key == Rml::Input::KI_UP
                ? "particle-emitter-up"
                : "particle-emitter-down");
        RefreshResourceEditor();
        return true;
    }
    return false;
}


void MaterialInstanceAssetEditorPanel::RefreshParticleEditorMarkup()
{
    const auto* active = resourceEditor.ActiveTab();
    const bool blueprintAsset =
        active && active->kind == EditorAssetKind::Blueprint;
    const bool particleAsset = active && active->kind == EditorAssetKind::ParticleSystem;
    for (const char* id : {"blueprint-asset-editor", "blueprint-asset-save", "blueprint-asset-revert"})
        widgetDocument.SetProperty(id, "display", blueprintAsset ? "block" : "none");
    for (const char* id : {"particle-undo", "particle-redo", "particle-validate", "particle-revert"})
        widgetDocument.SetProperty(id, "display", particleAsset ? "inline-block" : "none");
    widgetDocument.SetProperty("resource-preview-panel", "display", blueprintAsset ? "none" : "flex");
    widgetDocument.SetProperty("resource-details-panel", "display", blueprintAsset ? "none" : "flex");
    widgetDocument.SetClass("particle-editor", "visible", particleAsset);
    widgetDocument.SetProperty("resource-json-inspector", "display",
        particleAsset ? "none" : "block");
    if (particleAsset)
    {
        const auto* authored = ParticleDocument(active->path);
        if (active->path != particleEmitterDocumentPath)
        {
            if (!particleEmitterDocumentPath.empty())
            {
                particleEmitterSelectionByPath[particleEmitterDocumentPath] =
                    particleEmitterIndex;
                particleModuleByPath[particleEmitterDocumentPath] =
                    particleModule;
            }
            particleEmitterDocumentPath = active->path;
            const auto selection =
                particleEmitterSelectionByPath.find(active->path);
            particleEmitterIndex = selection !=
                particleEmitterSelectionByPath.end()
                ? selection->second : 0;
            const auto module = particleModuleByPath.find(active->path);
            particleModule = module != particleModuleByPath.end()
                ? module->second : std::string("emitter");
            particleEmitterMarkupCache.clear();
            particleFieldsMarkupCache.clear();
            particleModuleStackIndex = 0;
            pendingParticleEmitterSelection.reset();
            pendingParticleEmitterSelectionPath.clear();
            particleHistoryCommandAwaitingSnapshot = false;
            particleHistoryCommandPath.clear();
            particleHistoryCommandRevision = 0;
        }
        size_t emitterCount = 1;
        std::string selectorMarkup;
        if (authored != nullptr)
        {
            const auto& document = authored->document;
            const bool multiEmitter =
                document.contains("emitters") &&
                document.at("emitters").is_array() &&
                !document.at("emitters").empty();
            if (multiEmitter)
                emitterCount = document.at("emitters").size();
            // 结构命令的结果要到下一帧快照才可见：请求索引已落在当前数组内
            // 就立即采用，否则保留 pending，不能按旧 count 永久夹回。
            if (pendingParticleEmitterSelection &&
                pendingParticleEmitterSelectionPath == active->path)
            {
                const size_t requested =
                    *pendingParticleEmitterSelection;
                if (requested < emitterCount)
                {
                    particleEmitterIndex = requested;
                    pendingParticleEmitterSelection.reset();
                    pendingParticleEmitterSelectionPath.clear();
                }
                else
                {
                    particleEmitterIndex = std::min(
                        particleEmitterIndex, emitterCount - 1);
                }
            }
            else if (particleEmitterIndex >= emitterCount)
            {
                particleEmitterIndex = emitterCount - 1;
            }
            for (size_t index = 0; index < emitterCount; ++index)
            {
                std::string name = "emitter" + std::to_string(index);
                if (multiEmitter)
                    name = document.at("emitters").at(index).value(
                        "name", name);
                selectorMarkup += "<option value=\"" +
                    std::to_string(index) + "\"" +
                    (index == particleEmitterIndex ? " selected" : "") +
                    ">" + Text(name) + "</option>";
            }
            if (selectorMarkup != particleEmitterMarkupCache)
            {
                particleEmitterMarkupCache = selectorMarkup;
                SetMarkup("particle-emitter-select", selectorMarkup);
            }
            SetControlValue(
                "particle-emitter-select",
                std::to_string(particleEmitterIndex));
        }
        else if (!particleEmitterMarkupCache.empty())
        {
            particleEmitterMarkupCache.clear();
            SetMarkup("particle-emitter-select", std::string());
        }
        // v2 模块栈选择只服务编辑面板；每帧按当前发射器模块数夹紧，
        // 结构命令改变数组后旧索引不会越界。
        size_t moduleCount = 0;
        if (authored != nullptr &&
            authored->document.is_object() &&
            authored->document.value("version", 1) == 2)
        {
            const auto& document = authored->document;
            const bool multiEmitter =
                document.contains("emitters") &&
                document.at("emitters").is_array() &&
                !document.at("emitters").empty();
            if (multiEmitter)
            {
                const size_t index = std::min(
                    particleEmitterIndex,
                    document.at("emitters").size() - 1);
                const auto& emitter =
                    document.at("emitters").at(index);
                if (emitter.contains("modules") &&
                    emitter.at("modules").is_array())
                    moduleCount = emitter.at("modules").size();
            }
            else if (document.contains("modules") &&
                document.at("modules").is_array())
            {
                moduleCount = document.at("modules").size();
            }
        }
        particleModuleStackIndex = moduleCount == 0 ? 0 :
            std::min(particleModuleStackIndex, moduleCount - 1);
        SetDisabled("particle-emitter-add",
            authored == nullptr ||
                emitterCount >= kMaxParticleEmitters);
        SetDisabled("particle-emitter-remove",
            authored == nullptr || emitterCount <= 1);
        SetDisabled("particle-emitter-up",
            authored == nullptr || particleEmitterIndex == 0);
        SetDisabled("particle-emitter-down",
            authored == nullptr ||
                particleEmitterIndex + 1 >= emitterCount);
        SetDisabled("particle-validate",
            authored == nullptr || particleCommandAwaitingSnapshot ||
                particleHistoryCommandAwaitingSnapshot ||
                !pendingParticleChanges.empty());
        SetDisabled("particle-revert",
            authored == nullptr || particleCommandAwaitingSnapshot ||
                particleHistoryCommandAwaitingSnapshot ||
                !pendingParticleChanges.empty());
        RefreshParticleHistoryControls();
        const std::string fieldsMarkup = ParticleFieldsMarkup();
        if (fieldsMarkup != particleFieldsMarkupCache)
        {
            particleFieldsMarkupCache = fieldsMarkup;
            SetMarkup("particle-fields", fieldsMarkup);
        }
        SetMarkup("particle-status",
            authored == nullptr ? std::string("Reading particle asset...") :
            authored->dirty ? std::string("Unsaved particle changes.") :
            std::string("Particle asset clean."));
        for (const char* module : {"system", "emitter", "shape", "velocity",
                 "forces", "collision", "life", "color", "flipbook", "curves"})
            widgetDocument.SetClass(
                std::string("particle-module-") + module,
                "active",
                particleModule == module);
        if (authored != nullptr &&
            (active->path != particlePreviewPath ||
             authored->documentRevision != particlePreviewRevision))
        {
            particlePreviewPath = active->path;
            particlePreviewRevision = authored->documentRevision;
            RequestParticlePreview(active->path);
        }
        if (authored != nullptr)
        {
            particleEmitterSelectionByPath[active->path] =
                particleEmitterIndex;
            particleModuleByPath[active->path] = particleModule;
        }
    }
    else
    {
        particleEmitterDocumentPath.clear();
        particleEmitterMarkupCache.clear();
        particleFieldsMarkupCache.clear();
        particleModuleStackIndex = 0;
        pendingParticleEmitterSelection.reset();
        pendingParticleEmitterSelectionPath.clear();
        for (const char* id : {"particle-emitter-add",
                 "particle-emitter-remove",
                 "particle-emitter-up",
                 "particle-emitter-down"})
            SetDisabled(id, true);
        SetDisabled("particle-undo", true);
        SetDisabled("particle-redo", true);
    }
}

} // namespace VL::EditorUi
