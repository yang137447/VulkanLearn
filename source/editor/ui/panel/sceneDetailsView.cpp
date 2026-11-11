#include "editor/ui/panel/sceneDetailsView.h"
#include "editor/ui/panel/particlePanelHelpers.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "editor/ui/editorSceneActorTree.h"
#include "editor/ui/panel/actorMarkup.h"
#include "editor/ui/panel/sceneOutlinerView.h"
#include "editor/ui/scriptComponentControls.h"
#include "editor/ui/scriptPhysicsControls.h"
#include "editor/ui/scriptPropertyControls.h"
#include "script/scriptTypeIdentity.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/widgets/input/inputWidgets.h"
#include "ui/widgets/layout/layoutWidgets.h"
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi::Panel
{
namespace
{

using UiWidgets::Option;
using UiWidgets::Text;
using UiWidgets::WidgetRefreshScope;

// 文本输入与范围控件都是 ElementFormControl，但取值路径不同：
// 范围控件的 GetValue() 走 RmlUi WidgetSlider 的 "%f" 序列化（6 位小数），
// 小于 1e-6 的拖动结果会被截成 0；精确值只保存在 value 属性里。
// 这里对范围控件直接读 value 属性，让两类控件都返回可解析的有限数，
// 也保证拖动不会落进 HandleChange 的 "A complete finite number is required." 分支。
struct SliderRange
{
    float minValue = -1.0f;
    float maxValue = 1.0f;
};

// HEAD(ImGui) 的范围解析语义，迁移后必须保持一致：通道范围 > 参数范围 > 按当前值推导的固定兜底。
SliderRange ResolveSliderRange(const EditorParameterSnapshot& parameter,
    const EditorParameterChannel& channel, float currentValue)
{
    if (channel.hasRange && std::isfinite(channel.minValue) &&
        std::isfinite(channel.maxValue) && channel.minValue <= channel.maxValue)
    {
        return {channel.minValue, channel.maxValue};
    }
    if (parameter.hasRange && std::isfinite(parameter.minValue) &&
        std::isfinite(parameter.maxValue) && parameter.minValue <= parameter.maxValue)
    {
        return {parameter.minValue, parameter.maxValue};
    }

    // 旧 M_ 文件没有范围时按默认值生成固定兜底，避免错误输入继续扩大可调区间。
    // 基准是 defaultValue 而不是正在编辑的值：拖动过程中区间不能跟着漂移。
    const float magnitude = std::abs(currentValue);
    float extent = std::max(1.0f, magnitude * 2.0f);
    if (!std::isfinite(extent)) extent = 1.0f;
    return {-extent, extent};
}

// HEAD 的折叠分组名与默认展开状态：General / Render States / Parameters / Texture Bindings。
// 键同时是 DOM id 后缀，切换只改既有分组元素的 class，不重建 DOM。
constexpr const char* DetailsSectionKeys[] = {
    "general", "render-states", "parameters", "textures", "camera-exposure"};

} // namespace

bool CameraExposureFieldEditable(EditorActorProperty property, ExposureMode mode)
{
    if (property == EditorActorProperty::CameraEv100) return mode == ExposureMode::Ev100;
    if (property == EditorActorProperty::CameraExposureCompensation) return true;
    return mode == ExposureMode::Auto;
}

std::string DetailsSectionId(const std::string& key)
{
    return std::string("details-section-") + key;
}

// 大纲选中 actor 的详情分组：每个类型一个分组，键由 SceneActorGroupSpecs() 派生，
// 不在这里维护第二份类型清单。
std::string ActorSectionKey(const std::string& groupKey)
{
    return "actor-" + groupKey;
}

std::string PrimaryDirectionalLightControls(const EditorSceneActorSnapshot& actor,
    const UiViewModelSnapshot& runtimeView)
{
    // 仓库里已经存在的两条运行时命令：SetSunIntensity 与 SetCsmCastShadows。
    // 它们只作用于主方向光（WorldBuilder 约定：场景里第一个 directionalLight），
    // 因此这两个控件只在这一行出现，并且不在只读字段里重复同一个值。
    std::vector<UiWidgets::NumericComponent> components;
    UiWidgets::NumericComponent intensity;
    intensity.id = "actor-sun-intensity";
    intensity.label = "Intensity";
    intensity.value = SceneActorNumber(actor.light.intensity);
    intensity.change = {"actor-sun-intensity", 0};
    // 范围与运行时页的 sun 滑条一致（0..150000 lux）；滑条只影响拖动粒度，文本输入仍是精确值。
    intensity.hasRange = true;
    intensity.minValue = SunIntensityMin;
    intensity.maxValue = SunIntensityMax;
    components.push_back(std::move(intensity));

    std::string markup = UiWidgets::NumericComponents(components);
    // CSM 开关沿用运行时页的两个按钮而不是下拉：RmlUi 的 select 在程序化写值时会
    // 自己派发 change，同一帧可能提交两次；点击只产生一个事件，语义与运行时页一致。
    markup += UiWidgets::PropertyControlRow("Cast Shadows (CSM)",
        UiWidgets::Button("actor-csm-enable", {"actor-csm-enable", 0}, "Enabled",
            UiWidgets::ButtonStyle::Command, runtimeView.csmCastShadows) +
        UiWidgets::Button("actor-csm-disable", {"actor-csm-disable", 0}, "Disabled",
            UiWidgets::ButtonStyle::Command, !runtimeView.csmCastShadows));
    return markup;
}

std::string BuildActorSectionMarkup(const EditorSceneActorSnapshot& actor,
    const std::unordered_set<std::string>& collapsedSections,
    const UiViewModelSnapshot& runtimeView,
    const std::function<std::string(const EditorSceneActorSnapshot&)>& sceneLogicComponentsMarkup)
{
    const auto expanded = [&collapsedSections](const std::string& key) {
        return collapsedSections.count(key) == 0;
    };
    const EditorSceneActorGroupSpec* spec = FindSceneActorGroupSpec(actor.type);
    if (spec == nullptr) return std::string();
    // 只读字段每个值都带 id：值随 World、相机或运行时命令变化时只刷新这一项，
    // 不重建分组 DOM（重建会丢焦点与滚动位置）。
    std::string body;
    for (const auto& field : SceneActorEditableFields(actor))
    {
        if (CameraExposureField(field.property) == nullptr)
            body += ActorPropertyRowMarkup(field);
    }
    for (const EditorSceneActorField& field : SceneActorFields(actor))
        body += UiWidgets::PropertyInfo(field.label, field.value, field.id);
    if (actor.type == EditorSceneActorType::Script)
    {
        body += UiWidgets::PropertyControlRow("Name",
            UiWidgets::Input("script-blueprint-name", {"script-blueprint-name"}, actor.displayName) +
            "<button id=\"script-blueprint-duplicate\" class=\"browser-icon\" data-action=\"script-blueprint-duplicate\" "
            "title=\"Duplicate Blueprint\"><img src=\"icons/copy.png\"/></button>"
            "<button id=\"script-blueprint-delete\" class=\"browser-icon\" data-action=\"script-blueprint-delete\" "
            "title=\"Delete Blueprint\"><img src=\"icons/trash.png\"/></button>");
        body += UiWidgets::PropertyInfo("Blueprint", actor.scriptBlueprintPath.empty() ? "(inline)" : actor.scriptBlueprintPath);
        body += UiWidgets::CollapsibleSection("script-physics-section", "script-physics", "Physics",
            ScriptPhysicsControls("script-", actor.scriptPhysics.is_object() ? actor.scriptPhysics : nlohmann::json::object()),
            {"section"}, expanded("script-physics"));
        if (actor.scriptSpawned) body += UiWidgets::PropertyInfo("Lifetime", "Spawned");
        body += ScriptComponentControls(actor);
        body += UiWidgets::PropertyControlRow("Add Component",
            UiWidgets::Select("script-component-kind", {"script-component-kind"},
                ScriptComponentOptions()) +
            "<button id=\"script-component-add\" class=\"browser-icon\" data-action=\"script-component-add\" "
            "title=\"Add component\"><img src=\"icons/plus.png\"/></button>");
        body += UiWidgets::PropertyControlRow("Mesh Asset",
            UiWidgets::Input("script-component-mesh", {"script-component-mesh"}, ""));
        if (sceneLogicComponentsMarkup) body += sceneLogicComponentsMarkup(actor);
        size_t index = 0;
        for (const auto& property : actor.scriptProperties.items())
        {
            const std::string id = "script-property-" + std::to_string(index);
            const UiWidgets::WidgetAction change{"script-property", index};
            const auto& value = property.value();
            const auto field = ScriptPropertyField(runtimeView.scripts.classes, actor.scriptClass, property.key(), value);
            const std::string reset = "<button id=\"script-reset-" + std::to_string(index) +
                "\" class=\"browser-icon\" data-action=\"script-reset\" data-index=\"" +
                std::to_string(index) + "\" title=\"Restore inherited value\"><img src=\"icons/refresh.png\"/></button>";
            body += "<div class=\"actor-property-row\">" +
                ScriptPropertyRow(property.key(), id, change, field, value, reset) + "</div>";
            ++index;
        }
        body += UiWidgets::PropertyControlRow("New Asset",
            UiWidgets::Input("script-asset-path", {"script-asset-path"}, "Common/Blueprints/BP_object.json") +
            "<button id=\"script-asset-create\" class=\"browser-icon\" data-action=\"script-asset-create\" "
            "title=\"Create Blueprint from this instance\"><img src=\"icons/save.png\"/></button>");
    }
    if (actor.primaryDirectionalLight)
        body += PrimaryDirectionalLightControls(actor, runtimeView);
    const std::string key = ActorSectionKey(spec->key);
    std::string markup = UiWidgets::CollapsibleSection(DetailsSectionId(key), key, actor.displayName, body,
        {"section"}, expanded(key));
    if (actor.type == EditorSceneActorType::Camera)
    {
        std::string exposure = "<div class=\"actor-property-row\">" +
            UiWidgets::PropertyControlRow("Exposure Mode",
            "<div class=\"horizontal-box segmented\">" +
            UiWidgets::Button("actor-exposure-manual", {"actor-exposure-manual"}, "Manual") +
            UiWidgets::Button("actor-exposure-auto", {"actor-exposure-auto"}, "Auto") + "</div>") + "</div>";
        exposure += "<div id=\"actor-exposure-physical-row\">" +
            UiWidgets::PropertyInfo("Camera EV100 (physical)", "", "actor-exposure-physical-ev") + "</div>";
        for (const auto& field : SceneActorEditableFields(actor))
            if (CameraExposureField(field.property) != nullptr)
                exposure += ActorPropertyRowMarkup(field);
        markup += UiWidgets::CollapsibleSection(DetailsSectionId("camera-exposure"),
            "camera-exposure", "Exposure", exposure, {"section"},
            expanded("camera-exposure"));
    }
    return markup;
}

void ApplySectionStateView(UiWidgets::WidgetDocument& widgetDocument,
    const std::unordered_set<std::string>& collapsedSections)
{
    // 展开状态只落在既有分组元素的 class 上：重建 DOM 会丢焦点和滚动位置，
    // 而面板每帧都从冻结快照刷新，静止帧必须写 0 次属性（SetClass 自带值缓存）。
    for (const char* key : DetailsSectionKeys)
        widgetDocument.SetClass(DetailsSectionId(key), "collapsed", collapsedSections.count(key) != 0);
    for (const EditorSceneActorGroupSpec& spec : SceneActorGroupSpecs())
    {
        const std::string key = ActorSectionKey(spec.key);
        widgetDocument.SetClass(DetailsSectionId(key), "collapsed", collapsedSections.count(key) != 0);
    }
}

void RefreshDetailsView(const DetailsViewContext& context)
{
    UiWidgets::WidgetDocument& widgetDocument = *context.widgetDocument;
    WidgetRefreshScope refresh(*context.refreshingControls);
    const std::unordered_set<std::string>& collapsedSections = *context.collapsedSections;
    const auto expanded = [&collapsedSections](const std::string& key) {
        return collapsedSections.count(key) == 0;
    };
    const MaterialInstanceEditorSnapshot& snapshot = *context.snapshot;
    const UiViewModelSnapshot& runtimeView = *context.runtimeView;
    const EditorSceneActorSnapshot* actor = FindSelectedActor(snapshot);
    const EditorSceneActorGroupSpec* actorSpec =
        actor == nullptr ? nullptr : FindSceneActorGroupSpec(actor->type);
    std::string scriptStructure;
    if (actor && actor->type == EditorSceneActorType::Script)
    {
        if (actor->scriptPhysics.is_object())
            for (const auto& joint : actor->scriptPhysics.value("constraints", nlohmann::json::array()))
                scriptStructure += joint.at("id").get<std::string>() + ":" + joint.at("type").get<std::string>() + ";";
        for (const auto& node : actor->scriptComponents)
            scriptStructure += node.value("id", "root") + "|" + node.at("type").get<std::string>() +
                "|" + node.value("parent", "") + ";";
        for (const auto& property : actor->scriptProperties.items())
            scriptStructure += property.key() + ":" + property.value().type_name() + ";";
        scriptStructure += actor->scriptComponentInstanceEditable.dump();
        if (context.sceneLogicComponentsMarkup)
            scriptStructure += context.sceneLogicComponentsMarkup(*actor);
        for (const auto& type : runtimeView.scripts.classes)
            if (ScriptTypeNameMatches(type, actor->scriptClass)) scriptStructure += type.at("properties").dump();
    }
    const std::string actorIdentity = actor == nullptr || actorSpec == nullptr
        ? "no-actor" : std::to_string(actor->worldGeneration) + "|" +
            actor->identity + "|" + actorSpec->key + "|" + scriptStructure;
    if (*context.actorDetailsIdentity != actorIdentity)
    {
        *context.actorDetailsIdentity = actorIdentity;
        widgetDocument.SetMarkup("actor-details-content", actor != nullptr
            ? BuildActorSectionMarkup(*actor, collapsedSections, runtimeView,
                context.sceneLogicComponentsMarkup)
            : "");
    }
    if (context.refreshActorValues) context.refreshActorValues(actor);
    if (context.viewingJson || context.resourceEditorVisible)
    {
        for (const char* id : DocumentCommandIds)
            widgetDocument.SetDisabled(id, !context.particleResourceActive ||
                !ParticlePanelDetail::ParticleMenuCommandAllowed(id));
        widgetDocument.SetMarkup("editor-notice", Text(context.localStatus == nullptr
            ? std::string() : *context.localStatus));
        return;
    }
    // 材质正文与 actor Details 分开缓存；选择对象不会重建材质输入控件。
    const std::string identity = (snapshot.activeDocument
            ? snapshot.activeDocument->assetPath + "|" + snapshot.activeDocument->schemaDigest
            : "closed");
    if (identity != *context.detailsIdentity)
    {
        *context.textureOptionsDirty = true;
        *context.detailsIdentity = identity;
        std::string markup;
        if (snapshot.activeDocument)
        {
            const auto& asset = *snapshot.activeDocument;
            // General 承载当前面板没有单独展示的文档身份：MI 路径、M_ schema、摘要与 revision。
            std::string general;
            general += UiWidgets::PropertyInfo("MI Asset", asset.assetPath);
            general += UiWidgets::PropertyInfo("M_ schema", asset.baseMaterialPath);
            general += UiWidgets::PropertyInfo("Schema digest", asset.schemaDigest);
            general += UiWidgets::PropertyInfo("Revision", std::to_string(asset.revision));
            markup += UiWidgets::CollapsibleSection(DetailsSectionId("general"), "general",
                "General", general, {"section"}, expanded("general"));

            std::string renderStates;
            for (size_t index = 0; index < asset.renderStates.size(); ++index)
            {
                const auto& state = asset.renderStates[index];
                renderStates += UiWidgets::PropertyRow(state.name,
                    UiWidgets::Select("state-" + std::to_string(index), {"state", index},
                        RenderStateOptions(state.name)),
                    "reset-state-" + std::to_string(index), {"reset-state", index});
            }
            markup += UiWidgets::CollapsibleSection(DetailsSectionId("render-states"), "render-states",
                "Render States", renderStates, {"section"}, expanded("render-states"));

            std::string parameters;
            for (size_t index = 0; index < asset.parameters.size(); ++index)
            {
                const auto& parameter = asset.parameters[index];
                std::vector<UiWidgets::NumericComponent> components;
                const bool color = parameter.type == EditorParameterType::Color;
                for (uint32_t component = 0; component < parameter.effectiveValue.ComponentCount(); ++component)
                {
                    static const char* names[] = {"X", "Y", "Z", "W"};
                    const auto& channel = parameter.channels[component];
                    UiWidgets::NumericComponent entry;
                    entry.id = "parameter-" + std::to_string(index) + "-" + std::to_string(component);
                    entry.label = channel.name.empty() ? names[component] : channel.name;
                    entry.value = Number(parameter.effectiveValue.values[component]);
                    entry.change = {"parameter", index, component};
                    // HEAD 对 Color 分量不使用滑条（它走 ColorPicker4），这里保持同一分工：
                    // 线性 RGBA 分量仍是文本输入，颜色只多一个 sRGB 读数与色块。
                    if (!color)
                    {
                        const SliderRange range = ResolveSliderRange(parameter, channel,
                            parameter.defaultValue.values[component]);
                        entry.hasRange = true;
                        entry.minValue = range.minValue;
                        entry.maxValue = range.maxValue;
                    }
                    components.push_back(std::move(entry));
                }
                std::string controls;
                if (color)
                {
                    controls += UiWidgets::ColorPreview("color-swatch-" + std::to_string(index),
                        "color-hex-" + std::to_string(index),
                        UiWidgets::MakeColorReadout(parameter.effectiveValue.values));
                }
                controls += UiWidgets::NumericComponents(components);
                parameters += UiWidgets::PropertyGroup(parameter.name, controls,
                    "reset-parameter-" + std::to_string(index), {"reset-parameter", index},
                    parameter.description);
            }
            markup += UiWidgets::CollapsibleSection(DetailsSectionId("parameters"), "parameters",
                "Parameters", parameters, {"section"}, expanded("parameters"));

            std::string textures;
            for (size_t index = 0; index < asset.textures.size(); ++index)
            {
                const auto& texture = asset.textures[index];
                std::string options = Option(texture.effectiveAssetPath, texture.effectiveAssetPath);
                for (const auto& candidate : snapshot.textureAssets)
                {
                    if (candidate.valid && candidate.assetPath != texture.effectiveAssetPath)
                        options += Option(candidate.assetPath, candidate.assetPath);
                }
                textures += UiWidgets::PropertyGroup(texture.slotName,
                    UiWidgets::Select("texture-" + std::to_string(index), {"texture", index}, options),
                    "reset-texture-" + std::to_string(index), {"reset-texture", index}, texture.description);
            }
            markup += UiWidgets::CollapsibleSection(DetailsSectionId("textures"), "textures",
                "Texture Bindings", textures, {"section"}, expanded("textures"));
        }
        widgetDocument.SetMarkup("details-content", UiWidgets::VerticalBox(markup));
        // 新 DOM 不复用旧选择器的 markup 缓存。
        if (snapshot.activeDocument)
        {
            for (size_t index = 0; index < snapshot.activeDocument->textures.size(); ++index)
                widgetDocument.ForgetMarkup("texture-" + std::to_string(index));
        }
    }
    ApplySectionStateView(widgetDocument, collapsedSections);
    if (context.refreshControls) context.refreshControls();
}

} // namespace VL::EditorUi::Panel
