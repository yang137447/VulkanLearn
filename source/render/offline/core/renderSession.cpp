#include "core/renderSession.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace VL::Offline
{
namespace
{
Random PixelRandom(int x, int y, int width, int sample, std::uint32_t seed)
{
    return {((static_cast<std::uint32_t>(y * width + x) * 747796405u +
              static_cast<std::uint32_t>(sample) * 2891336453u) ^
             (seed * 0x9e3779b9u)) | 1u};
}

Ray CameraRay(const RenderInput& input, int x, int y, Random& random)
{
    const double sx = (x + random.next()) / input.width - 0.5;
    const double sy = 0.5 - (y + random.next()) / input.height;
    const double halfWidth = std::tan(input.horizontalFovDegrees * kPi / 360.0);
    // 水平 FOV 与 VL CameraSnapshot 一致，垂直视场按 height/width 推导。
    return {input.scene.cameraPosition,
            normalize(input.cameraForward +
                      input.cameraRight * (2.0 * sx * halfWidth) +
                      input.cameraUp * (2.0 * sy * halfWidth * input.height / input.width))};
}

PathResult TraceCameraPath(const RenderInput& input, const AlbedoTable& albedo,
                           Ray ray, Random& random)
{
    // 透视裁剪面垂直于相机 forward；斜射线的交点距离要除以方向投影，
    // 不能把近远深度直接当作所有像素共用的 ray length。
    const double forwardCosine = dot(ray.direction, input.cameraForward);
    const double nearDistance = input.cameraClipNear / forwardCosine;
    const double farDistance = input.cameraClipFar / forwardCosine;
    double startDistance = std::max(0.0, nearDistance - 2.0 * kEpsilon);
    ray.origin += ray.direction * startDistance;

    // trace 不接收首段 t 范围：先检查近远面内的首个表面/发光体；
    // 只有通过检查的主射线才进入原积分器，后续续射保持无限范围。
    // 起点略早于近面，保留恰在近面上的三角形（相交器要求 t > epsilon）。
    Hit hit;
    bool surface = intersect(input.scene, ray,
        farDistance - startDistance + 2.0 * kEpsilon, hit);
    if (surface && hit.distance + startDistance < nearDistance)
    {
        // 近面前极窄区间的表面不能遮住其后的合法几何。
        startDistance = nearDistance + kEpsilon;
        ray.origin = input.scene.cameraPosition + ray.direction * startDistance;
        surface = intersect(input.scene, ray,
            farDistance - startDistance + 2.0 * kEpsilon, hit);
    }
    surface = surface && hit.distance + startDistance >= nearDistance &&
        hit.distance + startDistance <= farDistance;
    double emitterDistance = 0.0;
    const bool emitter = input.scene.hasAreaEmitter &&
        intersectAreaLight(input.scene, ray, emitterDistance) &&
        emitterDistance + startDistance >= nearDistance &&
        emitterDistance + startDistance <= farDistance;
    if (!surface && !emitter)
        return {input.scene.backdrop, {}};
    return trace(input.scene, albedo, ray, input.bounces, input.lightSamples, random);
}

template <typename T>
bool IsOwned(const T* pointer, const std::vector<std::shared_ptr<const T>>& owners)
{
    if (!pointer) return false;
    for (const auto& owner : owners)
        if (owner.get() == pointer) return true;
    return false;
}

const AlbedoTable& SharedAlbedoTable()
{
    // 方向反照率积分只依赖固定论文参数；函数静态对象由 C++ 保证并发首次初始化一次，
    // 构造完成后所有会话和行 worker 只读共享，避免每次相机任务重算整张 LUT。
    static const AlbedoTable table;
    return table;
}

} // namespace

struct RenderSession::SampleWork
{
    SampleWork(const RenderInput& input, const AlbedoTable& albedo,
               std::vector<Vec3>& direct, std::vector<Vec3>& indirect,
               std::uint64_t generation)
        : input(input), albedo(albedo), direct(direct), indirect(indirect),
          generation(generation)
    {
    }

    ~SampleWork()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        ready.notify_all();
        for (std::thread& thread : helpers)
            if (thread.joinable()) thread.join();
    }

    const RenderInput& input;
    const AlbedoTable& albedo;
    std::vector<Vec3>& direct;
    std::vector<Vec3>& indirect;
    const std::uint64_t generation;
    std::atomic<int> nextRow{0};
    std::atomic<bool> failed{false};
    std::mutex mutex;
    std::condition_variable ready, finished;
    std::vector<std::thread> helpers;
    std::uint64_t serial = 0;
    int sample = 0;
    unsigned remaining = 0;
    bool stopping = false;
    std::exception_ptr failure;
};

