#include "render/offline/offlineRenderCoordinator.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include "render/offline/offlineCameraMotionPolicy.h"
#include "shader/build/contentHash.h"

namespace VL
{
namespace
{
nlohmann::json Vector(const Eigen::Vector3f& value)
{
    return {value.x(), value.y(), value.z()};
}

nlohmann::json Matrix(const Eigen::Matrix4f& value)
{
    nlohmann::json result = nlohmann::json::array();
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            result.push_back(value(row, col));
    return result;
}

std::string MeshDigest(const MeshSection& section)
{
    CanonicalFieldHasher hash("VL.Offline.MeshSection.v1");
    for (const Vertex& vertex : section.vertices)
    {
        // 编码字段值，不哈希 C++ 结构 padding 或指针，持久身份可跨进程。
        hash.AddString("position", Vector(vertex.position).dump());
        hash.AddString("normal", Vector(vertex.normal).dump());
        hash.AddString("uv", nlohmann::json{
            vertex.texCoord.x(), vertex.texCoord.y()}.dump());
    }
    for (uint32_t index : section.indices)
        hash.AddUInt32("index", index);
    return hash.Finalize().ToHex();
}

bool EqualVector(const Eigen::Vector3f& left, const Eigen::Vector3f& right)
{
    return left.x() == right.x() && left.y() == right.y() && left.z() == right.z();
}

bool EqualExposure(const CameraExposure& left, const CameraExposure& right)
{
    return left.mode == right.mode &&
        left.ev100 == right.ev100 &&
        left.aperture == right.aperture &&
        left.shutterSeconds == right.shutterSeconds &&
        left.iso == right.iso &&
        left.compensation == right.compensation &&
        left.autoMinEv100 == right.autoMinEv100 &&
        left.autoMaxEv100 == right.autoMaxEv100 &&
        left.autoSpeedUp == right.autoSpeedUp &&
        left.autoSpeedDown == right.autoSpeedDown &&
        left.autoTargetGray == right.autoTargetGray;
}

bool EqualMatrix(const Eigen::Matrix4f& left, const Eigen::Matrix4f& right)
{
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            if (left(row, col) != right(row, col)) return false;
    return true;
}

bool EqualInputSnapshot(const WorldSnapshot& left, const WorldSnapshot& right)
{
    if (left.worldGeneration != right.worldGeneration ||
        left.environment.type != right.environment.type ||
        !EqualExposure(left.camera.exposure, right.camera.exposure) ||
        !EqualVector(left.camera.position, right.camera.position) ||
        !EqualVector(left.camera.right, right.camera.right) ||
        !EqualVector(left.camera.up, right.camera.up) ||
        !EqualVector(left.camera.forward, right.camera.forward) ||
        left.camera.horizontalFovDegrees != right.camera.horizontalFovDegrees ||
        left.camera.clipNear != right.camera.clipNear ||
        left.camera.clipFar != right.camera.clipFar ||
        left.meshDraws.size() != right.meshDraws.size() ||
        left.lights.size() != right.lights.size())
        return false;
    for (size_t i = 0; i < left.meshDraws.size(); ++i)
    {
        const auto& a = left.meshDraws[i];
        const auto& b = right.meshDraws[i];
        if (a.debugName != b.debugName || a.mesh.key != b.mesh.key ||
            a.materialInstance.key != b.materialInstance.key ||
            !EqualMatrix(a.model, b.model))
            return false;
    }
    for (size_t i = 0; i < left.lights.size(); ++i)
    {
        const auto& a = left.lights[i];
        const auto& b = right.lights[i];
        if (a.type != b.type || !EqualVector(a.position, b.position) ||
            !EqualVector(a.direction, b.direction) ||
            !EqualVector(a.color, b.color) || a.intensity != b.intensity ||
            a.squareSize != b.squareSize)
            return false;
    }
    return true;
}

bool EqualIntegrator(const OfflineRenderSettings& left, const OfflineRenderSettings& right)
{
    return left.width == right.width && left.height == right.height &&
        left.seed == right.seed && left.bounces == right.bounces &&
        left.lightSamples == right.lightSamples &&
        left.interactivePreview == right.interactivePreview;
}

void WritePfm(const std::filesystem::path& path,
              const std::vector<Offline::Vec3>& pixels, int width, int height)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open offline output: " + path.string());
    file << "PF\n" << width << ' ' << height << "\n-1.0\n";
    for (int y = height - 1; y >= 0; --y)
        for (int x = 0; x < width; ++x)
        {
            const auto& color = pixels[static_cast<size_t>(y) * width + x];
            const float rgb[]{static_cast<float>(color.x),
                              static_cast<float>(color.y), static_cast<float>(color.z)};
            file.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
        }
    file.flush();
    if (!file) throw std::runtime_error("Offline PFM write failed: " + path.string());
}

unsigned char DisplayChannel(double value, float exposure)
{
    const double exposed = value * exposure;
    const double mapped = exposed / (1.0 + exposed);
    const double encoded = mapped <= 0.0031308 ? 12.92 * mapped
        : 1.055 * std::pow(mapped, 1.0 / 2.4) - 0.055;
    return static_cast<unsigned char>(std::clamp(encoded * 255, 0.0, 255.0));
}
} // namespace

