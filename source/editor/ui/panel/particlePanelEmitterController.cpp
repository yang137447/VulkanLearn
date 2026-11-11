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
}
