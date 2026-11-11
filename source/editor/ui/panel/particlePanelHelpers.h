#pragma once

#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "editor/particles/particleCurveEdit.h"
#include "editor/particles/particleEmitterEdit.h"
#include "editor/particles/particleFlipbookEdit.h"
#include "editor/particles/particleModuleEdit.h"
#include "editor/ui/materialInstanceEditorView.h"
#include "ui/uiAction.h"

namespace VL::EditorUi::ParticlePanelDetail
{

inline bool ParticleFieldInModule(
    const std::string& module,
    const std::string& field)
{
    // RmlUi 固定页签没有独立的 Renderer/Events/Determinism 页：
    // 这些字段归入 Emitter 页；v2 模块归属仍由
    // ParticleModuleTypeForField 写进 loader 接受的 renderer/events/emitter。
    if (module == "emitter")
        return field == "beam" ||
            field.rfind("beam.", 0) == 0 ||
            field == "name" || field == "position" ||
            field == "localSpace" ||
            field == "renderMode" ||
            field == "billboardAlignment" ||
            field == "billboardNormal" ||
            field == "billboardUp" ||
            field == "ribbonWidth" ||
            field == "ribbonSegments" ||
            field == "ribbonFade" ||
            field == "mesh.path" ||
            field == "mesh.scale" ||
            field == "mesh.alignment" ||
            field == "mesh.materialInstancePath" ||
            field == "materialInstancePath" ||
            field == "meshLighting" ||
            field == "meshAmbient" ||
            field == "billboardShadows" ||
            field == "billboardLighting" ||
            field == "billboardAmbient" ||
            field == "billboardLightingStrength" ||
            field == "billboardSpecular" ||
            field == "billboardRoughness" ||
            field == "billboardForwardScattering" ||
            field == "light.intensity" ||
            field == "light.radius" ||
            field == "light.iesProfile" ||
            field == "light.brightnessKeys" ||
            field == "light.castShadows" ||
            field == "light.shadowBias" ||
            field == "decal.size" ||
            field == "decal.rotation" ||
            field == "decal.color" ||
            field == "decal.opacity" ||
            field == "decal.edgeSoftness" ||
            field == "decal.noiseStrength" ||
            field == "decal.roughness" ||
            field == "decal.metallic" ||
            field == "decal.ambientOcclusion" ||
            field == "decal.normalStrength" ||
            field == "decal.emissive" ||
            field == "decal.emissiveStrength" ||
            field == "decal.decalResponse" ||
            field == "decal.albedoMap" ||
            field == "decal.normalMap" ||
            field == "decal.ormMap" ||
            field == "decal.sortOrder" ||
            field == "distortionStrength" ||
            field == "useFixedBounds" ||
            field == "fixedBounds.center" ||
            field == "fixedBounds.extent" ||
            field == "maxParticles" ||
            field == "randomSeed" ||
            field == "deterministicRandom" ||
            field == "fixedTimeStep" ||
            field == "spawnRate" || field == "burst" ||
            field == "spawnPerUnit" || field == "burstInterval" ||
            field == "burstCount" || field == "burstDelay" ||
            field == "burstPhase" || field == "attributeBindings" ||
            field == "lodDistance" ||
            field == "lodSpawnRateScale" ||
            field == "lod.tiers" ||
            field == "maxDrawDistance" ||
            field == "fadeDistance" ||
            field == "minScreenSize" ||
            field == "maxScreenSize" ||
            field == "rotation" ||
            field == "rotationSpeed" ||
            field == "rotationRandom" ||
            field == "events.spawn" ||
            field == "events.death" ||
            field == "events.collision";
    if (module == "shape")
        return field == "shape.type" || field == "shape.radius" ||
            field == "shape.extent" || field == "shape.height" ||
            field == "shape.innerRadius";
    if (module == "velocity")
        return field == "velocity.min" || field == "velocity.max";
    if (module == "forces")
        return field == "gravity" || field == "drag" ||
            field == "curlNoise.strength" ||
            field == "curlNoise.frequency" ||
            field == "curlNoise.speed" ||
            field == "vortex.strength" ||
            field == "vortex.radius" ||
            field == "vortex.axis" ||
            field == "attractor.position" ||
            field == "attractor.strength" ||
            field == "attractor.radius" ||
            field == "wind";
    if (module == "collision")
        return field == "collision.type" ||
            field == "collision.bounce" ||
            field == "collision.friction" ||
            field == "collision.radius" ||
            field == "collision.killOnCollision" ||
            field == "collision.ccd" ||
            field == "collision.plane.normal" ||
            field == "collision.plane.offset" ||
            field == "collision.box.min" ||
            field == "collision.box.max" ||
            field == "collision.capsule.a" ||
            field == "collision.capsule.b" ||
            field == "collision.capsule.radius" ||
            field == "collision.sdf.path" ||
            field == "collision.mesh.path";
    if (module == "life")
        return field == "lifetime" || field == "lifetimeScaleKeys" ||
            field == "size" ||
            field == "softness" || field == "softParticles" ||
            field == "softParticleDistance" || field == "sortParticles" ||
            field == "opacity";
    if (module == "color")
        return field == "colorStart" || field == "colorEnd" ||
            field == "blend";
    if (module == "flipbook")
        return field == "texture" || field == "flipbook.columns" ||
            field == "flipbook.rows" || field == "flipbook.fps" ||
            field == "flipbook.startFrame" || field == "flipbook.endFrame" ||
            field == "flipbook.loop" || field == "flipbook.randomStartFrame" ||
            field == "flipbook.frameDurations" ||
            field == "flipbook.framePivots" ||
            field == "flipbook.frameUVs" ||
            field == "flipbook.frameRotations";
    if (module == "curves")
        return field == "colorKeys" || field == "sizeKeys";
    return false;
}

inline bool ParticleMenuCommandAllowed(const char* id)
{
    const std::string_view name(id);
    return name == "menu-file-save" ||
        name == "menu-file-revert" ||
        name == "menu-file-reload" ||
        name == "menu-edit-validate";
}

inline std::string ParticleNumber(float value)
{
    std::ostringstream stream;
    stream << std::setprecision(9) << value;
    return stream.str();
}

inline std::string ParticleCurveInput(
    const std::string& curve,
    size_t index,
    const char* component,
    float value)
{
    return "<input type=\"text\" data-action=\"particle-curve-key\""
        " data-curve=\"" + curve +
        "\" data-index=\"" + std::to_string(index) +
        "\" data-component=\"" + component +
        "\" data-value-type=\"number\" value=\"" +
        ParticleNumber(value) + "\"/>";
}

inline nlohmann::json ParticleChangeValue(const EditorAssetFieldValue& value)
{
    return std::visit(
        [](const auto& item) { return nlohmann::json(item); }, value);
}

// v2 的字段真实存放在 emitters[].modules[].fields 里，编辑投影统一先展平；
// v1 没有 modules 时展平等价于原对象，调用方可以直接复用同一份读取逻辑。
inline nlohmann::json ParticleEmitterForEdit(const nlohmann::json& emitter)
{
    return FlattenParticleEmitter(emitter);
}

// 旧 emitter 根 materialInstancePath 是 mesh 路径的别名；结构编辑合并草稿
// 时也按同一规则规范化，避免根别名在 v2 合并阶段覆盖 mesh 值。
inline void RemoveParticleMaterialInstancePathAlias(nlohmann::json& emitter)
{
    if (!emitter.is_object()) return;
    emitter.erase("materialInstancePath");
    const auto mesh = emitter.find("mesh");
    if (mesh != emitter.end() && mesh->is_object() && mesh->empty())
        emitter.erase(mesh);
    const auto modules = emitter.find("modules");
    if (modules == emitter.end() || !modules->is_array()) return;
    for (auto& module : *modules)
    {
        if (!module.is_object()) continue;
        const auto fields = module.find("fields");
        if (fields == module.end() || !fields->is_object()) continue;
        fields->erase("materialInstancePath");
        const auto moduleMesh = fields->find("mesh");
        if (moduleMesh != fields->end() && moduleMesh->is_object() &&
            moduleMesh->empty())
            fields->erase(moduleMesh);
    }
}

// 结构编辑提交整条 emitters 数组；提交前把尚未 flush 的字段草稿合并进
// 工作副本，语义与 ApplyAssetChanges 的粒子路径一致：
// 根字段、emitters.<index>.<field>、点路径与 reset 都按同一规则写入。
inline void ApplyParticleDraftChange(
    nlohmann::json& document,
    const EditorAssetFieldChange& change)
{
    nlohmann::json* target = &document;
    std::string field = change.field;
    bool v2EmitterField = false;
    if (field.rfind("emitters.", 0) == 0)
    {
        const size_t indexStart = std::string("emitters.").size();
        const size_t separator = field.find('.', indexStart);
        if (separator == std::string::npos) return;
        size_t index = 0;
        for (size_t cursor = indexStart; cursor < separator; ++cursor)
        {
            const char character = field[cursor];
            if (character < '0' || character > '9') return;
            index = index * 10 +
                static_cast<size_t>(character - '0');
        }
        if (!document.contains("emitters") ||
            !document.at("emitters").is_array() ||
            index >= document.at("emitters").size())
        {
            return;
        }
        target = &document.at("emitters").at(index);
        field = field.substr(separator + 1);
        v2EmitterField = document.is_object() &&
            document.value("version", 1) == 2;
    }
    else if (document.is_object() &&
        document.value("version", 1) == 2 &&
        field != "name" && field != "version" &&
        field != "emitters" && field != "modules" &&
        field != "userParameters")
    {
        // v2 允许根对象直接承载隐式 emitter 的 modules；这类文档的字段
        // 同样必须写进模块，不能落到会被解析器忽略的根键上。
        v2EmitterField = true;
    }
    if (field.empty() || !target->is_object()) return;
    if (field == "materialInstancePath")
        field = "mesh.materialInstancePath";

    if (v2EmitterField)
    {
        nlohmann::json value;
        if (!change.reset) value = ParticleChangeValue(change.value);
        if (SetParticleEmitterField(
            *target, field, value, change.reset))
        {
            if (field == "mesh.materialInstancePath")
                RemoveParticleMaterialInstancePathAlias(*target);
            return;
        }
        // 模块 helper 拒绝时保留旧的 raw 写入兜底，避免工作副本与
        // 已提交命令在结构异常文档上完全失联。
    }

    size_t start = 0;
    while (true)
    {
        const size_t separator = field.find('.', start);
        const bool last = separator == std::string::npos;
        const std::string segment = field.substr(
            start, last ? std::string::npos : separator - start);
        if (segment.empty() || !target->is_object()) return;
        if (last)
        {
            if (change.reset) target->erase(segment);
            else (*target)[segment] = ParticleChangeValue(change.value);
            if (field == "mesh.materialInstancePath")
                RemoveParticleMaterialInstancePathAlias(*target);
            return;
        }
        if (!target->contains(segment))
        {
            if (change.reset) return;
            (*target)[segment] = nlohmann::json::object();
        }
        target = &target->at(segment);
        start = separator + 1;
    }
}

} // namespace VL::EditorUi::ParticlePanelDetail