nlohmann::json OfflineRenderCoordinator::DescribeInput(
    const WorldSnapshot& snapshot, const OfflineWorldAssets& assets,
    const OfflineRenderSettings& settings)
{
    nlohmann::json result{
        {"type", "offlineRenderResult"}, {"version", 1},
        {"worldGeneration", snapshot.worldGeneration}, {"scene", assets.scenePath},
        {"camera", {{"position", Vector(snapshot.camera.position)},
                    {"right", Vector(snapshot.camera.right)},
                    {"up", Vector(snapshot.camera.up)},
                    {"forward", Vector(snapshot.camera.forward)},
                    {"horizontalFov", snapshot.camera.horizontalFovDegrees},
                    {"clipNear", snapshot.camera.clipNear},
                    {"clipFar", snapshot.camera.clipFar},
                    {"exposure", DescribeCameraExposure(snapshot.camera.exposure)},
                    {"exposureScale", ExposureScale(snapshot.camera.exposure)}}},
        {"integrator", {{"width", settings.width}, {"height", settings.height},
                        {"seed", settings.seed}, {"bounces", settings.bounces},
                        {"lightSamples", settings.lightSamples}}},
        {"materials", assets.effectiveMaterialInstances},
        {"profiles", assets.modelProfiles},
        {"profileDigests", assets.modelProfileDigests},
        {"replay", "External source assets at the captured versions are required"},
        {"sampling", "LOD0; CPU model direction kernels; Reinhard viewport only"},
        {"modelImplementation", "VL.Offline.ClothSheen.ClothFiber.Lambert.AshikhminVelvet.AshikhminSatin.AshikhminSlantedVelvet.v3"},
        {"environment", static_cast<int>(snapshot.environment.type)}};
    result["groomAssets"] = nlohmann::json::object();
    for (const auto& [reference, groom] : assets.groomAssets)
        result["groomAssets"][reference] = {
            {"descriptorDigest", groom.descriptorDigest},
            {"sourceDigest", groom.sourceDigest},
            {"source", groom.sourcePath.string()},
            {"sampling", "signed RG, repeat, linear, LOD0"}};
    result["colorAssets"] = nlohmann::json::object();
    for (const auto& [reference, color] : assets.colorAssets)
        result["colorAssets"][reference] = {
            {"descriptorDigest", color.descriptorDigest},
            {"sourceDigest", color.sourceDigest},
            {"source", color.sourcePath.string()},
            {"colorSpace", color.srgb ? "sRGB" : "linear"},
            {"wrapMode", color.repeat ? "repeat" : "clamp"},
            {"filter", color.linear ? "linear" : "nearest"},
            {"sampling", "RGB, CPU rows flipped once; mesh UV unchanged; LOD0; alpha unused"}};
    result["draws"] = nlohmann::json::array();
    for (const auto& draw : snapshot.meshDraws)
        result["draws"].push_back({
            {"object", draw.debugName}, {"mesh", draw.mesh.key},
            {"materialInstance", draw.materialInstance.key}, {"model", Matrix(draw.model)}});
    result["lights"] = nlohmann::json::array();
    for (const auto& light : snapshot.lights)
        result["lights"].push_back({
            {"type", static_cast<int>(light.type)}, {"position", Vector(light.position)},
            {"direction", Vector(light.direction)}, {"color", Vector(light.color)},
            {"intensity", light.intensity}, {"squareSize", light.squareSize}});
    return result;
}

