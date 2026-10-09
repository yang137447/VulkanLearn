#include "render/offline/offlineSceneAdapter.h"

#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include "render/offline/offlineMeshAdapter.h"
#include "render/offline/offlineColorImage.h"
#include "core/groomPfm.h"

namespace VL
{
namespace
{
Offline::Vec3 ToCore(const Eigen::Vector3f& vector)
{
    return {vector.x(), vector.y(), vector.z()};
}

double FiniteNumber(const nlohmann::json& value, const char* label)
{
    if (!value.is_number() || !std::isfinite(value.get<double>()))
        throw std::runtime_error(std::string(label) + " must be finite");
    return value.get<double>();
}

Offline::Vec3 Color(const nlohmann::json& value, const char* label)
{
    if (!value.is_array() || (value.size() != 3 && value.size() != 4))
        throw std::runtime_error(std::string(label) + " requires RGB or RGBA");
    const Offline::Vec3 color{FiniteNumber(value[0], label),
                              FiniteNumber(value[1], label),
                              FiniteNumber(value[2], label)};
    if (color.x < 0 || color.y < 0 || color.z < 0)
        throw std::runtime_error(std::string(label) + " must be non-negative");
    return color;
}

void RequireExactKeys(const nlohmann::json& object,
                      std::initializer_list<std::string_view> allowed,
                      const char* label)
{
    if (!object.is_object())
        throw std::runtime_error(std::string(label) + " must be an object");
    for (const auto& [key, value] : object.items())
    {
        bool known = false;
        for (std::string_view name : allowed)
            if (key == name) known = true;
        if (!known)
            throw std::runtime_error(std::string(label) + " has unsupported input " + key);
    }
}

Offline::SurfaceModel ParseModel(const nlohmann::json& material)
{
    const std::string model = material.at("offlineModel").get<std::string>();
    if (model == "Lambert") return Offline::SurfaceModel::Lambert;
    if (model == "ClothSheen") return Offline::SurfaceModel::ClothSheen;
    if (model == "ClothFiber") return Offline::SurfaceModel::ClothFiber;
    if (model == "AshikhminVelvet") return Offline::SurfaceModel::AshikhminVelvet;
    throw std::runtime_error("Unknown offlineModel: " + model);
}

void ApplyMaterial(const nlohmann::json& material,
                   const OfflineWorldAssets& assets,
                   Offline::MeshInstance& instance,
                   Offline::RenderInput& input,
                   bool prepareGeometry,
                   std::unordered_map<std::string,
                       std::shared_ptr<const Offline::GroomMap>>& grooms,
                   std::unordered_map<std::string,
                       std::shared_ptr<const Offline::ColorMap>>& colors)
{
    if (!material.contains("offlineModel"))
        throw std::runtime_error("Material has no offlineModel capability");
    instance.model = ParseModel(material);
    const auto& states = material.at("renderStates");
    if (states.value("renderMode", std::string("Opaque")) != "Opaque" ||
        states.value("cullMode", std::string("Back")) != "None")
        throw std::runtime_error("Offline material requires Opaque and cullMode None");
    if (!material.at("macros").empty())
        throw std::runtime_error("Offline macro combination is not implemented");
    const auto& textures = material.at("textures");
    for (const auto& [slot, reference] : textures.items())
    {
        const bool supported = reference.is_string() &&
            ((instance.model == Offline::SurfaceModel::ClothFiber && slot == "groomMap") ||
             (instance.model == Offline::SurfaceModel::ClothSheen &&
              (slot == "baseColorMap" || slot == "sheenColorMap")) ||
             (instance.model == Offline::SurfaceModel::Lambert && slot == "baseColorMap"));
        if (!supported)
            throw std::runtime_error("Offline texture binding is not implemented: " + slot);
    }
    const auto colorMap = [&](const char* slot) -> const Offline::ColorMap*
    {
        if (!textures.contains(slot)) return nullptr;
        const std::string reference = textures.at(slot).get<std::string>();
        const auto frozen = assets.colorAssets.find(reference);
        if (frozen == assets.colorAssets.end() || !frozen->second.bytes)
            throw std::runtime_error("Frozen color image is missing: " + reference);
        auto [it, inserted] = colors.try_emplace(reference);
        if (inserted) it->second = DecodeOfflineColorImage(frozen->second);
        // 提交边界必须发现损坏的已冻结字节；只跳过 BVH，不把解码失败推迟到已提交的 worker。
        if (!prepareGeometry) return nullptr;
        input.instanceColorMaps.push_back(it->second);
        return it->second.get();
    };

    const auto& params = material.at("parameters");
    if (instance.model == Offline::SurfaceModel::Lambert)
    {
        RequireExactKeys(params, {"u_baseColor"}, "Lambert parameters");
        instance.baseColor = Color(params.at("u_baseColor"), "u_baseColor");
        instance.baseColorMap = colorMap("baseColorMap");
    }
    else if (instance.model == Offline::SurfaceModel::ClothSheen)
    {
        RequireExactKeys(params, {"u_baseColor", "u_sheenColor", "u_roughness"},
                         "ClothSheen parameters");
        instance.baseColor = Color(params.at("u_baseColor"), "u_baseColor");
        instance.sheenColor = Color(params.at("u_sheenColor"), "u_sheenColor");
        instance.baseColorMap = colorMap("baseColorMap");
        instance.sheenColorMap = colorMap("sheenColorMap");
        instance.roughness = FiniteNumber(params.at("u_roughness"), "u_roughness");
        if (instance.roughness <= 0 || instance.roughness > 1)
            throw std::runtime_error("u_roughness must be in (0,1]");
    }
    else if (instance.model == Offline::SurfaceModel::AshikhminVelvet)
    {
        RequireExactKeys(params, {"u_sigma", "u_f0", "u_baseColor"},
                         "AshikhminVelvet parameters");
        const double sigma = FiniteNumber(params.at("u_sigma"), "u_sigma");
        const double f0 = FiniteNumber(params.at("u_f0"), "u_f0");
        const Offline::Vec3 diffuse = Color(params.at("u_baseColor"), "u_baseColor");
        if (sigma <= 0 || sigma > 1 || f0 < 0 || f0 > 1 ||
            diffuse.x > 1 || diffuse.y > 1 || diffuse.z > 1)
            throw std::runtime_error("Invalid AshikhminVelvet parameter range");
        if (prepareGeometry)
        {
            auto velvet = std::make_shared<const Offline::VelvetMaterial>(
                Offline::makeVelvetMaterial(sigma, f0, diffuse));
            instance.velvet = velvet.get();
            input.instanceVelvets.push_back(std::move(velvet));
        }
    }
    else
    {
        RequireExactKeys(params, {"u_yarnRotationDegrees"}, "ClothFiber parameters");
        if (params.contains("u_yarnRotationDegrees"))
        {
            instance.yarnRotationDegrees =
                FiniteNumber(params.at("u_yarnRotationDegrees"), "u_yarnRotationDegrees");
            if (std::abs(instance.yarnRotationDegrees) > 360)
                throw std::runtime_error("u_yarnRotationDegrees must be within [-360,360]");
        }
        const std::string reference = material.at("modelProfile").get<std::string>();
        const auto profile = assets.modelProfiles.find(reference);
        if (profile == assets.modelProfiles.end())
            throw std::runtime_error("Frozen modelProfile is missing: " + reference);
        auto fabric = std::make_shared<const Offline::FabricMaterial>(
            ParseOfflineClothProfile(profile->second));
        instance.fabric = fabric.get();
        input.instanceFabrics.push_back(std::move(fabric));
        if (!instance.mesh->hasValidUvFrames)
            throw std::runtime_error("ClothFiber requires non-degenerate UV axes");
        if (textures.contains("groomMap"))
        {
            const std::string reference = textures.at("groomMap").get<std::string>();
            const auto frozen = assets.groomAssets.find(reference);
            if (frozen == assets.groomAssets.end() || !frozen->second.bytes)
                throw std::runtime_error("Frozen groomMap is missing: " + reference);
            auto [it, inserted] = grooms.try_emplace(reference);
            if (inserted)
                it->second = std::make_shared<const Offline::GroomMap>(
                    Offline::ParseGroomPfm(*frozen->second.bytes, reference));
            if (prepareGeometry)
            {
                instance.groom = it->second.get();
                input.instanceGrooms.push_back(it->second);
            }
        }
    }
}

} // namespace

Offline::FabricMaterial ParseOfflineClothProfile(const nlohmann::json& profile)
{
    RequireExactKeys(profile, {"type", "version", "model", "eta", "yarns"},
                     "modelProfile");
    if (profile.at("type") != "offlineModelProfile" ||
        profile.at("version") != 1 || profile.at("model") != "ClothFiber")
        throw std::runtime_error("Unsupported ClothFiber modelProfile identity/version");
    Offline::FabricMaterial result{};
    result.caseName = "vl-cloth-fiber";
    result.label = "VL authored";
    result.limitation = "Authored VL profile, not recovered paper metadata";
    result.eta = FiniteNumber(profile.at("eta"), "eta");
    if (result.eta <= 0) throw std::runtime_error("eta must be positive");
    const auto& yarns = profile.at("yarns");
    if (!yarns.is_array() || yarns.size() != 2)
        throw std::runtime_error("ClothFiber requires two yarn families");
    double totalCoverage = 0;
    for (int family = 0; family < 2; ++family)
    {
        const auto& source = yarns[family];
        RequireExactKeys(source, {"coverage", "dye", "kd", "gammaS",
                                  "gammaV", "offsets", "lengths"}, "yarn");
        auto& yarn = result.yarns[family];
        yarn.coverage = FiniteNumber(source.at("coverage"), "coverage");
        yarn.dye = Color(source.at("dye"), "dye");
        yarn.kd = FiniteNumber(source.at("kd"), "kd");
        yarn.gammaS = FiniteNumber(source.at("gammaS"), "gammaS");
        yarn.gammaV = FiniteNumber(source.at("gammaV"), "gammaV");
        if (yarn.coverage < 0 || yarn.coverage > 1 || yarn.kd < 0 || yarn.kd > 1 ||
            yarn.gammaS <= 0 || yarn.gammaV <= 0)
            throw std::runtime_error("Invalid yarn coverage/scattering parameters");
        totalCoverage += yarn.coverage;
        const auto& offsets = source.at("offsets");
        const auto& lengths = source.at("lengths");
        if (!offsets.is_array() || offsets.size() < 2 || offsets.size() > 8 ||
            !lengths.is_array() || lengths.size() + 1 != offsets.size())
            throw std::runtime_error("Invalid yarn curve dimensions");
        yarn.controlPoints = static_cast<int>(offsets.size());
        for (int index = 0; index < yarn.controlPoints; ++index)
            yarn.offsets[index] = FiniteNumber(offsets[index], "offset");
        for (int index = 0; index < yarn.controlPoints - 1; ++index)
        {
            yarn.lengths[index] = FiniteNumber(lengths[index], "length");
            if (yarn.lengths[index] < 0)
                throw std::runtime_error("Yarn segment length must be non-negative");
        }
        // 同一离散化同时供 B 求值和 PDF 使用，不能仅改变一侧。
        Offline::resampleYarn(yarn, Offline::kTangentSamples);
    }
    if (totalCoverage > 1)
        throw std::runtime_error("Combined yarn coverage exceeds one");
    return result;
}

RuntimeResult<Offline::RenderInput> BuildOfflineRenderInput(
    const WorldSnapshot& snapshot,
    const OfflineWorldAssets& assets,
    const OfflineRenderSettings& settings,
    bool prepareGeometry,
    const std::atomic<std::uint64_t>* latestGeneration,
    std::uint64_t expectedGeneration)
{
    if (snapshot.worldGeneration != assets.worldGeneration)
        return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
            "Offline.GenerationMismatch", "CPU assets do not match World snapshot",
            assets.scenePath));
    if (snapshot.environment.type != EnvironmentType::None)
        return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
            "Offline.EnvironmentUnsupported", "Active environment lighting is unsupported",
            assets.scenePath, "environment"));
    if (settings.width <= 0 || settings.height <= 0 || settings.samples <= 0 ||
        settings.bounces < 0 || settings.lightSamples <= 0)
        return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
            "Offline.InvalidSettings", "Invalid offline dimensions or integrator settings"));
    if (!std::isfinite(snapshot.camera.clipNear) ||
        !std::isfinite(snapshot.camera.clipFar) ||
        snapshot.camera.clipNear <= 0 ||
        snapshot.camera.clipFar <= snapshot.camera.clipNear)
        return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
            "Offline.InvalidCamera", "Invalid camera clipping planes",
            assets.scenePath, "camera"));

    Offline::RenderInput input;
    input.width = settings.width;
    input.height = settings.height;
    input.targetSamples = settings.samples;
    input.interactivePreview = settings.interactivePreview;
    input.bounces = settings.bounces;
    input.lightSamples = settings.lightSamples;
    input.horizontalFovDegrees = snapshot.camera.horizontalFovDegrees;
    input.cameraClipNear = snapshot.camera.clipNear;
    input.cameraClipFar = snapshot.camera.clipFar;
    input.cameraRight = ToCore(snapshot.camera.right);
    input.cameraUp = ToCore(snapshot.camera.up);
    input.cameraForward = ToCore(snapshot.camera.forward);
    input.scene.cameraPosition = ToCore(snapshot.camera.position);
    input.scene.seed = settings.seed;
    input.scene.backdrop = {};

    if (snapshot.lights.size() > 1)
        return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
            "Offline.LightUnsupported", "Only one directional light is supported",
            assets.scenePath, "lights"));
    if (!snapshot.lights.empty())
    {
        const auto& light = snapshot.lights.front();
        if (light.type != LightSnapshotType::Directional)
        {
            if (light.type != LightSnapshotType::SquareArea)
                return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
                    "Offline.LightUnsupported", "Light type has no offline evaluator",
                    assets.scenePath, std::to_string(light.lightId)));
            if (light.squareSize <= 0 || light.direction.squaredNorm() <= 1.0e-12f)
                return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
                    "Offline.InvalidAreaLight", "Invalid square area emitter",
                    assets.scenePath, std::to_string(light.lightId)));
            input.scene.hasAreaEmitter = true;
            input.scene.lightCenter = ToCore(light.position);
            input.scene.lightWidth = light.squareSize;
            input.scene.lightDepth = light.squareSize;
            input.scene.areaColor = ToCore(light.color);
            input.scene.areaRadiance = light.intensity;
            // core 的 U×V 是正面法线；这里只传发光面朝向，不沿用方向光的入射方向。
            Offline::aimAreaLight(input.scene,
                ToCore(light.position + light.direction));
        }
        else
        {
            input.scene.directionalLight = true;
            input.scene.toLight = Offline::normalize(-ToCore(light.direction));
            input.scene.directionalIrradiance = light.intensity;
            input.scene.directionalColor = ToCore(light.color);
        }
    }

    std::unordered_map<std::string, std::shared_ptr<const Offline::Mesh>> meshes;
    std::unordered_map<std::string, std::shared_ptr<const Offline::GroomMap>> grooms;
    std::unordered_map<std::string, std::shared_ptr<const Offline::ColorMap>> colors;
    for (const MeshDrawSnapshot& draw : snapshot.meshDraws)
    {
        if (latestGeneration &&
            latestGeneration->load(std::memory_order_relaxed) != expectedGeneration)
            return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
                "Offline.Canceled", "Superseded offline prepare", draw.mesh.key));
        const auto section = assets.sections.find(draw.mesh.key);
        if (section == assets.sections.end())
            return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
                "Offline.MeshMissing", "CPU section is absent from candidate package",
                draw.mesh.key, draw.debugName));
        if (section->second.imported->buildPlan.assetType != "mesh")
            return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
                "Offline.MeshUnsupported", "Animated/SpeedTree geometry is unsupported",
                section->second.imported->buildPlan.meshAssetPath, draw.debugName));
        const auto material = assets.effectiveMaterialInstances.find(draw.materialInstance.key);
        if (material == assets.effectiveMaterialInstances.end())
            return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
                "Offline.MaterialMissing", "Effective material version is absent",
                draw.materialInstance.key, draw.debugName));
        try
        {
            auto [mesh, inserted] = meshes.try_emplace(draw.mesh.key);
            if (inserted)
            {
                const auto& imported = *section->second.imported;
                const auto cached = assets.coreMeshes.find(draw.mesh.key);
                const MeshSection& source =
                    imported.modelResource.sections.at(section->second.sectionIndex);
                if (prepareGeometry)
                    mesh->second = cached == assets.coreMeshes.end()
                        ? BuildOfflineMesh(source, latestGeneration, expectedGeneration)
                        : cached->second;
                else
                {
                    if (source.indices.empty() || source.indices.size() % 3 != 0)
                        throw std::runtime_error("Section needs triangle indices");
                    auto probe = std::make_shared<Offline::Mesh>();
                    for (size_t i = 0; i < source.indices.size(); i += 3)
                    {
                        for (size_t corner = 0; corner < 3; ++corner)
                            if (source.indices[i + corner] >= source.vertices.size())
                                throw std::runtime_error("Section index out of range");
                        const auto& a = source.vertices[source.indices[i]].texCoord;
                        const auto& b = source.vertices[source.indices[i + 1]].texCoord;
                        const auto& c = source.vertices[source.indices[i + 2]].texCoord;
                        if (std::abs((b.x() - a.x()) * (c.y() - a.y()) -
                                     (b.y() - a.y()) * (c.x() - a.x())) <= 1e-12)
                            probe->hasValidUvFrames = false;
                    }
                    mesh->second = std::move(probe);
                }
                input.instanceMeshes.push_back(mesh->second);
            }
            Offline::MeshInstance instance = BuildOfflineInstance(
                *mesh->second, draw.model, Offline::SurfaceModel::Lambert);
            ApplyMaterial(material->second, assets, instance, input,
                prepareGeometry, grooms, colors);
            input.scene.instances.push_back(instance);
        }
        catch (const std::exception& exception)
        {
            return RuntimeResult<Offline::RenderInput>::Failure(MakeRuntimeError(
                "Offline.PreflightFailed", exception.what(),
                draw.materialInstance.key, draw.debugName));
        }
    }
    return RuntimeResult<Offline::RenderInput>::Success(std::move(input));
}

RuntimeResult<Offline::RenderInput> BuildOfflineRenderInput(
    const WorldSnapshot& snapshot, const OfflineWorldAssets& assets,
    const OfflineRenderSettings& settings,
    const std::atomic<std::uint64_t>* latestGeneration,
    std::uint64_t expectedGeneration)
{
    return BuildOfflineRenderInput(snapshot, assets, settings, true,
        latestGeneration, expectedGeneration);
}

RuntimeResult<bool> PreflightOfflineRenderInput(
    const WorldSnapshot& snapshot, const OfflineWorldAssets& assets,
    const OfflineRenderSettings& settings)
{
    auto result = BuildOfflineRenderInput(snapshot, assets, settings, false, nullptr, 0);
    if (result.IsFailure())
        return RuntimeResult<bool>::Failure(result.Error());
    return RuntimeResult<bool>::Success(true);
}

} // namespace VL
