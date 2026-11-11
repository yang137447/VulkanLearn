#include "mesh/vegetationAsset.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace VL
{
namespace
{

bool ReadVec3(
    const nlohmann::json& value,
    Eigen::Vector3f& out,
    const std::string& context)
{
    if (!value.is_array() || value.size() != 3)
        throw std::invalid_argument(context + " requires three numbers");
    for (size_t index = 0; index < 3; ++index)
    {
        if (!value[index].is_number())
            throw std::invalid_argument(context + " requires three numbers");
        out[static_cast<int>(index)] = value[index].get<float>();
        if (!std::isfinite(out[static_cast<int>(index)]))
            throw std::invalid_argument(context + " must be finite");
    }
    return true;
}

float ReadFiniteFloat(
    const nlohmann::json& value,
    const std::string& context)
{
    if (!value.is_number())
        throw std::invalid_argument(context + " must be a number");
    const float result = value.get<float>();
    if (!std::isfinite(result))
        throw std::invalid_argument(context + " must be finite");
    return result;
}

uint32_t ReadUint32(
    const nlohmann::json& value,
    const std::string& context)
{
    if (!value.is_number_integer())
        throw std::invalid_argument(context + " must be a non-negative integer");
    const int64_t result = value.get<int64_t>();
    if (result < 0 ||
        result > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
        throw std::invalid_argument(context + " must be a uint32");
    return static_cast<uint32_t>(result);
}

Eigen::Matrix4f InstanceMatrix(const VegetationInstance& instance)
{
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model.block<3, 3>(0, 0) =
        instance.rotation.toRotationMatrix() *
        instance.scale.asDiagonal();
    model.block<3, 1>(0, 3) = instance.position;
    return model;
}

} // namespace

bool LoadVegetationAsset(
    const std::filesystem::path& path,
    VegetationAsset& asset,
    std::string& error)
{
    asset = VegetationAsset{};
    error.clear();
    try
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            error = "Cannot open vegetation asset: " + path.string();
            return false;
        }
        nlohmann::json json;
        file >> json;
        if (!json.is_object())
            throw std::invalid_argument("vegetation asset must be an object");
        if (json.value("version", 0) != 1)
            throw std::invalid_argument("vegetation asset version must be 1");
        if (!json.contains("mesh") || !json.at("mesh").is_string() ||
            json.at("mesh").get<std::string>().empty())
            throw std::invalid_argument(
                "vegetation mesh must be a non-empty string");
        asset.meshPath = json.at("mesh").get<std::string>();
        if (!json.contains("instances") || !json.at("instances").is_array())
            throw std::invalid_argument("vegetation instances must be an array");
        const auto& instances = json.at("instances");
        if (instances.empty() || instances.size() > 100000)
            throw std::invalid_argument("vegetation instances requires 1..100000 entries");
        asset.instances.reserve(instances.size());
        for (size_t index = 0; index < instances.size(); ++index)
        {
            const auto& entry = instances[index];
            if (!entry.is_object())
                throw std::invalid_argument("vegetation instance must be an object");
            VegetationInstance instance;
            if (!entry.contains("position"))
                throw std::invalid_argument("vegetation instance requires position");
            ReadVec3(entry.at("position"), instance.position,
                "vegetation instance position");
            if (entry.contains("rotation"))
            {
                const auto& rotation = entry.at("rotation");
                if (!rotation.is_array() || rotation.size() != 4)
                    throw std::invalid_argument(
                        "vegetation instance rotation must be a xyzw quaternion");
                Eigen::Vector4f values;
                for (size_t component = 0; component < 4; ++component)
                {
                    if (!rotation[component].is_number())
                        throw std::invalid_argument(
                            "vegetation instance rotation must be numeric");
                    values[static_cast<int>(component)] =
                        rotation[component].get<float>();
                }
                if (!values.allFinite() || values.norm() <= 1.0e-6f)
                    throw std::invalid_argument(
                        "vegetation instance rotation must be finite and non-zero");
                instance.rotation = Eigen::Quaternionf(
                    values.w(), values.x(), values.y(), values.z());
                instance.rotation.normalize();
            }
            if (entry.contains("scale"))
            {
                if (entry.at("scale").is_number())
                {
                    const float scale = ReadFiniteFloat(
                        entry.at("scale"), "vegetation instance scale");
                    instance.scale = Eigen::Vector3f::Constant(scale);
                }
                else
                {
                    ReadVec3(entry.at("scale"), instance.scale,
                        "vegetation instance scale");
                }
                if (instance.scale.cwiseAbs().minCoeff() <= 1.0e-6f)
                    throw std::invalid_argument(
                        "vegetation instance scale must be non-zero");
            }
            if (entry.contains("windPhase"))
                instance.windPhase = ReadFiniteFloat(
                    entry.at("windPhase"), "vegetation instance windPhase");
            if (entry.contains("lodBias"))
                instance.lodBias = ReadFiniteFloat(
                    entry.at("lodBias"), "vegetation instance lodBias");
            if (entry.contains("variant"))
                instance.variant = ReadUint32(
                    entry.at("variant"), "vegetation instance variant");
            if (entry.contains("seed"))
                instance.seed = ReadUint32(
                    entry.at("seed"), "vegetation instance seed");
            asset.instances.push_back(instance);
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("Invalid vegetation asset: ") + path.string() +
            ": " + exception.what();
        asset = VegetationAsset{};
        return false;
    }
}

std::vector<InstanceVertex> BuildVegetationInstanceVertices(
    const VegetationAsset& asset)
{
    std::vector<InstanceVertex> vertices;
    vertices.reserve(asset.instances.size());
    for (const VegetationInstance& instance : asset.instances)
    {
        InstanceVertex vertex;
        vertex.model = InstanceMatrix(instance);
        vertex.params = Eigen::Vector4f(
            instance.windPhase,
            instance.lodBias,
            static_cast<float>(instance.variant),
            static_cast<float>(instance.seed));
        vertices.push_back(vertex);
    }
    return vertices;
}

void AccumulateVegetationBounds(
    const VegetationAsset& asset,
    const Eigen::Vector3f& sectionBoundsMin,
    const Eigen::Vector3f& sectionBoundsMax,
    Eigen::Vector3f& boundsMin,
    Eigen::Vector3f& boundsMax)
{
    if (asset.instances.empty())
    {
        boundsMin = sectionBoundsMin;
        boundsMax = sectionBoundsMax;
        return;
    }
    boundsMin = Eigen::Vector3f(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    boundsMax = -boundsMin;
    for (const VegetationInstance& instance : asset.instances)
    {
        const Eigen::Matrix4f model = InstanceMatrix(instance);
        for (float x : {sectionBoundsMin.x(), sectionBoundsMax.x()})
            for (float y : {sectionBoundsMin.y(), sectionBoundsMax.y()})
                for (float z : {sectionBoundsMin.z(), sectionBoundsMax.z()})
                {
                    const Eigen::Vector3f corner =
                        (model * Eigen::Vector4f(x, y, z, 1.0f)).head<3>();
                    boundsMin = boundsMin.cwiseMin(corner);
                    boundsMax = boundsMax.cwiseMax(corner);
                }
    }
}

} // namespace VL
