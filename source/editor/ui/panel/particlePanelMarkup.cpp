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
}
