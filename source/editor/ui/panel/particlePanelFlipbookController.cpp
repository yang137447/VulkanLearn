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
}
