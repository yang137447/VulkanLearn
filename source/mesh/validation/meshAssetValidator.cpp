#include "meshAssetValidator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "hair/tressfxSourceSchedule.h"
#include "hair/tressfxBehavior.h"

namespace
{
    uint32_t ReadSimulationInteger(const nlohmann::json& simulation,
        const char* field, uint32_t fallback, uint32_t minimum, uint32_t maximum,
        std::string_view assetPath)
    {
        if (!simulation.contains(field))
            return fallback;
        const auto& authored = simulation.at(field);
        // JSON 的 8.0 与 8 都是合法整数；必须先检查数值域，不能转换后截断或回绕。
        const double value = authored.is_number()
            ? authored.get<double>() : std::numeric_limits<double>::quiet_NaN();
        if (!std::isfinite(value) || std::floor(value) != value ||
            value < minimum || value > maximum)
            throw std::runtime_error("TressFX simulation " + std::string(field) +
                " must be an integer in [" + std::to_string(minimum) + ", " +
                std::to_string(maximum) + "]: " + std::string(assetPath));
        return static_cast<uint32_t>(value);
    }

    Eigen::Vector3f ReadSimulationVector(
        const nlohmann::json& json,
        std::string_view field,
        std::string_view meshAssetPath)
    {
        if (!json.contains(field) || !json.at(field).is_array() ||
            json.at(field).size() != 3)
            throw std::runtime_error(
                "TressFX simulation field must be a 3 element array: " +
                std::string(field) + " " + std::string(meshAssetPath));
        Eigen::Vector3f value;
        for (size_t i = 0; i < 3; ++i)
        {
            if (!json.at(field)[i].is_number())
                throw std::runtime_error(
                    "TressFX simulation vector contains a non-number: " +
                    std::string(field) + " " + std::string(meshAssetPath));
            value[static_cast<Eigen::Index>(i)] =
                json.at(field)[i].get<float>();
            if (!std::isfinite(value[static_cast<Eigen::Index>(i)]))
                throw std::runtime_error(
                    "TressFX simulation vector must be finite: " +
                    std::string(field) + " " + std::string(meshAssetPath));
        }
        return value;
    }

    // 作者逐顶点 stiffness 曲线：第 i 项 = 源曲线在 i/pointsPerStrand 处的采样值。
    // 这里只校验数值域与 64 项上限；与具体 .tfx 的 pointsPerStrand 等长由
    // TressfxModelImporter 在解析 TFX 后硬校验（描述符层拿不到该值）。
    std::vector<float> ReadSimulationStiffnessCurve(
        const nlohmann::json& simulation,
        const char* field,
        std::string_view meshAssetPath,
        bool clampToSourceLocalShapeLimit)
    {
        if (!simulation.contains(field))
            return {};
        const auto& array = simulation.at(field);
        if (!array.is_array())
            throw std::runtime_error(
                "TressFX simulation." + std::string(field) +
                " must be an array: " + std::string(meshAssetPath));
        if (array.empty())
            throw std::runtime_error(
                "TressFX simulation." + std::string(field) +
                " must not be empty; omit the field to use the fallback: " +
                std::string(meshAssetPath));
        if (array.size() > 64)
            throw std::runtime_error(
                "TressFX simulation." + std::string(field) +
                " supports at most 64 entries (one per strand point): " +
                std::string(meshAssetPath));
        std::vector<float> curve;
        curve.reserve(array.size());
        for (size_t index = 0; index < array.size(); ++index)
        {
            if (!array[index].is_number())
                throw std::runtime_error(
                    "TressFX simulation." + std::string(field) +
                    " contains a non-number at index " + std::to_string(index) +
                    ": " + std::string(meshAssetPath));
            const double value = array[index].get<double>();
            if (!std::isfinite(value) || value < 0.0 || value > 1.0)
                throw std::runtime_error(
                    "TressFX simulation." + std::string(field) +
                    " value at index " + std::to_string(index) +
                    " must be in [0, 1]: " + std::string(meshAssetPath));
            float stiffness = static_cast<float>(value);
            // 源在采样后立即 min(value, 0.95)（TressFXComponent.cpp:710）。
            // 加载期 clamp 并记录，避免静默改作者值。
            if (clampToSourceLocalShapeLimit &&
                stiffness > VL::Hair::TressfxSourceLocalShapeStiffnessLimit)
            {
                std::cerr << "[TressFX] simulation." << field << "[" << index << "] = "
                          << stiffness << " clamped to "
                          << VL::Hair::TressfxSourceLocalShapeStiffnessLimit
                          << " (TressFXComponent.cpp:710): " << meshAssetPath
                          << std::endl;
                stiffness = VL::Hair::TressfxSourceLocalShapeStiffnessLimit;
            }
            curve.push_back(stiffness);
        }
        return curve;
    }

