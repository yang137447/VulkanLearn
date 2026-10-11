#include "editor/ui/editorSceneActorTree.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

#include "editor/ui/editorTextMatch.h"
#include "ui/widgets/property/propertyWidgets.h"

namespace VL::EditorUi
{
namespace
{

// 固定分组顺序：与 SceneActorGroupSpecs() 的顺序一致，跨帧不重排。
const std::vector<EditorSceneActorGroupSpec> GroupSpecs = {
    {EditorSceneActorType::Mesh, "mesh", "Meshes", "icons/mesh.png"},
    {EditorSceneActorType::DirectionalLight, "directionalLight", "Directional Lights",
        "icons/light-directional.png"},
    {EditorSceneActorType::PointLight, "pointLight", "Point Lights", "icons/light-point.png"},
    {EditorSceneActorType::SpotLight, "spotLight", "Spot Lights", "icons/light-spot.png"},
    {EditorSceneActorType::SquareAreaLight, "squareAreaLight", "Square Area Lights",
        "icons/light-area.png"},
    {EditorSceneActorType::Camera, "camera", "Cameras", "icons/camera.png"},
    {EditorSceneActorType::Environment, "environment", "Environments", "icons/environment.png"},
    {EditorSceneActorType::WindProfile, "windProfile", "SpeedTree Wind Profiles",
        "icons/wind.png"},
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

size_t SceneActorGroupIndex(EditorSceneActorType type)
{
    for (size_t index = 0; index < GroupSpecs.size(); ++index)
        if (GroupSpecs[index].type == type) return index;
    return GroupSpecs.size();
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

std::vector<EditorSceneActorRow> BuildSceneActorRows(
    const std::vector<EditorSceneActorSnapshot>& actors,
    const std::string& query,
    const std::string& typeFilter)
{
    const std::string foldedQuery = FoldAsciiCase(query);
    std::vector<EditorSceneActorRow> rows;
    std::vector<size_t> groupIndices;
    for (const EditorSceneActorGroupSpec& spec : GroupSpecs)
    {
        groupIndices.clear();
        for (size_t index = 0; index < actors.size(); ++index)
        {
            const EditorSceneActorSnapshot& actor = actors[index];
            if (actor.type != spec.type) continue;
            if (!SceneActorMatchesQuery(actor, foldedQuery)) continue;
            if (!SceneActorMatchesTypeFilter(actor, typeFilter)) continue;
            groupIndices.push_back(index);
        }
        if (groupIndices.empty()) continue;
        std::sort(groupIndices.begin(), groupIndices.end(),
            [&actors](size_t left, size_t right)
            {
                return SceneActorNameLess(actors[left], actors[right]);
            });

        EditorSceneActorRow group;
        group.isGroup = true;
        group.type = spec.type;
        group.label = spec.title;
        group.actorCount = groupIndices.size();
        rows.push_back(std::move(group));
        for (const size_t index : groupIndices)
        {
            EditorSceneActorRow row;
            row.type = spec.type;
            row.actorIndex = index;
            row.label = actors[index].displayName;
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

std::vector<EditorSceneActorField> SceneActorFields(const EditorSceneActorSnapshot& actor)
{
    std::vector<EditorSceneActorField> fields;
    AddField(fields, "actor-field-name", "Name", actor.displayName);
    switch (actor.type)
    {
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
            AddField(fields, prefix + "wind", "Wind Profile" + suffix,
                EmptyFallback(section.windProfileKey));
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
        AddField(fields, "actor-field-color-linear", "Colour (linear)",
            ColorLinear(light.color));
        if (!actor.primaryDirectionalLight)
            AddField(fields, "actor-field-intensity", "Intensity",
                SceneActorNumber(light.intensity));
        AddVectorField(fields, "actor-field-position", "Position", light.position);
        if (light.type == LightSnapshotType::Directional || light.type == LightSnapshotType::Spot)
            AddVectorField(fields, "actor-field-direction", "Direction", light.direction);
        if (light.type == LightSnapshotType::SquareArea)
            AddVectorField(fields, "actor-field-normal", "Normal", light.direction);
        if (light.type == LightSnapshotType::Point || light.type == LightSnapshotType::Spot)
            AddField(fields, "actor-field-radius", "Radius", SceneActorNumber(light.radius));
        if (light.type == LightSnapshotType::Spot)
        {
            AddField(fields, "actor-field-cone-outer", "Cone Angle Outer (deg)",
                SceneActorNumber(light.coneAngleOuter * 180.0f / 3.14159265358979323846f));
            AddField(fields, "actor-field-cone-inner", "Cone Angle Inner (deg)",
                SceneActorNumber(light.coneAngleInner * 180.0f / 3.14159265358979323846f));
        }
        if (light.type == LightSnapshotType::SquareArea)
            AddField(fields, "actor-field-square-size", "Square Size",
                SceneActorNumber(light.squareSize));
        break;
    }
    case EditorSceneActorType::Camera:
    {
        const EditorSceneCameraActor& camera = actor.camera;
        AddVectorField(fields, "actor-field-position", "Position", camera.position);
        AddVectorField(fields, "actor-field-forward", "Forward", camera.forward);
        AddVectorField(fields, "actor-field-right", "Right", camera.right);
        AddVectorField(fields, "actor-field-up", "Up", camera.up);
        AddField(fields, "actor-field-hfov", "Horizontal FOV (deg)",
            SceneActorNumber(camera.horizontalFovDegrees));
        AddField(fields, "actor-field-clip-near", "Clip Near", SceneActorNumber(camera.clipNear));
        AddField(fields, "actor-field-clip-far", "Clip Far", SceneActorNumber(camera.clipFar));
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
    case EditorSceneActorType::WindProfile:
    {
        const EditorSceneWindActor& wind = actor.wind;
        AddField(fields, "actor-field-wind-key", "Profile Key", wind.key);
        AddVectorField(fields, "actor-field-bounds-min", "Source Bounds Min", wind.sourceBoundsMin);
        AddVectorField(fields, "actor-field-bounds-max", "Source Bounds Max", wind.sourceBoundsMax);
        AddField(fields, "actor-field-strength-response", "Strength Response",
            SceneActorNumber(wind.strengthResponse));
        AddField(fields, "actor-field-direction-response", "Direction Response",
            SceneActorNumber(wind.directionResponse));
        AddField(fields, "actor-field-gust-frequency", "Gust Frequency",
            SceneActorNumber(wind.gustFrequency));
        AddField(fields, "actor-field-gust-strength-min", "Gust Strength Min",
            SceneActorNumber(wind.gustStrengthMin));
        AddField(fields, "actor-field-gust-strength-max", "Gust Strength Max",
            SceneActorNumber(wind.gustStrengthMax));
        AddField(fields, "actor-field-gust-duration-min", "Gust Duration Min",
            SceneActorNumber(wind.gustDurationMin));
        AddField(fields, "actor-field-gust-duration-max", "Gust Duration Max",
            SceneActorNumber(wind.gustDurationMax));
        AddField(fields, "actor-field-gust-rise", "Gust Rise Scalar",
            SceneActorNumber(wind.gustRiseScalar));
        AddField(fields, "actor-field-gust-fall", "Gust Fall Scalar",
            SceneActorNumber(wind.gustFallScalar));
        AddField(fields, "actor-field-shared-start-height", "Shared Start Height",
            SceneActorNumber(wind.sharedStartHeight));
        AddField(fields, "actor-field-branch1-limit", "Branch 1 Stretch Limit",
            SceneActorNumber(wind.branch1StretchLimit));
        AddField(fields, "actor-field-branch2-limit", "Branch 2 Stretch Limit",
            SceneActorNumber(wind.branch2StretchLimit));
        AddField(fields, "actor-field-do-shared", "Shared Wind", wind.doShared ? "on" : "off");
        AddField(fields, "actor-field-do-branch1", "Branch 1 Wind", wind.doBranch1 ? "on" : "off");
        AddField(fields, "actor-field-do-branch2", "Branch 2 Wind", wind.doBranch2 ? "on" : "off");
        AddField(fields, "actor-field-do-ripple", "Ripple", wind.doRipple ? "on" : "off");
        AddField(fields, "actor-field-do-shimmer", "Shimmer", wind.doShimmer ? "on" : "off");
        break;
    }
    }
    return fields;
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