OfflineRenderCoordinator::OfflineRenderCoordinator()
    : prepareWorker(&OfflineRenderCoordinator::Run, this)
{
}

OfflineRenderCoordinator::~OfflineRenderCoordinator()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        exiting = true;
        ++generation;
        latestGeneration.store(generation, std::memory_order_relaxed);
    }
    session.Stop();
    wake.notify_all();
    prepareWorker.join();
    // session 的析构再 join 积分 worker，冻结资产不会提前释放。
}

void OfflineRenderCoordinator::Observe(WorldSnapshotPtr snapshot,
    std::shared_ptr<const OfflineWorldAssets> assets, OfflineRenderSettings settings)
{
    if (!snapshot || !assets) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        // Stop 是用户意图，帧驱动的 Observe 不应在下一帧自动把任务启动。
        if (stopped) return;
        // 冻结包指针只作为本进程的快速失效信号，不进入持久身份；
        // 每帧快照直接比对离线实际消费的字段，忽略 frameIndex/上一帧矩阵。
        if (observedSnapshot && observedAssets == assets &&
            EqualIntegrator(observedSettings, settings) &&
            EqualInputSnapshot(*observedSnapshot, *snapshot))
        {
            if (settings.samples == targetSamples) return;
            if (settings.samples > targetSamples && !settings.interactivePreview)
            {
                // prepare 已取走 pending 时也要记录目标，提交时以最新值启动。
                if (pending)
                {
                    auto updated = std::make_shared<Request>(*pending);
                    updated->settings.samples = settings.samples;
                    pending = std::move(updated);
                }
                if (active)
                {
                    session.ExtendTargetSamples(settings.samples);
                    if (paused) session.Pause();
                    auto extended = std::make_shared<Request>(*active);
                    extended->settings.samples = settings.samples;
                    active = std::move(extended);
                }
                targetSamples = settings.samples;
                return;
            }
            // 降低 spp 不回退已经发布的累积，换代重算以保持目标精确。
        }
        observedSnapshot = snapshot;
        observedAssets = assets;
        observedSettings = settings;
        targetSamples = settings.samples;
        ++generation;
        latestGeneration.store(generation, std::memory_order_relaxed);
        session.Stop();
        sessionGeneration = 0;
        error.clear();
        state = Offline::SessionState::Preparing;
        auto request = std::make_shared<Request>();
        request->generation = generation;
        request->snapshot = std::move(snapshot);
        request->assets = std::move(assets);
        request->settings = settings;
        request->acceptedAt = std::chrono::steady_clock::now();
        pending = std::move(request);
        active.reset();
    }
    wake.notify_all();
}