struct RenderSession::PreviewWork
{
    PreviewWork(const RenderInput& input, const AlbedoTable& albedo,
                PreviewSnapshot& snapshot, std::uint64_t generation)
        : input(input), albedo(albedo), snapshot(snapshot), generation(generation),
          tilesX(static_cast<std::size_t>(input.width / 8 + (input.width % 8 != 0))),
          tileCount(tilesX * static_cast<std::size_t>(
              input.height / 8 + (input.height % 8 != 0)))
    {
        // 与 tile 总数互素的步长打散连续领取的 tile，先覆盖视口各处。
        stride = tileCount / 2 + 1;
        while (std::gcd(stride, tileCount) != 1) ++stride;
    }

    ~PreviewWork()
    {
        for (std::thread& thread : helpers)
            if (thread.joinable()) thread.join();
    }

    const RenderInput& input;
    const AlbedoTable& albedo;
    PreviewSnapshot& snapshot;
    const std::uint64_t generation;
    const std::size_t tilesX, tileCount;
    std::size_t stride = 1;
    std::atomic<std::size_t> nextTile{0}, validCount{0};
    std::atomic<bool> failed{false};
    std::mutex failureMutex;
    std::exception_ptr failure;
    std::vector<std::thread> helpers;
};

RenderSession::RenderSession(unsigned requestedParallelism)
    : parallelism(requestedParallelism
          ? std::min(8u, requestedParallelism)
          : std::max(1u, std::min(8u,
                std::thread::hardware_concurrency() > 2
                    ? std::thread::hardware_concurrency() - 2 : 1u))),
      worker(&RenderSession::Run, this)
{
}

RenderSession::~RenderSession()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        exiting = true;
        ++generation;
        activeGeneration.store(generation, std::memory_order_release);
    }
    wake.notify_all();
    worker.join();
}

std::uint64_t RenderSession::Start(RenderInput input)
{
    if (input.width <= 0 || input.height <= 0 || input.targetSamples <= 0 ||
        input.bounces < 0 || input.lightSamples <= 0 ||
        (input.interactivePreview &&
         input.previewDeadline == std::chrono::steady_clock::time_point{}) ||
        (!std::isfinite(input.horizontalFovDegrees) ||
          input.horizontalFovDegrees <= 0 || input.horizontalFovDegrees >= 180 ||
          !std::isfinite(input.cameraClipNear) ||
          !std::isfinite(input.cameraClipFar) ||
          input.cameraClipNear <= 0 || input.cameraClipFar <= input.cameraClipNear ||
          !std::isfinite(input.cameraForward.x) ||
          !std::isfinite(input.cameraForward.y) ||
          !std::isfinite(input.cameraForward.z) ||
          std::abs(length(input.cameraForward) - 1.0) > 1.0e-4))
        throw std::invalid_argument("invalid offline render settings");
    if (input.scene.directionalLight && input.scene.hasAreaEmitter)
        throw std::invalid_argument("combined directional and area light is not supported");
    for (const MeshInstance& instance : input.scene.instances)
    {
        if (!IsOwned(instance.mesh, input.instanceMeshes) ||
            instance.mesh->nodes.empty() ||
            (instance.model == SurfaceModel::ClothFiber &&
             (!IsOwned(instance.fabric, input.instanceFabrics) ||
              !instance.mesh->hasValidUvFrames)) ||
            (instance.groom && !IsOwned(instance.groom, input.instanceGrooms)) ||
            (instance.baseColorMap &&
             !IsOwned(instance.baseColorMap, input.instanceColorMaps)) ||
            (instance.sheenColorMap &&
             !IsOwned(instance.sheenColorMap, input.instanceColorMaps)))
            throw std::invalid_argument("offline instance references an unowned asset");
    }
    auto candidate = std::make_shared<const RenderInput>(std::move(input));
    std::uint64_t task;
    {
        std::lock_guard<std::mutex> lock(mutex);
        // 提交只替换一个 pending；旧行 worker 在下一次检查代数时退出，不排队积累旧请求。
        ++generation;
        activeGeneration.store(generation, std::memory_order_release);
        task = generation;
        pending = std::move(candidate);
        targetSamples = pending->targetSamples;
        previewMode = pending->interactivePreview;
        // 上一张图仍可供显示，但其 generation 不等于当前任务；新结果不会
        // 与其累积，消费者必须按代数区分新旧输入。
        paused = false;
        pauseRequested.store(false, std::memory_order_release);
        error.clear();
        state = SessionState::Preparing;
    }
    wake.notify_all();
    return task;
}

