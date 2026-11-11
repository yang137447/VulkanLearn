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
}