void OfflineRenderCoordinator::Run()
{
    for (;;)
    {
        std::shared_ptr<const Request> request;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [&] { return exiting || pending != nullptr; });
            if (exiting) return;
            request = std::move(pending);
        }
        try
        {
            if (preparedMeshWorldGeneration != request->snapshot->worldGeneration)
            {
                preparedMeshes.clear();
                preparedMeshSections.clear();
                preparedMeshDigestAssets.reset();
                preparedMeshDigests = nullptr;
                preparedMeshWorldGeneration = request->snapshot->worldGeneration;
            }
            // World generation 相同也可能换入新的冻结网格；只按导入版本复用 BVH，
            // 材质或贴图版本变化则无需重建几何。
            for (auto it = preparedMeshes.begin(); it != preparedMeshes.end();)
            {
                const auto section = request->assets->sections.find(it->first);
                const auto cached = preparedMeshSections.find(it->first);
                if (section == request->assets->sections.end() ||
                    cached == preparedMeshSections.end() ||
                    section->second.imported != cached->second.imported ||
                    section->second.sectionIndex != cached->second.sectionIndex)
                {
                    preparedMeshSections.erase(it->first);
                    it = preparedMeshes.erase(it);
                }
                else ++it;
            }
            // 候选包不可变；在 worker 的局部副本中附加已建 BVH，避免相机/材质
            // 改动重新导入或重新构建相同几何，且不触碰活动 World。
            auto assets = *request->assets;
            assets.coreMeshes = preparedMeshes;
            auto prepared = BuildOfflineRenderInput(
                *request->snapshot, assets, request->settings,
                &latestGeneration, request->generation);
            if (prepared.IsFailure())
                throw std::runtime_error(FormatRuntimeError(prepared.Error()));
            if (latestGeneration.load(std::memory_order_relaxed) != request->generation)
                continue;
            nlohmann::json manifest;
            if (!request->settings.interactivePreview)
            {
                // 部分预览不进入正式导出：完整清单和 mesh 摘要只为可导出候选构建。
                manifest = DescribeInput(*request->snapshot, *request->assets,
                    request->settings);
                // 冻结包不可变，指针只用作本进程的缓存版本；持久身份仍取网格字节。
                if (preparedMeshDigestAssets != request->assets)
                {
                    preparedMeshDigests = nlohmann::json::object();
                    for (const auto& [key, section] : request->assets->sections)
                        preparedMeshDigests[key] = MeshDigest(
                            section.imported->modelResource.sections.at(section.sectionIndex));
                    preparedMeshDigestAssets = request->assets;
                }
                manifest["meshDigests"] = preparedMeshDigests;
                // 输出文件、完成样本数和 task generation 均在发布后附加，
                // 不属于相同输入在不同任务/导出中的稳定 BLAKE3 身份。
                manifest["inputDigest"] = ContentHasher::HashString(manifest.dump()).ToHex();
            }
            auto published = std::make_shared<Request>(*request);
            published->manifest = std::move(manifest);
            std::lock_guard<std::mutex> lock(mutex);
            if (exiting || request->generation != generation) continue;
            prepared.Value().targetSamples = targetSamples;
            // 截止时间始于 GT 接受请求，prepare 已花掉的预算不能重新领取。
            if (request->settings.interactivePreview)
                prepared.Value().previewDeadline =
                    request->acceptedAt + OfflineCameraMotionPolicy::PreviewBudget;
            published->settings.samples = targetSamples;
            for (size_t index = 0; index < request->snapshot->meshDraws.size(); ++index)
                for (const auto& mesh : prepared.Value().instanceMeshes)
                    if (mesh.get() == prepared.Value().scene.instances[index].mesh)
                    {
                        preparedMeshes.emplace(
                            request->snapshot->meshDraws[index].mesh.key, mesh);
                        preparedMeshSections.insert_or_assign(
                            request->snapshot->meshDraws[index].mesh.key,
                            request->assets->sections.at(
                                request->snapshot->meshDraws[index].mesh.key));
                    }
            sessionGeneration = session.Start(std::move(prepared.Value()));
            if (paused) session.Pause();
            active = std::move(published);
            state = Offline::SessionState::Rendering;
        }
        catch (const std::exception& exception)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (request->generation != generation || exiting) continue;
            error = exception.what();
            state = Offline::SessionState::Failed;
        }
    }
}

void OfflineRenderCoordinator::Pause()
{
    std::lock_guard<std::mutex> lock(mutex);
    paused = true;
    session.Pause();
}

void OfflineRenderCoordinator::Resume()
{
    std::lock_guard<std::mutex> lock(mutex);
    paused = false;
    session.Resume();
}

void OfflineRenderCoordinator::Stop()
{
    std::lock_guard<std::mutex> lock(mutex);
    stopped = true;
    ++generation;
    latestGeneration.store(generation, std::memory_order_relaxed);
    pending.reset();
    session.Stop();
    state = Offline::SessionState::Canceled;
}