bool RenderSession::ExtendTargetSamples(int requestedSamples)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (previewMode || requestedSamples <= targetSamples ||
            (state != SessionState::Preparing &&
             state != SessionState::Rendering &&
             state != SessionState::Paused &&
             state != SessionState::Completed))
            return false;
        targetSamples = requestedSamples;
        if (latest && latest->generation == generation)
        {
            // 发布副本的目标与控制目标同步，旧 Capture 仍保持不可变。
            auto updated = std::make_shared<FilmSnapshot>(*latest);
            updated->targetSamples = requestedSamples;
            latest = std::move(updated);
        }
        if (state == SessionState::Completed)
            state = SessionState::Rendering;
    }
    wake.notify_all();
    return true;
}

void RenderSession::Pause()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (state == SessionState::Rendering || state == SessionState::Preparing ||
        state == SessionState::Completed)
    {
        paused = true;
        pauseRequested.store(true, std::memory_order_release);
        state = SessionState::Paused;
    }
}

void RenderSession::Resume()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!paused) return;
        paused = false;
        pauseRequested.store(false, std::memory_order_release);
        state = previewMode
            ? (latestPreview && latestPreview->generation == generation
                ? SessionState::Completed : SessionState::Rendering)
            : (latest && latest->generation == generation &&
                latest->completedSamples >= targetSamples
            ? SessionState::Completed : SessionState::Rendering);
    }
    wake.notify_all();
}

void RenderSession::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++generation;
        activeGeneration.store(generation, std::memory_order_release);
        pending.reset();
        paused = false;
        pauseRequested.store(false, std::memory_order_release);
        // 停止保留最后一张图，但 generation 已改变，消费者能识别它是旧结果。
        state = SessionState::Canceled;
    }
    wake.notify_all();
}

SessionState RenderSession::State() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return state;
}

std::string RenderSession::Error() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return error;
}

std::shared_ptr<const FilmSnapshot> RenderSession::Latest() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return latest;
}

std::shared_ptr<const PreviewSnapshot> RenderSession::LatestPreview() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return latestPreview;
}

void RenderSession::Run()
{
    for (;;)
    {
        std::shared_ptr<const RenderInput> input;
        std::uint64_t task;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [&] { return exiting || pending != nullptr; });
            if (exiting) return;
            input = std::move(pending);
            task = generation;
        }
        try
        {
            Render(task, *input);
        }
        catch (const std::exception& exception)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (task == generation)
            {
                error = exception.what();
                state = SessionState::Failed;
            }
        }
    }
}

void RenderSession::RenderRows(SampleWork& work, int sample)
{
    const RenderInput& input = work.input;
    for (;;)
    {
        if (work.failed.load(std::memory_order_acquire) ||
            activeGeneration.load(std::memory_order_acquire) != work.generation)
            return;
        if (pauseRequested.load(std::memory_order_acquire))
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [&] {
                return exiting || work.generation != generation || !paused ||
                       work.failed.load(std::memory_order_acquire);
            });
            if (exiting || work.generation != generation || work.failed.load())
                return;
        }
        const int y = work.nextRow.fetch_add(1, std::memory_order_relaxed);
        if (y >= input.height) return;
        for (int x = 0; x < input.width; ++x)
        {
            if ((x & 15) == 0)
            {
                if (work.failed.load(std::memory_order_acquire) ||
                    activeGeneration.load(std::memory_order_acquire) != work.generation)
                    return;
                if (pauseRequested.load(std::memory_order_acquire))
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait(lock, [&] {
                        return exiting || work.generation != generation || !paused ||
                               work.failed.load(std::memory_order_acquire);
                    });
                    if (exiting || work.generation != generation || work.failed.load())
                        return;
                }
            }
            // 每个像素只由一个 worker 写入；种子、射线和累积次序与串行路径完全一致。
            Random random = PixelRandom(x, y, input.width, sample, input.scene.seed);
            const Ray ray = CameraRay(input, x, y, random);
            const PathResult path = TraceCameraPath(input, work.albedo, ray, random);
            const std::size_t index = static_cast<std::size_t>(y) * input.width + x;
            work.direct[index] += path.direct;
            work.indirect[index] += path.indirect;
        }
    }
}

