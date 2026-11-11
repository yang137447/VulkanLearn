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
}