void OfflineRenderCoordinator::Restart()
{
    std::lock_guard<std::mutex> lock(mutex);
    stopped = false;
    observedSnapshot.reset();
    observedAssets.reset();
    paused = false;
}

Offline::SessionState OfflineRenderCoordinator::State() const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (state == Offline::SessionState::Canceled || state == Offline::SessionState::Failed)
        return state;
    if (paused) return Offline::SessionState::Paused;
    return active ? session.State() : state;
}

std::string OfflineRenderCoordinator::Error() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return error.empty() ? session.Error() : error;
}

std::shared_ptr<const OfflinePublishedResult> OfflineRenderCoordinator::Capture() const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (active && active->settings.interactivePreview) return {};
    const auto film = session.Latest();
    if (!active || !film || film->generation != sessionGeneration) return {};
    auto result = std::make_shared<OfflinePublishedResult>();
    result->taskGeneration = active->generation;
    result->film = film;
    result->manifest = active->manifest;
    result->manifest["taskGeneration"] = active->generation;
    result->manifest["samples"] = film->completedSamples;
    result->manifest["targetSamples"] = film->targetSamples;
    result->manifest["complete"] = film->completedSamples >= film->targetSamples;
    return result;
}

std::shared_ptr<const Offline::PreviewSnapshot> OfflineRenderCoordinator::CapturePreview() const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!active || !active->settings.interactivePreview) return {};
    const auto preview = session.LatestPreview();
    if (!preview || preview->generation != sessionGeneration) return {};
    return preview;
}

void ExportOfflineResult(const OfflinePublishedResult& result,
    const std::filesystem::path& outputDirectory, float displayExposure)
{
    if (!result.film || !std::isfinite(displayExposure) || displayExposure <= 0)
        throw std::invalid_argument("No offline result or invalid display exposure");
    // 目标是一次独立导出的目录；已有清单/文件禁止覆盖，避免混合两代数据。
    if (std::filesystem::exists(outputDirectory))
        throw std::runtime_error("Offline output already exists: " + outputDirectory.string());
    const auto temporary = std::filesystem::path(outputDirectory.string() + ".partial");
    if (!std::filesystem::create_directories(temporary))
        throw std::runtime_error("Offline partial output already exists: " + temporary.string());
    const auto& film = *result.film;
    std::vector<Offline::Vec3> combined(film.direct.size());
    for (size_t i = 0; i < combined.size(); ++i)
        combined[i] = film.direct[i] + film.indirect[i];
    WritePfm(temporary / "combined.pfm", combined, film.width, film.height);
    WritePfm(temporary / "direct.pfm", film.direct, film.width, film.height);
    WritePfm(temporary / "indirect.pfm", film.indirect, film.width, film.height);
    std::ofstream preview(temporary / "combined.ppm", std::ios::binary);
    preview << "P6\n" << film.width << ' ' << film.height << "\n255\n";
    for (const auto& color : combined)
    {
        const unsigned char rgb[]{DisplayChannel(color.x, displayExposure),
            DisplayChannel(color.y, displayExposure), DisplayChannel(color.z, displayExposure)};
        preview.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
    }
    preview.flush();
    if (!preview) throw std::runtime_error("Offline preview write failed");
    preview.close();
    nlohmann::json manifest = result.manifest;
    manifest["display"] = {{"exposure", displayExposure}, {"mapping", "Reinhard + sRGB"}};
    for (const char* file : {"combined.pfm", "direct.pfm", "indirect.pfm", "combined.ppm"})
        manifest["outputs"][file] = ContentHasher::HashFile(temporary / file).ToHex();
    std::ofstream file(temporary / "manifest.json");
    file << manifest.dump(2) << '\n';
    file.flush();
    if (!file) throw std::runtime_error("Offline manifest write failed");
    file.close();
    // 所有输出关闭后才发布目录；.partial 永远不作为成功结果。
    std::filesystem::rename(temporary, outputDirectory);
}
} // namespace VL