void RenderSession::RunSampleWorker(SampleWork& work)
{
    std::uint64_t observed = 0;
    for (;;)
    {
        int sample;
        {
            std::unique_lock<std::mutex> lock(work.mutex);
            work.ready.wait(lock, [&] { return work.stopping || observed != work.serial; });
            if (work.stopping) return;
            observed = work.serial;
            sample = work.sample;
        }
        try
        {
            RenderRows(work, sample);
        }
        catch (...)
        {
            {
                std::lock_guard<std::mutex> lock(work.mutex);
                if (!work.failure) work.failure = std::current_exception();
                work.failed.store(true, std::memory_order_release);
            }
            wake.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(work.mutex);
            --work.remaining;
        }
        work.finished.notify_one();
    }
}

void RenderSession::RunPreviewWorker(PreviewWork& work)
{
    try
    {
        for (;;)
        {
            std::size_t tile;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] {
                    return exiting || work.generation != generation ||
                        !paused || work.failed.load(std::memory_order_acquire);
                });
                if (exiting || work.generation != generation ||
                    work.failed.load(std::memory_order_acquire) ||
                    std::chrono::steady_clock::now() >= work.input.previewDeadline)
                    return;
                tile = work.nextTile.fetch_add(1, std::memory_order_relaxed);
            }
            if (tile >= work.tileCount) return;
            const std::size_t scattered = tile * work.stride % work.tileCount;
            const int startX = static_cast<int>(scattered % work.tilesX) * 8;
            const int startY = static_cast<int>(scattered / work.tilesX) * 8;
            std::size_t completed = 0;
            for (int y = startY; y < std::min(startY + 8, work.input.height); ++y)
                for (int x = startX; x < std::min(startX + 8, work.input.width); ++x)
                {
                    // 一个 tile 领取后算完再标有效；超时只阻止后续领取，
                    // 取消的 generation 则在统一发布点丢弃整个候选。
                    Random random = PixelRandom(x, y, work.input.width, 0,
                        work.input.scene.seed);
                    const Ray ray = CameraRay(work.input, x, y, random);
                    const PathResult path = TraceCameraPath(
                        work.input, work.albedo, ray, random);
                    const std::size_t index = static_cast<std::size_t>(y) *
                        work.input.width + x;
                    work.snapshot.direct[index] = path.direct;
                    work.snapshot.indirect[index] = path.indirect;
                    ++completed;
                }
            for (int y = startY; y < std::min(startY + 8, work.input.height); ++y)
                for (int x = startX; x < std::min(startX + 8, work.input.width); ++x)
                    work.snapshot.valid[static_cast<std::size_t>(y) *
                        work.input.width + x] = 1;
            work.validCount.fetch_add(completed, std::memory_order_relaxed);
        }
    }
    catch (...)
    {
        {
            std::lock_guard<std::mutex> lock(work.failureMutex);
            if (!work.failure) work.failure = std::current_exception();
            work.failed.store(true, std::memory_order_release);
        }
        wake.notify_all();
    }
}