    void ReadTressfxSimulation(
        const nlohmann::json& json,
        MeshAssetBuildPlan& loadPlan,
        std::string_view meshAssetPath)
    {
        if (!json.contains("simulation"))
            return;
        const auto& simulation = json.at("simulation");
        if (!simulation.is_object())
            throw std::runtime_error(
                "TressFX simulation must be an object: " + std::string(meshAssetPath));
        auto& config = loadPlan.tressfxSimulation;
        // 格式迁移：固定步 accumulator 已由 TressFX 的变量步长取代。静默忽略旧字段会让
        // 资产看起来仍然生效，因此在资产边界显式拒绝。
        for (const char* removed : {"fixedStep", "maxSubsteps"})
        {
            if (simulation.contains(removed))
                throw std::runtime_error(
                    "TressFX simulation " + std::string(removed) +
                    " was removed; use maxDt (one clamp(frameDelta, 0, maxDt) step per frame): " +
                    std::string(meshAssetPath));
        }
        // VL 的 windGust/windFrequency 与 TressFX SetWind 的帧调制叠加会形成双重
        // 调制。现在只保留 TressFX 语义，旧字段在资产边界显式拒绝而不是静默失效。
        for (const char* removed : {"windGust", "windFrequency"})
        {
            if (simulation.contains(removed))
                throw std::runtime_error(
                    "TressFX simulation " + std::string(removed) +
                    " was removed; wind modulation follows TressFX SetWind "
                    "(wM = |wind| * (sin(frame*0.001)^2 + 0.5)): " +
                    std::string(meshAssetPath));
        }
        // 风已迁到场景级 `wind` 对象（documents/contracts/scene-wind.md）。资产默认值、
        // 放置 simulation.overrides 与分组 overrides 共用本解析器，旧字段一律在
        // 边界拒绝并指向新的作者位置，不能静默失效成无风。
        if (simulation.contains("wind"))
            throw std::runtime_error(
                "TressFX simulation wind was removed; wind is authored by the scene "
                "wind object (type: \"wind\") instead: " + std::string(meshAssetPath));
        // simulation 与 schema 一样按封闭对象处理：未知键必须是硬错误，避免拼错
        // 的新曲线字段静默落回旧近似。
        {
            const char* known[] = {"enabled", "gravity", "damping",
                "iterations", "rootFixedPoints", "maxDt", "bendStiffness",
                "localShapeStiffness", "globalShapeStiffness",
                "globalShapeEffectiveRange", "vspCoefficient",
                "vspAccelThresholdMax", "vspStiffness", "backend", "skinning",
                "windGain"};
            for (auto field = simulation.begin(); field != simulation.end(); ++field)
            {
                if (std::find(std::begin(known), std::end(known), field.key()) ==
                    std::end(known))
                    throw std::runtime_error(
                        "TressFX simulation has unknown field \"" + field.key() +
                        "\": " + std::string(meshAssetPath));
            }
        }
        config.enabled = simulation.value("enabled", true);
        if (simulation.contains("gravity"))
            config.gravity = ReadSimulationVector(simulation, "gravity", meshAssetPath);
        config.damping = simulation.value("damping", config.damping);
        config.iterations = ReadSimulationInteger(
            simulation, "iterations", config.iterations, 1, 16, meshAssetPath);
        config.rootFixedPoints = ReadSimulationInteger(
            simulation, "rootFixedPoints", config.rootFixedPoints, 0, 32, meshAssetPath);
        config.maxDt = simulation.value("maxDt", config.maxDt);
        config.windGain = simulation.value("windGain", config.windGain);
        if (!std::isfinite(config.windGain) || config.windGain < 0.0f ||
            config.windGain > 10.0f)
            throw std::runtime_error(
                "TressFX simulation windGain must be finite in [0, 10]: " +
                std::string(meshAssetPath));
        config.bendStiffness = simulation.value("bendStiffness", config.bendStiffness);
        config.globalShapeStiffness =
            simulation.value("globalShapeStiffness", config.globalShapeStiffness);
        config.globalShapeEffectiveRange =
            simulation.value("globalShapeEffectiveRange", config.globalShapeEffectiveRange);
        config.vspCoefficient =
            simulation.value("vspCoefficient", config.vspCoefficient);
        config.vspAccelThresholdMax =
            simulation.value("vspAccelThresholdMax", config.vspAccelThresholdMax);
        config.vspStiffness = ReadSimulationStiffnessCurve(
            simulation, "vspStiffness", meshAssetPath, false);
        // LocalShape 源曲线在采样后立即 min(value, 0.95)，加载期 clamp 并记录。
        config.localShapeStiffness = ReadSimulationStiffnessCurve(
            simulation, "localShapeStiffness", meshAssetPath, true);
        if (simulation.contains("backend"))
        {
            // 只接受两个已实现的 backend；未知值必须在资产边界失败，而不是静默回退。
            const auto& backend = simulation.at("backend");
            const std::string value = backend.is_string()
                ? backend.get<std::string>() : std::string();
            if (value == "cpu")
                config.backend = TressfxSimulationBackend::Cpu;
            else if (value == "gpu")
                config.backend = TressfxSimulationBackend::Gpu;
            else
                throw std::runtime_error(
                    "TressFX simulation backend must be \"cpu\" or \"gpu\": " +
                    std::string(meshAssetPath));
        }
        if (simulation.contains("skinning"))
        {
            const auto& skinning = simulation.at("skinning");
            if (!skinning.is_object())
                throw std::runtime_error(
                    "TressFX simulation.skinning must be an object: " +
                    std::string(meshAssetPath));
            for (auto field = skinning.begin(); field != skinning.end(); ++field)
            {
                if (field.key() != "source" && field.key() != "rootBone" &&
                    field.key() != "boneWeights" && field.key() != "weights")
                    throw std::runtime_error(
                        "TressFX simulation.skinning has unknown field \"" +
                        field.key() + "\": " + std::string(meshAssetPath));
            }
            TressfxSimulationSkinning binding;
            // rootBone 始终必需；source 在“由蓝图 tressfx 组件提供绑定”时可以省略，
            // 由放置级信息补上。描述符层不能知道挂载方式，因此只校验“写了就必须合法”。
            if (skinning.contains("source") &&
                (!skinning.at("source").is_string() ||
                 skinning.at("source").get<std::string>().empty()))
                throw std::runtime_error(
                    "TressFX simulation.skinning.source must be a non-empty string: " +
                    std::string(meshAssetPath));
            if (skinning.contains("source"))
                binding.source = skinning.at("source").get<std::string>();
            if (!skinning.contains("rootBone") || !skinning.at("rootBone").is_string() ||
                skinning.at("rootBone").get<std::string>().empty())
                throw std::runtime_error(
                    "TressFX simulation.skinning requires a non-empty \"rootBone\": " +
                    std::string(meshAssetPath));
            binding.rootBone = skinning.at("rootBone").get<std::string>();
            if (skinning.contains("boneWeights"))
            {
                if (!skinning.at("boneWeights").is_string() ||
                    skinning.at("boneWeights").get<std::string>().empty())
                    throw std::runtime_error(
                        "TressFX simulation.skinning.boneWeights must be a non-empty "
                        "string path: " + std::string(meshAssetPath));
                binding.boneWeightsPath =
                    skinning.at("boneWeights").get<std::string>();
            }
            // 根点权重来源：缺省 rootBone 保持历史单骨；mesh 在加载期从绑定姿态
            // 身体派生。.tfxbone 是显式作者数据，两者同时出现会让“谁覆盖谁”
            // 变成隐式规则，因此在描述符边界拒绝而不是运行期择一。
            if (skinning.contains("weights"))
            {
                binding.rootWeightsAuthored = true;
                const auto& weights = skinning.at("weights");
                const std::string value = weights.is_string()
                    ? weights.get<std::string>() : std::string();
                if (value == "rootBone")
                    binding.rootWeights = TressfxRootWeightSource::RootBone;
                else if (value == "mesh")
                    binding.rootWeights = TressfxRootWeightSource::Mesh;
                else
                    throw std::runtime_error(
                        "TressFX simulation.skinning.weights must be \"rootBone\" or "
                        "\"mesh\": " + std::string(meshAssetPath));
            }
            if (binding.rootWeights == TressfxRootWeightSource::Mesh &&
                !binding.boneWeightsPath.empty())
                throw std::runtime_error(
                    "TressFX simulation.skinning.weights = \"mesh\" cannot be combined "
                    "with boneWeights: " + std::string(meshAssetPath));
            // 姿态来自渲染快照；CPU 参考 solver 运行在游戏线程，没有 SkinPose。
            // 这是作者错误，必须在加载期拒绝，而不是运行时静默退回 rest pin。
            if (config.backend != TressfxSimulationBackend::Gpu)
                throw std::runtime_error(
                    "TressFX simulation.skinning requires simulation.backend = \"gpu\": " +
                    std::string(meshAssetPath));
            config.skinning = std::move(binding);
        }
        // GPU 上一个作者 iteration 会展开成一次长 dispatch；上限防止异常值直接 TDR。
        // damping 是 TressFX 的 DampingCoeff（默认 0.035，UI 0..0.07），不是保留比例；
        // maxDt 不能超过 TressFX 的硬上限 0.016 s。
        if (!std::isfinite(config.damping) || config.damping < 0.0f ||
            config.damping > 1.0f ||
            !std::isfinite(config.maxDt) ||
            config.maxDt <= 0.0f ||
            config.maxDt > VL::Hair::TressfxMaxTimeStepSeconds)
            throw std::runtime_error(
                "TressFX simulation has invalid damping/rootFixedPoints/maxDt "
                "(damping is DampingCoeff in [0,1], maxDt in (0, 0.016]): " +
                std::string(meshAssetPath));
        if (!std::isfinite(config.bendStiffness) || config.bendStiffness < 0.0f ||
            config.bendStiffness > 1.0f)
            throw std::runtime_error(
                "TressFX simulation bendStiffness must be in [0, 1]: " +
                std::string(meshAssetPath));
        // global shape 的两个作者值都是 [0,1]；两者同时 > 0 才启用。
        if (!std::isfinite(config.globalShapeStiffness) ||
            config.globalShapeStiffness < 0.0f || config.globalShapeStiffness > 1.0f)
            throw std::runtime_error(
                "TressFX simulation globalShapeStiffness must be in [0, 1]: " +
                std::string(meshAssetPath));
        if (!std::isfinite(config.globalShapeEffectiveRange) ||
            config.globalShapeEffectiveRange < 0.0f ||
            config.globalShapeEffectiveRange > 1.0f)
            throw std::runtime_error(
                "TressFX simulation globalShapeEffectiveRange must be in [0, 1]: " +
                std::string(meshAssetPath));
        // VSP：系数为 0 表示 Host 完全不录制 VSP dispatch；阈值必须为正。
        if (!std::isfinite(config.vspCoefficient) || config.vspCoefficient < 0.0f ||
            config.vspCoefficient > 1.0f)
            throw std::runtime_error(
                "TressFX simulation vspCoefficient must be in [0, 1]: " +
                std::string(meshAssetPath));
        if (!std::isfinite(config.vspAccelThresholdMax) ||
            config.vspAccelThresholdMax <= 0.0f || config.vspAccelThresholdMax > 1.0e7f)
            throw std::runtime_error(
                "TressFX simulation vspAccelThresholdMax must be in (0, 1e7]: " +
                std::string(meshAssetPath));
    }

