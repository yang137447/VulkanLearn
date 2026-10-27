#include "editor/ui/editorSceneActorTree.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "editor/ui/editorTextMatch.h"
#include "ui/widgets/property/propertyWidgets.h"

namespace VL::EditorUi
{
namespace
{

// 类型表只供身份解析、过滤器与图标使用；大纲不再按类型自动生成文件夹。
const std::vector<EditorSceneActorGroupSpec> GroupSpecs = {
    {EditorSceneActorType::DirectionalLight, "directionalLight", "Directional Lights",
        "icons/light-directional.png"},
    {EditorSceneActorType::PointLight, "pointLight", "Point Lights", "icons/light-point.png"},
    {EditorSceneActorType::SpotLight, "spotLight", "Spot Lights", "icons/light-spot.png"},
    {EditorSceneActorType::SquareAreaLight, "squareAreaLight", "Square Area Lights",
        "icons/light-area.png"},
    {EditorSceneActorType::Camera, "camera", "Cameras", "icons/camera.png"},
    {EditorSceneActorType::Environment, "environment", "Environments", "icons/environment.png"},
    {EditorSceneActorType::SceneWind, "wind", "Wind", "icons/wind.png"},
    {EditorSceneActorType::Mesh, "mesh", "Meshes", "icons/mesh.png"},
    {EditorSceneActorType::Script, "blueprintInstance", "Blueprint Instances", "icons/file-box.png"},
};

const char* EnvironmentTypeName(EnvironmentType type)
{
    switch (type)
    {
    case EnvironmentType::None: return "None";
    case EnvironmentType::Hdri: return "HDRI";
    case EnvironmentType::ProceduralSky: return "Procedural Sky";
    }
    return "Unknown";
}

const char* LightTypeName(LightSnapshotType type)
{
    switch (type)
    {
    case LightSnapshotType::Directional: return "Directional";
    case LightSnapshotType::Point: return "Point";
    case LightSnapshotType::Spot: return "Spot";
    case LightSnapshotType::SquareArea: return "Square Area";
    }
    return "Unknown";
}

std::string Vector(Eigen::Vector3f value)
{
    return SceneActorNumber(value.x()) + ", " + SceneActorNumber(value.y()) + ", " +
        SceneActorNumber(value.z());
}

std::string ColorHex(const Eigen::Vector3f& linearColor)
{
    const std::array<float, 4> rgba = {linearColor.x(), linearColor.y(), linearColor.z(), 1.0f};
    return UiWidgets::MakeColorReadout(rgba).hex;
}

std::string ColorLinear(const Eigen::Vector3f& linearColor)
{
    return Vector(linearColor);
}

void AddField(std::vector<EditorSceneActorField>& fields, const std::string& id,
    const std::string& label, const std::string& value)
{
    fields.push_back({id, label, value});
}

void AddVectorField(std::vector<EditorSceneActorField>& fields, const std::string& id,
    const std::string& label, const Eigen::Vector3f& value)
{
    AddField(fields, id, label, Vector(value));
}

// mesh actor 的 section 行：多 section 时用 section 名做后缀，
// 避免同一份 Details 里出现多组无法区分的行。
std::string EmptyFallback(const std::string& value)
{
    return value.empty() ? "(none)" : value;
}

// 只有携带有效材质实例的 section 才是可打开的槽；没有 MI 的 section 仍然
// 出现在只读字段里，但不会在树里生成一个点了没有反应的子行。
bool MeshActorHasOpenableSlot(const EditorSceneActorSnapshot& actor)
{
    for (const EditorSceneMeshSectionSnapshot& section : actor.sections)
        if (!section.materialInstancePath.empty()) return true;
    return false;
}

// 槽行标签与迁移前一致：优先用槽名，没有槽名时退回 MI 文件名。
std::string SlotLabel(const MaterialInstanceModelMaterialSnapshot& slot)
{
    return slot.materialSlotName.empty()
        ? std::filesystem::path(slot.materialInstancePath).filename().string()
        : slot.materialSlotName;
}

// 行内容签名用的 FNV-1a 混合：不追求密码学强度，只要求"内容变了几乎必然变号"。
constexpr uint64_t SignatureOffset = 1469598103934665603ull;
constexpr uint64_t SignaturePrime = 1099511628211ull;

void MixSignature(uint64_t& hash, const std::string& value)
{
    for (const unsigned char byte : value)
    {
        hash ^= byte;
        hash *= SignaturePrime;
    }
    // 分隔符让 ("ab","") 与 ("a","b") 不产生同一个签名。
    hash ^= 0xffu;
    hash *= SignaturePrime;
}

void MixSignature(uint64_t& hash, uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
    {
        hash ^= (value >> shift) & 0xffu;
        hash *= SignaturePrime;
    }
}

} // namespace

const std::vector<EditorSceneActorGroupSpec>& SceneActorGroupSpecs()
{
    return GroupSpecs;
}

const EditorSceneActorGroupSpec* FindSceneActorGroupSpec(EditorSceneActorType type)
{
    for (const EditorSceneActorGroupSpec& spec : GroupSpecs)
        if (spec.type == type) return &spec;
    return nullptr;
}

const EditorSceneActorGroupSpec* FindSceneActorGroupSpecByKey(const std::string& key)
{
    for (const EditorSceneActorGroupSpec& spec : GroupSpecs)
        if (key == spec.key) return &spec;
    return nullptr;
}

const EditorSceneActorList& SceneActorList(const std::shared_ptr<const EditorSceneActorList>& actors)
{
    static const EditorSceneActorList empty;
    return actors == nullptr ? empty : *actors;
}

const char* SceneActorGroupTitle(EditorSceneActorType type)
{
    const EditorSceneActorGroupSpec* spec = FindSceneActorGroupSpec(type);
    return spec == nullptr ? "" : spec->title;
}

const char* SceneActorGroupIcon(EditorSceneActorType type)
{
    const EditorSceneActorGroupSpec* spec = FindSceneActorGroupSpec(type);
    return spec == nullptr ? "" : spec->icon;
}

std::string SceneActorIdentity(EditorSceneActorType type, const std::string& name)
{
    const EditorSceneActorGroupSpec* spec = FindSceneActorGroupSpec(type);
    return std::string(spec == nullptr ? "actor" : spec->key) + "|" + name;
}

bool ParseSceneActorIdentity(const std::string& identity, EditorSceneActorIdentity& parsed)
{
    const size_t separator = identity.find('|');
    if (separator == std::string::npos) return false;
    const EditorSceneActorGroupSpec* spec =
        FindSceneActorGroupSpecByKey(identity.substr(0, separator));
    if (spec == nullptr) return false;
    parsed.type = spec->type;
    parsed.name = identity.substr(separator + 1);
    // 与 SceneActorIdentity() 对称：空名字不可能由 World 构建出来，解析端不接受它，
    // 否则会拿空名字去查表并报一条误导性的"对象不存在"。
    return !parsed.name.empty();
}

bool SceneActorHasVisibilityToggle(EditorSceneActorType type)
{
    switch (type)
    {
    case EditorSceneActorType::Mesh:
    case EditorSceneActorType::DirectionalLight:
    case EditorSceneActorType::PointLight:
    case EditorSceneActorType::SpotLight:
    case EditorSceneActorType::SquareAreaLight:
        return true;
    // 相机 / 环境 / 风剖面 / 场景风没有渲染侧可见性；蓝图实例的可见性属于脚本状态
    //（destroyed），也没有逐 actor 的渲染开关命令。
    case EditorSceneActorType::Camera:
    case EditorSceneActorType::Environment:
    case EditorSceneActorType::SceneWind:
    case EditorSceneActorType::Script:
        return false;
    }
    return false;
}

bool SceneActorNameLess(const EditorSceneActorSnapshot& left, const EditorSceneActorSnapshot& right)
{
    const std::string leftName = FoldAsciiCase(left.displayName);
    const std::string rightName = FoldAsciiCase(right.displayName);
    if (leftName != rightName) return leftName < rightName;
    return left.identity < right.identity;
}

bool SceneActorMatchesQuery(const EditorSceneActorSnapshot& actor, const std::string& foldedQuery)
{
    if (foldedQuery.empty()) return true;
    return FoldAsciiCase(actor.displayName).find(foldedQuery) != std::string::npos;
}

bool SceneActorMatchesTypeFilter(const EditorSceneActorSnapshot& actor, const std::string& typeFilter)
{
    if (typeFilter.empty() || typeFilter == "all") return true;
    const EditorSceneActorGroupSpec* spec = FindSceneActorGroupSpecByKey(typeFilter);
    if (spec == nullptr) return true;
    return actor.type == spec->type;
}

bool SceneActorOrderLess(const EditorSceneActorSnapshot& left,
    const EditorSceneActorSnapshot& right, SceneActorSortOrder order)
{
    return order == SceneActorSortOrder::NameDescending
        ? SceneActorNameLess(right, left) : SceneActorNameLess(left, right);
}

bool SameSceneActorExpansion(
    const EditorSceneActorTreeExpansion& left, const EditorSceneActorTreeExpansion& right)
{
    return left.expandedActors == right.expandedActors;
}

std::vector<EditorSceneActorRow> BuildSceneActorRows(
    const std::vector<EditorSceneActorSnapshot>& actors,
    const std::string& query,
    const std::string& typeFilter,
    SceneActorSortOrder order,
    const EditorSceneActorTreeExpansion& expansion)
{
    const std::string foldedQuery = FoldAsciiCase(query);
    std::vector<EditorSceneActorRow> rows;
    std::vector<size_t> indices;
    for (size_t index = 0; index < actors.size(); ++index)
    {
        if (SceneActorMatchesQuery(actors[index], foldedQuery) &&
            SceneActorMatchesTypeFilter(actors[index], typeFilter))
            indices.push_back(index);
    }
    // UE 风格的 Actor 列表：跨类型统一排序；材质槽仍紧随它所属的 actor。
    std::sort(indices.begin(), indices.end(),
        [&actors, order](size_t left, size_t right)
        {
            return SceneActorOrderLess(actors[left], actors[right], order);
        });
    for (const size_t index : indices)
    {
        const EditorSceneActorSnapshot& actor = actors[index];
        const bool hasSlots = actor.type == EditorSceneActorType::Mesh &&
            MeshActorHasOpenableSlot(actor);
        const bool actorExpanded =
            hasSlots && expansion.expandedActors.count(actor.identity) != 0;
        EditorSceneActorRow row;
        row.type = actor.type;
        row.actorIndex = index;
        row.label = actor.displayName;
        row.hasChildren = hasSlots;
        row.expanded = actorExpanded;
        rows.push_back(std::move(row));
        if (!actorExpanded) continue;
        // 槽行与 MeshSceneActorModel 的 materials 同序：动作分量索引可以直接交给
        // 既有的 SelectModelMaterial，不需要第二份槽位表。
        const MaterialInstanceModelSnapshot model = MeshSceneActorModel(actor);
        for (size_t slotIndex = 0; slotIndex < model.materials.size(); ++slotIndex)
        {
            const MaterialInstanceModelMaterialSnapshot& slot = model.materials[slotIndex];
            EditorSceneActorRow slotRow;
            slotRow.isSlot = true;
            slotRow.type = actor.type;
            slotRow.actorIndex = index;
            slotRow.slotIndex = slotIndex;
            slotRow.materialSlotIndex = slot.materialSlotIndex;
            slotRow.label = SlotLabel(slot);
            rows.push_back(std::move(slotRow));
        }
    }
    return rows;
}

uint64_t SceneActorRowsSignature(const std::vector<EditorSceneActorSnapshot>& actors)
{
    uint64_t hash = SignatureOffset;
    MixSignature(hash, static_cast<uint64_t>(actors.size()));
    if (!actors.empty()) MixSignature(hash, actors.front().worldGeneration);
    for (const EditorSceneActorSnapshot& actor : actors)
    {
        MixSignature(hash, static_cast<uint64_t>(actor.type));
        MixSignature(hash, actor.displayName);
        MixSignature(hash, actor.identity);
        MixSignature(hash, static_cast<uint64_t>(actor.sections.size()));
        for (const EditorSceneMeshSectionSnapshot& section : actor.sections)
        {
            MixSignature(hash, section.materialInstancePath);
            MixSignature(hash, section.materialSlotName);
        }
    }
    return hash;
}

const char* SceneActorSortOrderKey(SceneActorSortOrder order)
{
    return order == SceneActorSortOrder::NameDescending ? "name-desc" : "name-asc";
}

bool ParseSceneActorSortOrder(const std::string& key, SceneActorSortOrder& order)
{
    if (key == "name-asc")
    {
        order = SceneActorSortOrder::NameAscending;
        return true;
    }
    if (key == "name-desc")
    {
        order = SceneActorSortOrder::NameDescending;
        return true;
    }
    return false;
}

bool SameSceneActorWindow(const EditorSceneActorWindow& left, const EditorSceneActorWindow& right)
{
    return left.begin == right.begin && left.end == right.end &&
        left.rowHeight == right.rowHeight && left.top == right.top && left.bottom == right.bottom;
}

EditorSceneActorWindow ComputeSceneActorWindow(
    size_t rowCount, float clientHeight, float scrollOffset, float rowHeight)
{
    EditorSceneActorWindow window;
    if (!(rowHeight > 0.0f)) rowHeight = SceneActorRowHeightFallback;
    window.rowHeight = rowHeight;
    if (rowCount == 0) return window;
    // 可视行数 +1 覆盖半行，再由 SceneActorOverscanRows 提供上下预取；总挂载量受预算限制。
    const size_t visibleRows = clientHeight > 0.0f
        ? static_cast<size_t>(std::ceil(clientHeight / rowHeight)) + 1
        : SceneActorOverscanRows + 1;
    const size_t windowRows = std::min(
        visibleRows + 2 * SceneActorOverscanRows, SceneActorMaxMountedRows);
    // rowCount - 1 是最后一行：滚动到内容底部时窗口必须能覆盖尾页。
    const size_t scrollRow = std::min(
        static_cast<size_t>(std::max(0.0f, scrollOffset) / rowHeight), rowCount - 1);
    size_t firstRow = scrollRow > SceneActorOverscanRows ? scrollRow - SceneActorOverscanRows : 0;
    firstRow = std::min(firstRow, rowCount > windowRows ? rowCount - windowRows : 0);
    const size_t endRow = std::min(rowCount, firstRow + windowRows);
    window.begin = firstRow;
    window.end = endRow;
    window.top = static_cast<float>(firstRow) * rowHeight;
    window.bottom = static_cast<float>(rowCount - endRow) * rowHeight;
    return window;
}

std::vector<EditorSceneActorField> SceneActorFields(const EditorSceneActorSnapshot& actor)
{
    std::vector<EditorSceneActorField> fields;
    AddField(fields, "actor-field-name", "Name", actor.displayName);
    switch (actor.type)
    {
    case EditorSceneActorType::Script:
        AddField(fields, "actor-field-script-class", "C# Class", actor.scriptClass);
        AddField(fields, "actor-field-script-id", "Blueprint ID", actor.identity.substr(std::string_view("blueprintInstance|").size()));
        break;
    case EditorSceneActorType::Mesh:
        AddField(fields, "actor-field-scene-object", "Scene Object",
            actor.sceneObjectIdentity);
        AddField(fields, "actor-field-sections", "Sections",
            std::to_string(actor.sections.size()));
        for (size_t index = 0; index < actor.sections.size(); ++index)
        {
            const EditorSceneMeshSectionSnapshot& section = actor.sections[index];
            const std::string prefix = "actor-field-section-" + std::to_string(index) + "-";
            const std::string suffix = actor.sections.size() == 1
                ? std::string() : " " + section.objectName;
            AddField(fields, prefix + "mesh", "Mesh" + suffix, EmptyFallback(section.meshKey));
            AddField(fields, prefix + "material", "Material" + suffix,
                EmptyFallback(section.materialKey));
            AddField(fields, prefix + "material-instance", "Material Instance" + suffix,
                EmptyFallback(section.materialInstancePath));
            AddField(fields, prefix + "slot", "Slot" + suffix,
                (section.materialSlotName.empty()
                    ? std::to_string(section.materialSlotIndex)
                    : section.materialSlotName + " (#" +
                        std::to_string(section.materialSlotIndex) + ")"));
        }
        break;
    case EditorSceneActorType::DirectionalLight:
    case EditorSceneActorType::PointLight:
    case EditorSceneActorType::SpotLight:
    case EditorSceneActorType::SquareAreaLight:
    {
        const LightSnapshot& light = actor.light;
        AddField(fields, "actor-field-light-type", "Light Type", LightTypeName(light.type));
        AddField(fields, "actor-field-color-hex", "Colour (sRGB)", ColorHex(light.color));
        if (light.type == LightSnapshotType::Directional || light.type == LightSnapshotType::Spot)
            AddVectorField(fields, "actor-field-direction", "Direction", light.direction);
        if (light.type == LightSnapshotType::SquareArea)
            AddVectorField(fields, "actor-field-normal", "Normal", light.direction);
        if (light.type == LightSnapshotType::Point || light.type == LightSnapshotType::Spot)
            AddField(fields, "actor-field-radius", "Radius", SceneActorNumber(light.radius));
        break;
    }
    case EditorSceneActorType::Camera:
    {
        const EditorSceneCameraActor& camera = actor.camera;
        AddVectorField(fields, "actor-field-forward", "Forward", camera.forward);
        AddVectorField(fields, "actor-field-right", "Right", camera.right);
        AddVectorField(fields, "actor-field-up", "Up", camera.up);
        break;
    }
    case EditorSceneActorType::Environment:
    {
        const EditorSceneEnvironmentActor& environment = actor.environment;
        AddField(fields, "actor-field-environment-type", "Environment Type",
            EnvironmentTypeName(environment.type));
        AddField(fields, "actor-field-intensity", "Intensity",
            SceneActorNumber(environment.intensity));
        AddField(fields, "actor-field-cube-size", "Cube Size",
            std::to_string(environment.cubeSize));
        AddField(fields, "actor-field-hdr-path", "HDR Path", EmptyFallback(environment.hdrPath));
        break;
    }
    case EditorSceneActorType::SceneWind:
    {
        // 场景风的值由可编辑控件展示（SceneActorEditableFields），这里不再重复
        // 生成一组只读行；作者 name 已在大纲/分组标题中显示。
        break;
    }
    }
    return fields;
}

std::vector<EditorSceneActorEditableField> SceneActorEditableFields(
    const EditorSceneActorSnapshot& actor)
{
    using P = EditorActorProperty;
    std::vector<EditorSceneActorEditableField> fields;
    if (actor.type == EditorSceneActorType::SceneWind)
    {
        // 场景风没有 TRS；方向、强度和阵风都直接编辑 World，再由场景文档暂存。
        // 阵风字段始终可见，写入非零值即开启；全零等效关闭。
        fields.push_back({P::WindDirection, "Direction", actor.worldWind.direction, 3});
        fields.push_back({P::WindStrength, "Strength (cm/s)",
            Eigen::Vector3f(actor.worldWind.strengthCmPerSec, 0, 0), 1});
        fields.push_back({P::WindGustAmplitude, "Gust Amplitude",
            Eigen::Vector3f(actor.worldWind.gustAmplitude, 0, 0), 1});
        fields.push_back({P::WindGustFrequency, "Gust Frequency",
            Eigen::Vector3f(actor.worldWind.gustFrequency, 0, 0), 1});
        fields.push_back({P::WindGustTurbulence, "Gust Turbulence",
            Eigen::Vector3f(actor.worldWind.gustTurbulence, 0, 0), 1});
        return fields;
    }
    if (actor.type == EditorSceneActorType::Environment)
        return fields;
    fields.push_back({P::Position, "Location (cm)", actor.position, 3});
    if (actor.type != EditorSceneActorType::SquareAreaLight)
        fields.push_back({P::Rotation, "Rotation (deg)", actor.rotation, 3});
    if (actor.type == EditorSceneActorType::Mesh || actor.type == EditorSceneActorType::Script)
    {
        fields.push_back({P::Scale, "Scale", actor.scale, 3});
        return fields;
    }
    if (actor.type == EditorSceneActorType::Camera)
    {
        fields.push_back({P::CameraFov, "Horizontal FOV (deg)",
            Eigen::Vector3f(actor.camera.horizontalFovDegrees, 0, 0), 1});
        fields.push_back({P::CameraNear, "Near Clip (cm)",
            Eigen::Vector3f(actor.camera.clipNear, 0, 0), 1});
        fields.push_back({P::CameraFar, "Far Clip (cm)",
            Eigen::Vector3f(actor.camera.clipFar, 0, 0), 1});
        // 字段集合不随曝光模式变化，切换只改显示；焦点与折叠状态不靠重建 DOM 维持。
        const CameraExposure& exposure = actor.camera.exposure;
        fields.push_back({P::CameraEv100, "Manual EV100",
            Eigen::Vector3f(static_cast<float>(exposure.ev100), 0, 0), 1});
        fields.push_back({P::CameraExposureCompensation, "Compensation (EV)",
            Eigen::Vector3f(static_cast<float>(exposure.compensation), 0, 0), 1});
        fields.push_back({P::CameraAutoMinEv100, "Min EV100",
            Eigen::Vector3f(static_cast<float>(exposure.autoMinEv100), 0, 0), 1});
        fields.push_back({P::CameraAutoMaxEv100, "Max EV100",
            Eigen::Vector3f(static_cast<float>(exposure.autoMaxEv100), 0, 0), 1});
        fields.push_back({P::CameraAutoSpeedUp, "Up Rate (1/s)",
            Eigen::Vector3f(static_cast<float>(exposure.autoSpeedUp), 0, 0), 1});
        fields.push_back({P::CameraAutoSpeedDown, "Down Rate (1/s)",
            Eigen::Vector3f(static_cast<float>(exposure.autoSpeedDown), 0, 0), 1});
        fields.push_back({P::CameraAutoTargetGray, "Target Gray (linear)",
            Eigen::Vector3f(static_cast<float>(exposure.autoTargetGray), 0, 0), 1});
        return fields;
    }
    fields.push_back({P::Color, "Light Colour (linear RGB)", actor.light.color, 3});
    // 主光的 lux 控件保留既有命令，其余灯光使用对象级命令与规范化物理单位。
    if (!actor.primaryDirectionalLight)
        fields.push_back({P::Intensity,
            actor.type == EditorSceneActorType::DirectionalLight ? "Intensity (lux)" :
            actor.type == EditorSceneActorType::SquareAreaLight ? "Radiance" : "Intensity (cd)",
            Eigen::Vector3f(actor.light.intensity, 0, 0), 1});
    if (actor.type == EditorSceneActorType::SpotLight)
    {
        fields.push_back({P::InnerCone, "Inner Half Angle (deg)",
            Eigen::Vector3f(actor.light.coneAngleInner * 180.0f / 3.14159265358979323846f, 0, 0), 1});
        fields.push_back({P::OuterCone, "Outer Half Angle (deg)",
            Eigen::Vector3f(actor.light.coneAngleOuter * 180.0f / 3.14159265358979323846f, 0, 0), 1});
    }
    if (actor.type == EditorSceneActorType::SquareAreaLight)
    {
        fields.push_back({P::Normal, "Normal", actor.light.direction, 3});
        fields.push_back({P::AreaSize, "Side Length (cm)",
            Eigen::Vector3f(actor.light.squareSize, 0, 0), 1});
    }
    return fields;
}

std::string SceneActorPropertyControlId(EditorActorProperty property, uint32_t component)
{
    return "actor-property-" + std::to_string(static_cast<unsigned>(property)) +
        "-" + std::to_string(component);
}

MaterialInstanceModelSnapshot MeshSceneActorModel(const EditorSceneActorSnapshot& actor)
{
    MaterialInstanceModelSnapshot model;
    model.worldGeneration = actor.worldGeneration;
    model.scenePath = actor.scenePath;
    model.objectId = actor.meshObjectId;
    model.displayName = actor.displayName;
    model.objectIdentity = actor.sceneObjectIdentity;
    for (const EditorSceneMeshSectionSnapshot& section : actor.sections)
    {
        // 只有携带有效材质实例的 section 才是可打开的槽；没有 MI 的 section 仍然
        // 出现在只读字段里，但不会在树里生成一个点了没有反应的子行。
        if (section.materialInstancePath.empty()) continue;
        MaterialInstanceModelMaterialSnapshot material;
        material.objectId = section.objectId;
        material.materialSlotIndex = section.materialSlotIndex;
        material.materialSlotName = section.materialSlotName;
        material.materialInstancePath = section.materialInstancePath;
        model.materials.push_back(std::move(material));
    }
    return model;
}

std::string SceneActorNumber(float value)
{
    std::ostringstream stream;
    stream << std::setprecision(9) << value;
    return stream.str();
}

} // namespace VL::EditorUi