void RenderSession::RenderPreview(std::uint64_t task, const RenderInput& input)
{
    const std::size_t count = static_cast<std::size_t>(input.width) * input.height;
    auto preview = std::make_shared<PreviewSnapshot>();
    preview->generation = task;
    preview->width = input.width;
    preview->height = input.height;
    preview->valid.resize(count);
    preview->direct.resize(count);
    preview->indirect.resize(count);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (task != generation || exiting) return;
        if (!paused) state = SessionState::Rendering;
    }
    if (std::chrono::steady_clock::now() < input.previewDeadline)
    {
        // 准备阶段已耗尽绝对预算时，不构造 LUT 或启动 tile worker。
        const AlbedoTable& albedo = SharedAlbedoTable();
        PreviewWork work(input, albedo, *preview, task);
        // 独立预览只计算 sample 0 的分散 tile，不改变原完整样本路径的
        // 行分配、随机流或逐像素累积顺序；join 后才发布不可变有效掩码。
        const unsigned workers = static_cast<unsigned>(
            std::min<std::size_t>(parallelism, work.tileCount));
        for (unsigned i = 1; i < workers; ++i)
            work.helpers.emplace_back(&RenderSession::RunPreviewWorker, this, std::ref(work));
        RunPreviewWorker(work);
        for (std::thread& thread : work.helpers)
            if (thread.joinable()) thread.join();
        if (work.failure) std::rethrow_exception(work.failure);
        preview->validCount = work.validCount.load(std::memory_order_relaxed);
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [&] { return exiting || task != generation || !paused; });
        if (task != generation || exiting) return;
        // 即使预算在准备阶段耗尽、validCount==0，也发布可识别的空预览；
        // 显示层按 mask 保留旧像素，Latest() 的正式 film 永不被预览替换。
        latestPreview = std::move(preview);
        state = SessionState::Completed;
    }
}

void RenderSession::Render(std::uint64_t task, RenderInput input)
{
    // 实例裸指针由 RenderInput 的冻结资源持有；worker 不访问活动 World/MI/Vulkan。
    if (input.interactivePreview)
    {
        RenderPreview(task, input);
        return;
    }
    const AlbedoTable& albedo = SharedAlbedoTable();
    const std::size_t count = static_cast<std::size_t>(input.width) * input.height;
    std::vector<Vec3> direct(count), indirect(count);
    SampleWork work(input, albedo, direct, indirect, task);
    const unsigned helperCount = std::min(parallelism,
        static_cast<unsigned>(input.height)) - 1;
    // 行 worker 在本次 Render 内复用；析构时先唤醒再 join，避免悬空场景引用。
    for (unsigned i = 0; i < helperCount; ++i)
        work.helpers.emplace_back(&RenderSession::RunSampleWorker, this, std::ref(work));
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (task != generation) return;
        if (!paused) state = SessionState::Rendering;
    }
    for (int sample = 0;; ++sample)
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            // 完成态保留累积和样本序号；提高目标 spp 不重新播种或重复计算。
            wake.wait(lock, [&] {
                return exiting || task != generation ||
                       (!paused && sample < targetSamples);
            });
            if (exiting || task != generation) return;
        }
        work.nextRow.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(work.mutex);
            work.sample = sample;
            work.remaining = helperCount;
            ++work.serial;
        }
        work.ready.notify_all();
        std::exception_ptr localFailure;
        try
        {
            RenderRows(work, sample);
        }
        catch (...)
        {
            localFailure = std::current_exception();
            work.failed.store(true, std::memory_order_release);
            wake.notify_all();
        }
        {
            std::unique_lock<std::mutex> lock(work.mutex);
            // 所有行 worker 退出本批后才允许复制、发布；取消的半样本直接丢弃。
            work.finished.wait(lock, [&] { return work.remaining == 0; });
            if (!localFailure) localFailure = work.failure;
        }
        if (localFailure) std::rethrow_exception(localFailure);
        if (activeGeneration.load(std::memory_order_acquire) != task) return;
        auto film = std::make_shared<FilmSnapshot>();
        film->generation = task;
        film->width = input.width;
        film->height = input.height;
        film->completedSamples = sample + 1;
        film->direct.resize(count);
        film->indirect.resize(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            film->direct[i] = direct[i] / (sample + 1);
            film->indirect[i] = indirect[i] / (sample + 1);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            // 即使计算线程刚完成一批，发布点也必须再次检查代数。
            if (task != generation || exiting) return;
            film->targetSamples = targetSamples;
            latest = std::move(film);
            if (sample + 1 == targetSamples)
            {
                paused = false;
                state = SessionState::Completed;
            }
        }
    }
}

} // namespace VL::Offline