    void ReadTressfxCollision(
        const nlohmann::json& json,
        MeshAssetBuildPlan& loadPlan,
        std::string_view meshAssetPath)
    {
        auto& capsules = loadPlan.tressfxSimulation.capsules;
        capsules.clear();
        loadPlan.tressfxSimulation.collisionMesh.reset();
        if (!json.contains("collision"))
            return;
        const auto& collision = json.at("collision");
        if (!collision.is_object())
            throw std::runtime_error(
                "TressFX collision must be an object: " + std::string(meshAssetPath));
        for (auto field = collision.begin(); field != collision.end(); ++field)
        {
            if (field.key() != "capsules" && field.key() != "mesh")
                throw std::runtime_error(
                    "TressFX collision has unknown field \"" + field.key() + "\": " +
                    std::string(meshAssetPath));
        }
        if (collision.contains("capsules") && !collision.at("capsules").is_array())
            throw std::runtime_error(
                "TressFX collision requires a capsules array: " +
                std::string(meshAssetPath));
        const auto array = collision.value("capsules", nlohmann::json::array());
        if (array.size() > VL::Hair::TressfxMaxColliders)
            throw std::runtime_error(
                "TressFX collision supports at most " +
                std::to_string(VL::Hair::TressfxMaxColliders) + " capsules: " +
                std::string(meshAssetPath));
        for (const auto& item : array)
        {
            if (!item.is_object())
                throw std::runtime_error(
                    "TressFX collision capsule must be an object: " +
                    std::string(meshAssetPath));
            for (auto field = item.begin(); field != item.end(); ++field)
            {
                if (field.key() != "a" && field.key() != "b" && field.key() != "radius" &&
                    field.key() != "bone")
                    throw std::runtime_error(
                        "TressFX collision capsule has unknown field \"" +
                        field.key() + "\": " + std::string(meshAssetPath));
            }
            if (!item.contains("a") || !item.contains("b") || !item.contains("radius"))
                throw std::runtime_error(
                    "TressFX collision capsule requires a, b and radius: " +
                    std::string(meshAssetPath));
            VL::Hair::TressfxCollider collider;
            collider.a = ReadSimulationVector(item, "a", meshAssetPath);
            collider.b = ReadSimulationVector(item, "b", meshAssetPath);
            if (!item.at("radius").is_number())
                throw std::runtime_error(
                    "TressFX collision capsule radius must be a number: " +
                    std::string(meshAssetPath));
            collider.radius = item.at("radius").get<float>();
            if (!std::isfinite(collider.radius) || collider.radius <= 0.0f)
                throw std::runtime_error(
                    "TressFX collision capsule radius must be finite and positive: " +
                    std::string(meshAssetPath));
            // 可选骨骼绑定：作者空间仍是绑定空间，运行期只按该关节的
            // bind→pose 矩阵变换端点。空字符串不是“未绑定”的合法写法。
            if (item.contains("bone"))
            {
                if (!item.at("bone").is_string() ||
                    item.at("bone").get<std::string>().empty())
                    throw std::runtime_error(
                        "TressFX collision capsule bone must be a non-empty string: " +
                        std::string(meshAssetPath));
                collider.bone = item.at("bone").get<std::string>();
            }
            capsules.push_back(collider);
        }
        // 骨骼绑定需要源对象骨架来解析关节；声明 bone 却没有 skinning.source
        // 会让运行期无矩阵可用，必须在加载期拒绝而不是静默保持静态。
        // 同理，CPU 参考 solver 没有渲染线程的 SkinPose。
        bool anyCapsuleBone = false;
        for (const VL::Hair::TressfxCollider& capsule : capsules)
            anyCapsuleBone |= !capsule.bone.empty();
        if (anyCapsuleBone && !loadPlan.tressfxSimulation.skinning)
            throw std::runtime_error(
                "TressFX collision capsule bone requires simulation.skinning "
                "(source and rootBone): " + std::string(meshAssetPath));
        if (anyCapsuleBone &&
            loadPlan.tressfxSimulation.backend != TressfxSimulationBackend::Gpu)
            throw std::runtime_error(
                "TressFX collision capsule bone requires simulation.backend = \"gpu\": " +
                std::string(meshAssetPath));

        if (!collision.contains("mesh"))
            return;
        const auto& mesh = collision.at("mesh");
        if (!mesh.is_object())
            throw std::runtime_error(
                "TressFX collision.mesh must be an object: " + std::string(meshAssetPath));
        for (auto field = mesh.begin(); field != mesh.end(); ++field)
        {
            if (field.key() != "path" && field.key() != "cellSize" && field.key() != "margin")
                throw std::runtime_error(
                    "TressFX collision.mesh has unknown field \"" + field.key() + "\": " +
                    std::string(meshAssetPath));
        }
        if (!mesh.contains("path") || !mesh.at("path").is_string() ||
            mesh.at("path").get<std::string>().empty())
            throw std::runtime_error(
                "TressFX collision.mesh requires a non-empty path: " +
                std::string(meshAssetPath));
        TressfxCollisionMeshConfig collisionMesh;
        collisionMesh.path = mesh.at("path").get<std::string>();
        // 代理路径与模型/材质路径同一规则：资源根相对、不允许绝对路径或 ".." 逃逸。
        const std::filesystem::path collisionPath(collisionMesh.path);
        if (collisionPath.is_absolute() || collisionPath.has_root_directory())
            throw std::runtime_error(
                "TressFX collision.mesh path must be resource-relative: " +
                std::string(meshAssetPath) + " path=" + collisionMesh.path);
        for (const std::filesystem::path& part : collisionPath)
        {
            if (part == "..")
                throw std::runtime_error(
                    "TressFX collision.mesh path must not escape the resource root: " +
                    std::string(meshAssetPath) + " path=" + collisionMesh.path);
        }
        // 两个作者值都有默认值；显式给出时必须落在合同范围内，错误消息带路径。
        if (mesh.contains("cellSize"))
        {
            if (!mesh.at("cellSize").is_number())
                throw std::runtime_error(
                    "TressFX collision.mesh cellSize must be a number: " +
                    std::string(meshAssetPath) + " path=" + collisionMesh.path);
            collisionMesh.cellSize = mesh.at("cellSize").get<float>();
        }
        if (!std::isfinite(collisionMesh.cellSize) || !(collisionMesh.cellSize > 0.2f) ||
            collisionMesh.cellSize > 4.0f)
            throw std::runtime_error(
                "TressFX collision.mesh cellSize must be in (0.2, 4] cm: " +
                std::string(meshAssetPath) + " path=" + collisionMesh.path);
        if (mesh.contains("margin"))
        {
            if (!mesh.at("margin").is_number())
                throw std::runtime_error(
                    "TressFX collision.mesh margin must be a number: " +
                    std::string(meshAssetPath) + " path=" + collisionMesh.path);
            collisionMesh.margin = mesh.at("margin").get<float>();
        }
        if (!std::isfinite(collisionMesh.margin) || collisionMesh.margin < 0.05f ||
            collisionMesh.margin > 4.0f)
            throw std::runtime_error(
                "TressFX collision.mesh margin must be in [0.05, 4] cm: " +
                std::string(meshAssetPath) + " path=" + collisionMesh.path);
        // SDF 重建与碰撞 pass 只存在于 GPU backend；CPU 参考 solver 没有距离场，
        // 静默忽略作者块会让资产看起来生效但实际没有碰撞。
        if (loadPlan.tressfxSimulation.backend != TressfxSimulationBackend::Gpu)
            throw std::runtime_error(
                "TressFX collision.mesh requires simulation.backend = \"gpu\": " +
                std::string(meshAssetPath) + " path=" + collisionMesh.path);
        loadPlan.tressfxSimulation.collisionMesh = std::move(collisionMesh);
    }

