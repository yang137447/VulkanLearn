#include "world/motionBricksRuntime.h"

#include "animation/animationRetarget.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace VL::Animation::MotionBricks
{
namespace
{
std::filesystem::path Resolve(const std::filesystem::path& root, const std::string& path)
{
    if (path.empty())
        throw std::invalid_argument("MotionBricks path cannot be empty");
    const std::filesystem::path relative = std::filesystem::u8path(path);
    return relative.is_absolute() ? relative : root / relative;
}

std::string RequireString(const nlohmann::json& object, const char* key,
    const std::string& context)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() ||
        found->get<std::string>().empty())
        throw std::invalid_argument(context + " requires nonempty " + key);
    return found->get<std::string>();
}

std::string OptionalString(const nlohmann::json& object, const char* key,
    const std::string& fallback, const std::string& context)
{
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    if (!found->is_string())
        throw std::invalid_argument(context + "." + key + " must be a string");
    return found->get<std::string>();
}

OnnxExecutionProvider ParseProvider(const std::string& value)
{
    if (value.empty() || value == "auto") return OnnxExecutionProvider::Auto;
    if (value == "cpu") return OnnxExecutionProvider::Cpu;
    if (value == "directml") return OnnxExecutionProvider::DirectML;
    if (value == "cuda") return OnnxExecutionProvider::Cuda;
    throw std::invalid_argument("motionBricks.provider must be auto/cpu/directml/cuda");
}

struct PreparedMotionBricks
{
    std::shared_ptr<const SomaSkeletonAsset> source;
    RetargetProfile profile;
};

PreparedMotionBricks Prepare(const MotionBricksMeshConfig& config,
    const std::filesystem::path& resourceRoot,
    std::map<std::string, std::shared_ptr<const SomaSkeletonAsset>>& skeletonCache)
{
    const std::filesystem::path skeletonPath = Resolve(resourceRoot, config.skeleton);
    const std::filesystem::path manifestPath = Resolve(resourceRoot, config.manifest);
    const std::filesystem::path retargetPath = Resolve(resourceRoot, config.retarget);

    const std::string cacheKey = skeletonPath.string() + "|" + manifestPath.string();
    std::shared_ptr<const SomaSkeletonAsset> source = skeletonCache[cacheKey];
    if (!source)
    {
        source = LoadSomaSkeleton(skeletonPath, manifestPath);
        skeletonCache[cacheKey] = source;
    }

    std::ifstream retargetInput(retargetPath, std::ios::binary);
    if (!retargetInput)
        throw std::invalid_argument("Could not open MotionBricks retarget profile: " +
            retargetPath.string());
    nlohmann::json retargetJson;
    retargetInput >> retargetJson;
    PreparedMotionBricks prepared;
    prepared.source = std::move(source);
    prepared.profile = ParseRetargetProfile(retargetJson, retargetPath.string());
    return prepared;
}

std::filesystem::path ResolveLiveModel(const MotionBricksMeshConfig& config,
    const std::filesystem::path& resourceRoot)
{
    if (!config.model.empty())
        return Resolve(resourceRoot, config.model);
    const char* environment = std::getenv("VULKANLEARN_MOTIONBRICKS_SOMA_ONNX");
    if (environment && *environment)
        return std::filesystem::u8path(environment);
    throw std::invalid_argument(
        "motionBricks live mode requires model or VULKANLEARN_MOTIONBRICKS_SOMA_ONNX");
}
}

bool TryReadMotionBricksConfig(const nlohmann::json& component,
    MotionBricksMeshConfig& config, std::string& error)
{
    const auto found = component.find("motionBricks");
    if (found == component.end())
        return false;
    if (!found->is_object())
    {
        error = "motionBricks must be an object";
        return false;
    }
    try
    {
        config.mode = OptionalString(*found, "mode", "baked", "motionBricks");
        if (config.mode != "baked" && config.mode != "live")
            throw std::invalid_argument("motionBricks.mode must be baked or live");
        config.skeleton = RequireString(*found, "skeleton", "motionBricks");
        config.manifest = RequireString(*found, "manifest", "motionBricks");
        config.retarget = RequireString(*found, "retarget", "motionBricks");
        config.model = OptionalString(*found, "model", "", "motionBricks");
        config.provider = OptionalString(*found, "provider", "auto", "motionBricks");
        config.locomotionTargets = OptionalString(*found, "locomotionTargets", "", "motionBricks");
        if (config.mode == "baked")
            config.baked = RequireString(*found, "baked", "motionBricks");
        else
            config.baked = OptionalString(*found, "baked", "", "motionBricks");
        if (found->contains("transitionEarlyByFrames"))
        {
            if (!found->at("transitionEarlyByFrames").is_number_integer())
                throw std::invalid_argument(
                    "motionBricks.transitionEarlyByFrames must be an integer");
            config.transitionEarlyByFrames =
                found->at("transitionEarlyByFrames").get<int>();
        }
        if (config.transitionEarlyByFrames < 0)
            throw std::invalid_argument(
                "motionBricks.transitionEarlyByFrames cannot be negative");
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

std::shared_ptr<IMotionBricksRuntime> CreateMotionBricksRuntime(
    const MotionBricksMeshConfig& config,
    const std::filesystem::path& resourceRoot,
    const SkeletonMesh& target,
    std::map<std::string, std::shared_ptr<const SomaSkeletonAsset>>& skeletonCache)
{
    PreparedMotionBricks prepared = Prepare(config, resourceRoot, skeletonCache);
    if (config.mode == "live")
    {
        const std::filesystem::path modelPath = ResolveLiveModel(config, resourceRoot);
        OnnxSessionOptions options;
        options.provider = ParseProvider(config.provider);
        const ModelContract contract = prepared.source->contract;
        if (config.locomotionTargets.empty())
            throw std::invalid_argument("motionBricks live mode requires locomotionTargets");
        auto targets = LoadLocomotionTargets(Resolve(resourceRoot, config.locomotionTargets),
            prepared.source->binding);
        return std::make_shared<LiveMotionRuntime>(std::move(prepared.source), target,
            prepared.profile, modelPath, contract, options, config.transitionEarlyByFrames,
            0.0f, std::move(targets));
    }

    if (config.baked.empty())
        throw std::invalid_argument("motionBricks baked mode requires baked");
    BakedMotion baked = LoadBakedMotion(Resolve(resourceRoot, config.baked));
    baked.transitionEarlyByFrames = config.transitionEarlyByFrames;
    return std::make_shared<BakedMotionRuntime>(std::move(prepared.source), target,
        prepared.profile, std::move(baked));
}

std::shared_ptr<BakedMotionRuntime> CreateBakedRuntime(
    const MotionBricksMeshConfig& config,
    const std::filesystem::path& resourceRoot,
    const SkeletonMesh& target,
    std::map<std::string, std::shared_ptr<const SomaSkeletonAsset>>& skeletonCache)
{
    if (config.mode != "baked" || config.baked.empty())
        throw std::invalid_argument("CreateBakedRuntime requires baked mode and a baked path");
    PreparedMotionBricks prepared = Prepare(config, resourceRoot, skeletonCache);
    BakedMotion baked = LoadBakedMotion(Resolve(resourceRoot, config.baked));
    baked.transitionEarlyByFrames = config.transitionEarlyByFrames;
    return std::make_shared<BakedMotionRuntime>(std::move(prepared.source), target,
        prepared.profile, std::move(baked));
}
}