    void EnsureObject(const nlohmann::json& json, std::string_view meshAssetPath)
    {
        if (!json.is_object())
        {
            throw std::runtime_error("Mesh asset must be a JSON object: " + std::string(meshAssetPath));
        }
    }

    std::string ReadRequiredString(
        const nlohmann::json& json,
        std::string_view fieldName,
        std::string_view meshAssetPath)
    {
        const std::string field(fieldName);
        if (!json.contains(field) || !json[field].is_string())
        {
            throw std::runtime_error(
                "Mesh asset is missing string field \"" + field + "\": " +
                std::string(meshAssetPath));
        }

        const std::string value = json[field].get<std::string>();
        if (value.empty())
        {
            throw std::runtime_error(
                "Mesh asset field \"" + field + "\" must not be empty: " +
                std::string(meshAssetPath));
        }
        return value;
    }

    std::vector<MeshMaterialSlot> ReadMaterialSlots(
        const nlohmann::json& json,
        std::string_view meshAssetPath,
        std::string_view modelDataPath)
    {
        if (!json.contains("materialSlots"))
        {
            throw std::runtime_error(
                "Mesh asset is missing required materialSlots. Migrate materialInstancePath to materialSlots: " +
                std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
        }
        if (!json["materialSlots"].is_array() || json["materialSlots"].empty())
        {
            throw std::runtime_error(
                "Mesh asset materialSlots must be a non-empty ordered array: " +
                std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
        }

        std::vector<MeshMaterialSlot> materialSlots;
        for (size_t slotIndex = 0; slotIndex < json["materialSlots"].size(); ++slotIndex)
        {
            const auto& item = json["materialSlots"][slotIndex];
            if (!item.is_object())
            {
                throw std::runtime_error(
                    "Mesh asset material slot must be an object: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slotIndex=" + std::to_string(slotIndex));
            }
            if (!item.contains("name") || !item["name"].is_string())
            {
                throw std::runtime_error(
                    "Mesh asset material slot is missing string field name: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slotIndex=" + std::to_string(slotIndex));
            }
            const std::string slotName = item["name"].get<std::string>();
            if (slotName.empty())
            {
                throw std::runtime_error(
                    "Mesh asset material slot name must not be empty: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slotIndex=" + std::to_string(slotIndex));
            }
            bool bHasDuplicateSlotName = false;
            for (const MeshMaterialSlot& materialSlot : materialSlots)
            {
                if (materialSlot.name == slotName)
                {
                    bHasDuplicateSlotName = true;
                    break;
                }
            }
            if (bHasDuplicateSlotName)
            {
                throw std::runtime_error(
                    "Mesh asset materialSlots contains a duplicate slot name: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slot=" + slotName);
            }
            if (!item.contains("materialInstancePath") || !item["materialInstancePath"].is_string())
            {
                throw std::runtime_error(
                    "Mesh asset material slot is missing string field materialInstancePath: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slot=" + slotName);
            }

            const std::string materialInstancePath = item["materialInstancePath"].get<std::string>();
            if (materialInstancePath.empty())
            {
                throw std::runtime_error(
                    "Mesh asset material slot path must not be empty: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath) +
                    " slot=" + slotName);
            }
            MeshMaterialSlot materialSlot;
            materialSlot.name = slotName;
            materialSlot.materialInstancePath = materialInstancePath;
            materialSlots.push_back(materialSlot);
        }
        return materialSlots;
    }

    MeshImportOptions ReadImportOptions(
        const nlohmann::json& json,
        std::string_view meshAssetPath,
        std::string_view modelDataPath)
    {
        MeshImportOptions importOptions;
        if (!json.contains("importOptions"))
        {
            return importOptions;
        }
        if (!json["importOptions"].is_object())
        {
            throw std::runtime_error(
                "Mesh asset importOptions must be an object: " +
                std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
        }

        const auto& optionsJson = json["importOptions"];
        if (optionsJson.contains("generateSmoothNormals"))
        {
            if (!optionsJson["generateSmoothNormals"].is_boolean())
            {
                throw std::runtime_error(
                    "Mesh asset importOptions.generateSmoothNormals must be a boolean: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
            }
            importOptions.generateSmoothNormals = optionsJson["generateSmoothNormals"].get<bool>();
        }
        if (optionsJson.contains("includedSectionNames"))
        {
            if (!optionsJson["includedSectionNames"].is_array() ||
                optionsJson["includedSectionNames"].empty())
            {
                throw std::runtime_error(
                    "Mesh asset importOptions.includedSectionNames must be a non-empty array of strings: " +
                    std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
            }
            for (const auto& sectionName : optionsJson["includedSectionNames"])
            {
                if (!sectionName.is_string() || sectionName.get<std::string>().empty())
                {
                    throw std::runtime_error(
                        "Mesh asset importOptions.includedSectionNames must contain non-empty strings: " +
                        std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
                }
                importOptions.includedSectionNames.push_back(sectionName.get<std::string>());
            }
        }
        return importOptions;
    }

    std::string ReadOptionalAnimationJsonPath(
        const nlohmann::json& json,
        std::string_view fieldName,
        std::string_view meshAssetPath,
        std::string_view modelDataPath)
    {
        const std::string field(fieldName);
        if (!json.contains(field))
        {
            return {};
        }
        if (!json.at(field).is_string())
        {
            throw std::runtime_error(
                "Mesh asset field \"" + field + "\" must be a resource-relative JSON path: " +
                std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
        }
        const std::string value = json.at(field).get<std::string>();
        if (value.empty())
        {
            throw std::runtime_error(
                "Mesh asset field \"" + field + "\" must not be empty: " +
                std::string(meshAssetPath) + " model=" + std::string(modelDataPath));
        }
        const std::filesystem::path path(value);
        if (path.is_absolute() || path.has_root_directory())
        {
            throw std::runtime_error(
                "Mesh asset field \"" + field + "\" must be resource-relative: " +
                std::string(meshAssetPath) + " value=" + value);
        }
        for (const std::filesystem::path& part : path)
        {
            if (part == "..")
            {
                throw std::runtime_error(
                    "Mesh asset field \"" + field + "\" must not escape its resource root: " +
                    std::string(meshAssetPath) + " value=" + value);
            }
        }
        if (path.extension() != ".json")
        {
            throw std::runtime_error(
                "Mesh asset field \"" + field + "\" must reference a .json asset: " +
                std::string(meshAssetPath) + " value=" + value);
        }
        return value;
    }

    std::vector<std::string> BuildUniqueSourceMaterialSlots(const ModelResource& modelResource)
    {
        if (!modelResource.sourceMaterialSlotNames.empty())
        {
            return modelResource.sourceMaterialSlotNames;
        }

        std::vector<std::string> sourceSlots;
        for (const MeshSection& section : modelResource.sections)
        {
            const auto it = std::find(sourceSlots.begin(), sourceSlots.end(), section.materialSlotName);
            if (it == sourceSlots.end())
            {
                sourceSlots.push_back(section.materialSlotName);
            }
        }
        return sourceSlots;
    }

    size_t FindSourceSlotIndex(const std::vector<std::string>& sourceSlots, const std::string& slotName)
    {
        const auto it = std::find(sourceSlots.begin(), sourceSlots.end(), slotName);
        if (it == sourceSlots.end())
        {
            return sourceSlots.size();
        }
        return static_cast<size_t>(std::distance(sourceSlots.begin(), it));
    }

    std::string BuildSlotCountMismatchReason(
        const MeshAssetBuildPlan& buildPlan,
        size_t sourceSlotCount,
        bool bIsStrictMatch)
    {
        std::string reason = bIsStrictMatch
            ? "SpeedTree materialSlots count does not match parsed .stsdk selected LOD material slot count"
            : "Mesh materialSlots count does not match source material slot count";
        reason += ": " + buildPlan.meshAssetPath +
            " model=" + buildPlan.modelDataPath +
            " materialSlots=" + std::to_string(buildPlan.materialSlots.size()) +
            " parsedSlots=" + std::to_string(sourceSlotCount);
        return reason;
    }

    // guide 索引必须是 [0, 2^32) 内的整数（JSON 的 4.0 与 4 等价，true/字符串拒绝）。
    uint32_t ReadGuideGroupIndex(const nlohmann::json& value, std::string_view meshAssetPath)
    {
        const double number = value.is_number()
            ? value.get<double>() : std::numeric_limits<double>::quiet_NaN();
        if (!std::isfinite(number) || std::floor(number) != number ||
            number < 0.0 || number >= 4294967296.0)
            throw std::runtime_error(
                "TressFX groups guide index must be a non-negative integer: " +
                std::string(meshAssetPath));
        return static_cast<uint32_t>(number);
    }

    // 分组 overrides 的键集与值域和放置 simulation.overrides 一致。键集取编辑器实时
    // 参数表；值域不在此重写，而是交给 simulation 读取器（与放置路径同一份代码）。
    // enabled、skinning、collision 等行为字段在键集检查中直接拒绝。
    void ValidateGuideGroupOverrides(
        const nlohmann::json& overrides,
        const std::string& groupName,
        std::string_view meshAssetPath)
    {
        const std::string context = "TressFX group \"" + groupName + "\": " +
            std::string(meshAssetPath);
        if (!overrides.is_object())
            throw std::runtime_error(
                "TressFX group simulationOverrides must be an object: " + context);
        // wind 已迁到场景 `wind` 对象；分组层给出与资产默认值/放置 overrides 一致的
        // 迁移错误，而不是笼统的“键不在允许表”。
        if (overrides.contains("wind"))
            throw std::runtime_error(
                "TressFX group simulationOverrides field \"wind\" was removed; wind is "
                "authored by the scene wind object (type: \"wind\"): " + context);
        // gravity、iterations 是引擎公共参数，不按组覆盖；wind 属于场景 `wind` 对象，
        // 同样不会是组级标量。
        static const char* const allowed[] = {"damping", "bendStiffness",
            "globalShapeStiffness", "globalShapeEffectiveRange", "vspCoefficient",
            "vspAccelThresholdMax"};
        for (auto field = overrides.begin(); field != overrides.end(); ++field)
        {
            bool known = false;
            for (const char* name : allowed)
                known = known || field.key() == name;
            if (!known)
                throw std::runtime_error(
                    "TressFX group simulationOverrides field \"" + field.key() +
                    "\" is not allowed (enabled, skinning and collision belong to the "
                    "asset defaults or placement): " + context);
        }
        // 结果对象丢弃：这里只借用数值校验，不把组覆盖提前合并进资产默认值。
        MeshAssetValidator::BuildTressfxSimulationConfig({{"simulation", overrides}}, context);
    }

    struct OwnedGuideRange
    {
        uint32_t start;
        uint32_t end;
        size_t group;
    };

    bool OwnedGuideRangeStartsBefore(const OwnedGuideRange& left, const OwnedGuideRange& right)
    {
        return left.start != right.start ? left.start < right.start : left.end < right.end;
    }

    // 名称规则必须与编辑器 Groups 控制器一致：Trim 后非空、大小写不敏感唯一，
    // 且 `Default`/`unassigned` 保留给隐式组 0。否则运行期合法的资产会在编辑器
    // 里加载失败，随后一次保存就可能把整份 groups 清空。
    std::string TrimGroupName(std::string_view text)
    {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
        return std::string(text.substr(begin, end - begin));
    }

    std::string FoldGroupName(std::string_view text)
    {
        std::string result(text);
        for (char& ch : result)
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        return result;
    }

    bool IsReservedGroupName(std::string_view name)
    {
        const std::string folded = FoldGroupName(name);
        return folded == "default" || folded == "unassigned";
    }

    // TF groups[] 的描述符层校验：名称非空且唯一，每组 guideRanges 为非空的 [start,end) 对且
    // 组内不重叠，跨组也不重叠。guide 上界（end <= guideCount）依赖 .tfx 解析结果，
    // 由 TressfxModelImporter 在导入时检查。
    // 总区间上限必须与编辑器 TressfxGroupEditor::MaxGuideRanges 一致；笔刷一次可能产生
    // 大量不连续区间，资产层若放宽会让编辑器保存后的结果反而无法通过下一次导入。
    constexpr size_t MaxTressfxGuideRanges = 4096;
    std::vector<TressfxGuideGroupPlan> ReadTressfxGuideGroups(
        const nlohmann::json& json, std::string_view meshAssetPath)
    {
        std::vector<TressfxGuideGroupPlan> groups;
        if (!json.contains("groups"))
            return groups;
        const auto& array = json.at("groups");
        if (!array.is_array())
            throw std::runtime_error(
                "TressFX groups must be an array when present: " +
                std::string(meshAssetPath));
        // 空数组与未声明 groups 语义相同（全部 guide 属于默认组 0）；
        // 编辑器也把“删除最后一个组”写成移除键，两条路径保持一致。
        if (array.empty())
            return groups;

        std::vector<OwnedGuideRange> ranges;
        std::vector<std::string> names;
        for (size_t index = 0; index < array.size(); ++index)
        {
            const auto& node = array[index];
            if (!node.is_object())
                throw std::runtime_error(
                    "TressFX group must be an object: " + std::string(meshAssetPath));
            for (auto field = node.begin(); field != node.end(); ++field)
            {
                if (field.key() != "name" && field.key() != "guideRanges" &&
                    field.key() != "simulationOverrides")
                    throw std::runtime_error(
                        "TressFX group has unknown field \"" + field.key() +
                        "\": " + std::string(meshAssetPath));
            }
            if (!node.contains("name") || !node.at("name").is_string())
                throw std::runtime_error(
                    "TressFX group requires a non-empty string name: " +
                    std::string(meshAssetPath));
            TressfxGuideGroupPlan group;
            group.name = node.at("name").get<std::string>();
            const std::string trimmed = TrimGroupName(group.name);
            if (trimmed.empty() || trimmed != group.name)
                throw std::runtime_error(
                    "TressFX group name must be non-empty without leading or trailing "
                    "whitespace: \"" + group.name + "\": " + std::string(meshAssetPath));
            if (IsReservedGroupName(trimmed))
                throw std::runtime_error(
                    "TressFX group name \"" + trimmed + "\" is reserved for the implicit "
                    "default group: " + std::string(meshAssetPath));
            const std::string folded = FoldGroupName(trimmed);
            if (std::find(names.begin(), names.end(), folded) != names.end())
                throw std::runtime_error(
                    "TressFX group name is duplicated: \"" + group.name + "\": " +
                    std::string(meshAssetPath));
            names.push_back(folded);

            const std::string context = "TressFX group \"" + group.name + "\": " +
                std::string(meshAssetPath);
            // 空组是合法作者状态：新建组可以先没有 guide，之后用笔刷填充。
            if (!node.contains("guideRanges") || !node.at("guideRanges").is_array())
                throw std::runtime_error(
                    "TressFX group guideRanges must be an array of [start, end] pairs: " +
                    context);
            for (const auto& pair : node.at("guideRanges"))
            {
                if (!pair.is_array() || pair.size() != 2)
                    throw std::runtime_error(
                        "TressFX group guideRanges entry must be a [start, end] pair: " + context);
                const uint32_t start = ReadGuideGroupIndex(pair[0], context);
                const uint32_t end = ReadGuideGroupIndex(pair[1], context);
                if (start >= end)
                    throw std::runtime_error(
                        "TressFX group guideRanges requires 0 <= start < end (half-open): " +
                        context);
                group.guideRanges.emplace_back(start, end);
                ranges.push_back({start, end, index});
            }
            if (node.contains("simulationOverrides"))
            {
                ValidateGuideGroupOverrides(
                    node.at("simulationOverrides"), group.name, meshAssetPath);
                group.simulationOverrides = node.at("simulationOverrides");
            }
            groups.push_back(std::move(group));
        }

        if (ranges.size() > MaxTressfxGuideRanges)
            throw std::runtime_error(
                "TressFX groups contain " + std::to_string(ranges.size()) +
                " guide ranges; the limit is " +
                std::to_string(MaxTressfxGuideRanges) + ": " +
                std::string(meshAssetPath));

        // 按起点扫描并记录已出现区间的最大终点；半开区间首尾相接不算重叠。
        std::sort(ranges.begin(), ranges.end(), OwnedGuideRangeStartsBefore);
        if (!ranges.empty())
        {
            size_t owner = ranges.front().group;
            uint32_t maxEnd = ranges.front().end;
            for (size_t i = 1; i < ranges.size(); ++i)
            {
                if (ranges[i].start < maxEnd)
                {
                    const std::string first = groups[owner].name;
                    const std::string second = groups[ranges[i].group].name;
                    throw std::runtime_error(
                        (owner == ranges[i].group
                            ? "TressFX group guideRanges overlap within group \"" + first + "\""
                            : "TressFX guide belongs to two groups: \"" + first +
                                "\" and \"" + second + "\"") +
                        ": " + std::string(meshAssetPath));
                }
                if (ranges[i].end > maxEnd)
                {
                    maxEnd = ranges[i].end;
                    owner = ranges[i].group;
                }
            }
        }
        return groups;
    }

    // 分组绑定身份：名称与区间决定每根 guide 的分组号，但 ribbon 顶点/索引与它们无关。
    // 因此这份身份不进入几何 modelCacheKey，避免一次组编辑触发整份 .tfx 重新导入；
    // RendererMeshLoader 把它拼进 renderable key，Phase 2 后组只更新 group buffer。
    // overrides 不影响导入结果，也不进入绑定身份。
    std::string TressfxGuideGroupIdentity(const std::vector<TressfxGuideGroupPlan>& groups)
    {
        nlohmann::json identity = nlohmann::json::array();
        for (const auto& group : groups)
        {
            nlohmann::json ranges = nlohmann::json::array();
            for (const auto& range : group.guideRanges)
                ranges.push_back({range.first, range.second});
            identity.push_back({{"name", group.name}, {"guideRanges", ranges}});
        }
        return identity.dump();
    }
}

TressfxSimulationConfig MeshAssetValidator::BuildTressfxSimulationConfig(
    const nlohmann::json& configuration, std::string_view context)
{
    MeshAssetBuildPlan plan;
    ReadTressfxSimulation(configuration, plan, context);
    ReadTressfxCollision(configuration, plan, context);
    return plan.tressfxSimulation;
}

MeshAssetBuildPlan MeshAssetValidator::BuildLoadPlan(
    std::string_view meshAssetPath,
    const nlohmann::json& effectiveMeshAssetJson)
{
    EnsureObject(effectiveMeshAssetJson, meshAssetPath);

    if (effectiveMeshAssetJson.contains("materialInstancePath"))
    {
        throw std::runtime_error(
            "Mesh asset field materialInstancePath is no longer supported. Use materialSlots instead: " +
            std::string(meshAssetPath));
    }

    MeshAssetBuildPlan loadPlan;
    loadPlan.assetType = ReadRequiredString(effectiveMeshAssetJson, "type", meshAssetPath);
    if (loadPlan.assetType != "mesh" && loadPlan.assetType != "speedtree" &&
        loadPlan.assetType != "tressfx")
    {
        throw std::runtime_error(
            "Mesh asset type must be \"mesh\", \"speedtree\" or \"tressfx\": " +
            std::string(meshAssetPath));
    }
    loadPlan.meshAssetPath = std::string(meshAssetPath);
    loadPlan.modelDataPath = ReadRequiredString(effectiveMeshAssetJson, "modelDataPath", meshAssetPath);
    loadPlan.importOptions = ReadImportOptions(
        effectiveMeshAssetJson,
        meshAssetPath,
        loadPlan.modelDataPath);
    loadPlan.materialSlots = ReadMaterialSlots(effectiveMeshAssetJson, meshAssetPath, loadPlan.modelDataPath);
    loadPlan.modelCacheKey = loadPlan.modelDataPath;
    if (loadPlan.assetType == "tressfx")
    {
        // 半径是作者数据，在描述符边界收口；shader 不逐帧修补非法宽度。
        if (!effectiveMeshAssetJson.contains("strandRadius") ||
            !effectiveMeshAssetJson.at("strandRadius").is_number())
            throw std::runtime_error("TressFX mesh requires numeric strandRadius: " +
                std::string(meshAssetPath));
        const double radius = effectiveMeshAssetJson.at("strandRadius").get<double>();
        loadPlan.strandRadius = static_cast<float>(radius);
        if (!std::isfinite(radius) || !std::isfinite(loadPlan.strandRadius) ||
            loadPlan.strandRadius <= 0.0f)
            throw std::runtime_error("TressFX strandRadius must be finite and positive: " +
                std::string(meshAssetPath));
        // 阻力的质量归一化需要半径；把它随已校验的 simulation 配置带进运行时。
        loadPlan.tressfxSimulation.strandRadius = loadPlan.strandRadius;
        if (std::filesystem::path(loadPlan.modelDataPath).extension() != ".tfx" ||
            loadPlan.materialSlots.size() != 1 || effectiveMeshAssetJson.contains("importOptions"))
            throw std::runtime_error(
                "TressFX requires a .tfx source, one material slot and no importOptions: " +
                std::string(meshAssetPath));
        loadPlan.tressfxSimulation = VL::Hair::ReadTressfxSimulationDefaults(
            effectiveMeshAssetJson, std::string(meshAssetPath));
        loadPlan.tressfxGuideGroups = ReadTressfxGuideGroups(
            effectiveMeshAssetJson, meshAssetPath);
        std::ostringstream identity;
        // 几何身份只含几何输入；默认值变更在 renderable 有效配置键中体现。
        identity << "|tressfx-geometry-v2|radius=" << std::hexfloat << loadPlan.strandRadius;
        loadPlan.modelCacheKey += identity.str();
        if (!loadPlan.tressfxGuideGroups.empty())
            loadPlan.tressfxGroupBindingKey =
                TressfxGuideGroupIdentity(loadPlan.tressfxGuideGroups);
    }
    else if (effectiveMeshAssetJson.contains("groups"))
        throw std::runtime_error(
            "Mesh asset field groups is only supported on tressfx assets: " +
            std::string(meshAssetPath));
    if (effectiveMeshAssetJson.contains("skinning"))
    {
        const auto& skinning = effectiveMeshAssetJson.at("skinning");
        if (loadPlan.assetType != "mesh" || !skinning.is_object() ||
            !skinning.contains("clips") || !skinning.at("clips").is_array())
            throw std::runtime_error(
                "Mesh skinning requires an object with a clips array: " +
                std::string(meshAssetPath));
        if (loadPlan.importOptions.generateSmoothNormals ||
            !loadPlan.importOptions.includedSectionNames.empty())
            throw std::runtime_error("Skeletal import does not support static importOptions");

        // skinning.root 已删除。显式拒绝而不是静默忽略，避免旧资源带着错误的
        // 根骨假设继续导入；根骨唯一来源是 skinning.skeleton 指向的 SK。
        if (skinning.contains("root"))
            throw std::runtime_error(
                "Mesh skinning.root is no longer supported; use skinning.skeleton: " +
                std::string(meshAssetPath));
        loadPlan.skeletonPath = ReadOptionalAnimationJsonPath(
            skinning, "skeleton", meshAssetPath, loadPlan.modelDataPath);
        if (loadPlan.skeletonPath.empty())
            throw std::runtime_error(
                "Mesh skinning requires skinning.skeleton to identify the skeleton: " +
                std::string(meshAssetPath));
        loadPlan.modelCacheKey += "|skeleton=" + loadPlan.skeletonPath;

        if (skinning.contains("sourceModel"))
        {
            loadPlan.animationSourceModelPath = ReadOptionalAnimationJsonPath(
                skinning, "sourceModel", meshAssetPath, loadPlan.modelDataPath);
            if (loadPlan.animationSourceModelPath == loadPlan.meshAssetPath)
                throw std::runtime_error(
                    "Mesh skinning.sourceModel must not reference the target mesh asset: " +
                    std::string(meshAssetPath));
            loadPlan.modelCacheKey += "|animationSourceModel=" +
                loadPlan.animationSourceModelPath;
            if (!skinning.contains("retarget"))
                throw std::runtime_error(
                    "Mesh skinning.sourceModel requires skinning.retarget: " +
                    std::string(meshAssetPath));
        }
        if (skinning.contains("retarget"))
        {
            loadPlan.retargetProfilePath = ReadOptionalAnimationJsonPath(
                skinning, "retarget", meshAssetPath, loadPlan.modelDataPath);
            if (loadPlan.animationSourceModelPath.empty())
                throw std::runtime_error(
                    "Mesh skinning.retarget requires skinning.sourceModel: " +
                    std::string(meshAssetPath));
            loadPlan.modelCacheKey += "|retarget=" + loadPlan.retargetProfilePath;
        }

        for (const auto& clip : skinning.at("clips"))
        {
            if (!clip.is_object() || !clip.contains("id") || !clip.at("id").is_string() ||
                clip.at("id").get<std::string>().empty() ||
                !clip.contains("path") || !clip.at("path").is_string() ||
                clip.at("path").get<std::string>().empty())
                throw std::runtime_error("Skeletal clip requires nonempty id/path: " +
                    std::string(meshAssetPath));
            MeshAnimationClipPlan clipPlan;
            clipPlan.id = clip.at("id").get<std::string>();
            clipPlan.path = clip.at("path").get<std::string>();
            for (const auto& existing : loadPlan.animationClips)
                if (existing.id == clipPlan.id)
                    throw std::runtime_error("Duplicate skeletal clip id: " + clipPlan.id);
            loadPlan.modelCacheKey += "|clip=" + clipPlan.id + ":" + clipPlan.path;
            // sequence 是可选 sidecar；缺失表示该 clip 没有曲线/notify，行为与旧资源一致。
            if (clip.contains("sequence"))
            {
                clipPlan.sequencePath = ReadOptionalAnimationJsonPath(
                    clip, "sequence", meshAssetPath, loadPlan.modelDataPath);
                loadPlan.modelCacheKey +=
                    "|sequence=" + clipPlan.id + ":" + clipPlan.sequencePath;
            }
            loadPlan.animationClips.push_back(std::move(clipPlan));
        }

        // montages 是可选清单；路径级 key 与 clip/sequence 同规则，内容摘要由 loader 追加。
        // 上方已强制要求 skinning.skeleton，因此 Montage 的 slot/骨名校验总有 SK 可用。
        if (skinning.contains("montages"))
        {
            if (!skinning.at("montages").is_array())
                throw std::runtime_error(
                    "Mesh skinning.montages must be an array: " +
                    std::string(meshAssetPath));
            for (const auto& montage : skinning.at("montages"))
            {
                if (!montage.is_object() || !montage.contains("id") ||
                    !montage.at("id").is_string() ||
                    montage.at("id").get<std::string>().empty() ||
                    !montage.contains("path") || !montage.at("path").is_string() ||
                    montage.at("path").get<std::string>().empty())
                    throw std::runtime_error(
                        "Skeletal montage requires nonempty id/path: " +
                        std::string(meshAssetPath));
                MeshAnimationMontagePlan montagePlan;
                montagePlan.id = montage.at("id").get<std::string>();
                for (const auto& existing : loadPlan.animationMontages)
                    if (existing.id == montagePlan.id)
                        throw std::runtime_error(
                            "Duplicate skeletal montage id: " + montagePlan.id);
                montagePlan.path = ReadOptionalAnimationJsonPath(
                    montage, "path", meshAssetPath, loadPlan.modelDataPath);
                loadPlan.modelCacheKey += "|montage=" + montagePlan.path;
                loadPlan.animationMontages.push_back(std::move(montagePlan));
            }
        }
        if (!loadPlan.animationSourceModelPath.empty() &&
            (!loadPlan.animationClips.empty() || !loadPlan.animationMontages.empty()))
            throw std::runtime_error(
                "Retargeted target mesh must not declare target-owned clips or montages; "
                "author them on skinning.sourceModel: " + std::string(meshAssetPath));
    }
    if (loadPlan.importOptions.generateSmoothNormals)
    {
        loadPlan.modelCacheKey += "|generateSmoothNormals";
    }
    for (const std::string& sectionName : loadPlan.importOptions.includedSectionNames)
    {
        loadPlan.modelCacheKey += "|section=" + sectionName;
    }
    return loadPlan;
}

std::vector<MeshSectionLoadPlan> MeshAssetValidator::BuildSectionLoadPlans(
    const MeshAssetBuildPlan& buildPlan,
    const ModelResource& modelResource)
{
    if (modelResource.sections.empty())
    {
        throw std::runtime_error(
            "Model resource has no sections: " +
            buildPlan.meshAssetPath + " model=" + buildPlan.modelDataPath);
    }

    std::vector<MeshSectionLoadPlan> sectionPlans;
    sectionPlans.reserve(modelResource.sections.size());
    const std::vector<std::string> sourceSlots = BuildUniqueSourceMaterialSlots(modelResource);
    if (buildPlan.assetType == "speedtree" && buildPlan.materialSlots.size() != sourceSlots.size())
    {
        throw std::runtime_error(BuildSlotCountMismatchReason(buildPlan, sourceSlots.size(), true));
    }
    if (buildPlan.assetType != "speedtree" && buildPlan.materialSlots.size() != sourceSlots.size())
    {
        throw std::runtime_error(BuildSlotCountMismatchReason(buildPlan, sourceSlots.size(), false));
    }

    for (const MeshSection& section : modelResource.sections)
    {
        if (section.materialSlotName.empty())
        {
            throw std::runtime_error(
                "Model section is missing material slot name: " +
                buildPlan.meshAssetPath + " model=" + buildPlan.modelDataPath +
                " section=" + section.sectionName);
        }

        const size_t sourceSlotIndex = FindSourceSlotIndex(sourceSlots, section.materialSlotName);
        if (sourceSlotIndex >= buildPlan.materialSlots.size())
        {
            throw std::runtime_error(
                "Mesh section material slot cannot be mapped by source slot index: " +
                buildPlan.meshAssetPath + " model=" + buildPlan.modelDataPath +
                " section=" + section.sectionName +
                " slot=" + section.materialSlotName);
        }
        const MeshMaterialSlot& materialSlot = buildPlan.materialSlots[sourceSlotIndex];

        MeshSectionLoadPlan sectionPlan;
        sectionPlan.sectionName = section.sectionName;
        sectionPlan.materialSlotIndex = static_cast<uint32_t>(sourceSlotIndex);
        sectionPlan.materialSlotName = materialSlot.name;
        sectionPlan.materialInstancePath = materialSlot.materialInstancePath;
        sectionPlans.push_back(sectionPlan);
    }
    return sectionPlans;
}

namespace VL::Hair
{
nlohmann::json WriteTressfxSimulationValues(const TressfxSimulationConfig& config)
{
    nlohmann::json value = {
        {"enabled", config.enabled},
        {"backend", config.backend == TressfxSimulationBackend::Gpu ? "gpu" : "cpu"},
        {"gravity", {config.gravity.x(), config.gravity.y(), config.gravity.z()}},
        {"damping", config.damping}, {"iterations", config.iterations},
        {"rootFixedPoints", config.rootFixedPoints}, {"maxDt", config.maxDt},
        {"bendStiffness", config.bendStiffness},
        {"globalShapeStiffness", config.globalShapeStiffness},
        {"globalShapeEffectiveRange", config.globalShapeEffectiveRange},
        {"vspCoefficient", config.vspCoefficient},
        {"vspAccelThresholdMax", config.vspAccelThresholdMax},
        {"windGain", config.windGain}};
    if (!config.vspStiffness.empty())
        value["vspStiffness"] = config.vspStiffness;
    if (!config.localShapeStiffness.empty())
        value["localShapeStiffness"] = config.localShapeStiffness;
    return value;
}

nlohmann::json WriteTressfxSkinning(const TressfxSimulationSkinning& skinning)
{
    nlohmann::json value = {{"rootBone", skinning.rootBone}};
    if (!skinning.source.empty())
        value["source"] = skinning.source;
    if (!skinning.boneWeightsPath.empty())
        value["boneWeights"] = skinning.boneWeightsPath;
    if (skinning.rootWeightsAuthored)
        value["weights"] = skinning.rootWeights == TressfxRootWeightSource::Mesh
            ? "mesh" : "rootBone";
    return value;
}

TressfxSimulationConfig ReadTressfxSimulationDefaults(
    const nlohmann::json& asset, const std::string& context)
{
    if (asset.contains("simulation") || asset.contains("collision") ||
        asset.contains("hairCollision") || asset.contains("skinning") ||
        asset.contains("collisionDefaults"))
        throw std::runtime_error("TressFX asset must use simulationDefaults; "
            "skinning and hairCollision belong to the placement: " + context);
    if (!asset.contains("simulationDefaults"))
        return {};
    const auto& defaults = asset.at("simulationDefaults");
    if (!defaults.is_object() || defaults.contains("skinning") ||
        defaults.contains("collision") || defaults.contains("hairCollision"))
        throw std::runtime_error("TressFX simulationDefaults must contain only "
            "simulation values: " + context);
    return MeshAssetValidator::BuildTressfxSimulationConfig(
        {{"simulation", defaults}}, context);
}

TressfxPlacementBehavior ReadTressfxPlacementBehavior(
    const nlohmann::json& node, const std::string& context)
{
    if (!node.is_object())
        throw std::runtime_error("TressFX placement behavior must be an object: " + context);
    for (auto field = node.begin(); field != node.end(); ++field)
        if (field.key() != "simulation" && field.key() != "skinning" &&
            field.key() != "hairCollision")
            throw std::runtime_error("Unknown TressFX placement field: " +
                field.key() + " " + context);
    TressfxPlacementBehavior behavior;
    nlohmann::json internal = {{"simulation", {{"enabled", false}, {"backend", "gpu"}}}};
    if (node.contains("simulation"))
    {
        const auto& simulation = node.at("simulation");
        if (!simulation.is_object())
            throw std::runtime_error("TressFX placement simulation must be an object: " + context);
        for (auto field = simulation.begin(); field != simulation.end(); ++field)
            if (field.key() != "mode" && field.key() != "overrides")
                throw std::runtime_error("Unknown TressFX placement simulation field: " +
                    field.key() + " " + context);
        const auto mode = simulation.value("mode", std::string("inherit"));
        if (mode == "off") behavior.mode = TressfxSimulationMode::Off;
        else if (mode == "on") behavior.mode = TressfxSimulationMode::On;
        else if (mode != "inherit")
            throw std::runtime_error("TressFX simulation mode must be inherit, off or on: " + context);
        if (simulation.contains("overrides"))
        {
            behavior.overrides = simulation.at("overrides");
            if (!behavior.overrides.is_object() ||
                behavior.overrides.contains("enabled") ||
                behavior.overrides.contains("skinning") ||
                behavior.overrides.contains("collision"))
                throw std::runtime_error("TressFX overrides contain forbidden fields: " + context);
            internal["simulation"].update(behavior.overrides);
        }
    }
    if (node.contains("skinning"))
        internal["simulation"]["skinning"] = node.at("skinning");
    if (node.contains("hairCollision"))
    {
        behavior.hairCollision = node.at("hairCollision");
        internal["collision"] = behavior.hairCollision;
    }
    behavior.skinning = MeshAssetValidator::BuildTressfxSimulationConfig(
        internal, context).skinning;
    return behavior;
}

TressfxSimulationConfig ResolveTressfxBehavior(
    const TressfxSimulationConfig& defaults,
    const TressfxPlacementBehavior& placement,
    const std::string& context)
{
    return ResolveTressfxBehaviorLayers(defaults, nlohmann::json::object(), placement, context);
}

TressfxSimulationConfig ResolveTressfxBehaviorLayers(
    const TressfxSimulationConfig& defaults, const nlohmann::json& overrides,
    const TressfxPlacementBehavior& placement,
    const std::string& context)
{
    if (!overrides.is_object())
        throw std::runtime_error("TressFX simulation overrides must be an object" +
            (context.empty() ? std::string() : ": " + context));
    nlohmann::json simulation = WriteTressfxSimulationValues(defaults);
    simulation.update(overrides);
    simulation.update(placement.overrides);
    if (placement.mode != TressfxSimulationMode::Inherit)
        simulation["enabled"] = placement.mode == TressfxSimulationMode::On;
    if (placement.skinning)
        simulation["skinning"] = WriteTressfxSkinning(*placement.skinning);
    nlohmann::json internal = {{"simulation", simulation}};
    if (!placement.hairCollision.empty())
        internal["collision"] = placement.hairCollision;
    return MeshAssetValidator::BuildTressfxSimulationConfig(internal, context);
}

std::string MakeTressfxBehaviorKey(const TressfxSimulationConfig& config)
{
    nlohmann::json identity = WriteTressfxSimulationValues(config);
    if (config.skinning)
        identity["skinning"] = WriteTressfxSkinning(*config.skinning);
    identity["capsules"] = nlohmann::json::array();
    for (const auto& capsule : config.capsules)
        identity["capsules"].push_back({{"a", {capsule.a.x(), capsule.a.y(), capsule.a.z()}},
            {"b", {capsule.b.x(), capsule.b.y(), capsule.b.z()}},
            {"radius", capsule.radius}, {"bone", capsule.bone}});
    if (config.collisionMesh)
        identity["collisionMesh"] = {{"path", config.collisionMesh->path},
            {"cellSize", config.collisionMesh->cellSize}, {"margin", config.collisionMesh->margin}};
    return identity.dump();
}

} // namespace VL::Hair
