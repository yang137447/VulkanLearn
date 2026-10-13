#include "editorThumbnailLibrary.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include "editorJsonAssetLibrary.h"
#include "resource/image/textureIO.h"
#include "shader/build/atomicFile.h"
#include "shader/build/contentHash.h"

namespace VL::Editor
{
namespace
{
using Thumbnail = EditorUi::EditorThumbnail;
using Snapshot = EditorUi::EditorThumbnailSnapshot;
constexpr uint32_t Side = 256;
constexpr size_t PixelBytes = Side * Side * 4;
constexpr size_t MaxMemoryEntries = 256;
constexpr size_t MaxDiskEntries = 512;
std::atomic<uint64_t> NextEpoch{1};

std::filesystem::path ResolveSource(const std::filesystem::path& root, const std::string& source)
{
    std::string normalized = source;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const auto relative = std::filesystem::u8path(normalized);
    if (relative.empty() || relative.has_root_path() || normalized.find(':') != std::string::npos ||
        normalized.find('\0') != std::string::npos)
        throw std::runtime_error("Thumbnail source must be resource-root relative");
    for (const auto& part : relative)
        if (part == "..") throw std::runtime_error("Thumbnail source escapes resource root");
    const auto physical = std::filesystem::canonical(root / relative);
    const auto fromRoot = physical.lexically_relative(root);
    if (fromRoot.empty() || *fromRoot.begin() == ".." || !std::filesystem::is_regular_file(physical))
        throw std::runtime_error("Thumbnail source is missing or outside resource root");
    return physical;
}

std::vector<uint8_t> ReadBounded(const std::filesystem::path& path, size_t limit)
{
    const auto size = std::filesystem::file_size(path);
    if (size == 0 || size > limit) throw std::runtime_error("Thumbnail input exceeds size limit");
    std::vector<uint8_t> bytes(static_cast<size_t>(size) + 1);
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream.eof() || static_cast<size_t>(stream.gcount()) != size)
        throw std::runtime_error("Thumbnail source changed while reading");
    bytes.resize(static_cast<size_t>(size));
    return bytes;
}

float ToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

uint8_t DisplayByte(float value)
{
    value = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(std::lround(value * 255.0f), 0l, 255l));
}

std::shared_ptr<const EditorUi::EditorTexturePreview> MakeImage(
    const std::vector<uint8_t>& bytes, bool srgb, bool transparency)
{
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels) ||
        width <= 0 || height <= 0 || uint64_t(width) * uint64_t(height) > 32 * 1024 * 1024)
        throw std::runtime_error("Unsupported image or thumbnail pixel limit exceeded (32 MP)");
    const bool hdr = stbi_is_hdr_from_memory(bytes.data(), static_cast<int>(bytes.size())) != 0;
    TextureIO::LoadOptions options;
    options.forceChannels = 4;
    if (hdr) options.formatHint = TextureIO::FileFormat::Hdr;
    auto decoded = TextureIO::LoadFromMemory(bytes.data(), bytes.size(), options);
    if (!decoded || (decoded->format != HostImage::PixelFormat::RGBA8_UNORM &&
        decoded->format != HostImage::PixelFormat::RGBA32_FLOAT))
        throw std::runtime_error("Thumbnail image decoding failed");
    auto image = std::make_shared<EditorUi::EditorTexturePreview>();
    image->sourceWidth = width;
    image->sourceHeight = height;
    image->width = image->height = Side;
    image->pixels.resize(PixelBytes);
    const double scale = double(Side) / std::max(width, height);
    const uint32_t outputWidth = std::max(1u, uint32_t(width * scale));
    const uint32_t outputHeight = std::max(1u, uint32_t(height * scale));
    const uint32_t left = (Side - outputWidth) / 2, top = (Side - outputHeight) / 2;
    float lookup[256];
    for (size_t i = 0; i < 256; ++i) lookup[i] = srgb ? ToLinear(i / 255.0f) : i / 255.0f;
    for (uint32_t y = 0; y < Side; ++y)
        for (uint32_t x = 0; x < Side; ++x)
        {
            const size_t target = (size_t(y) * Side + x) * 4;
            image->pixels[target + 3] = 255;
            if (x < left || x >= left + outputWidth || y < top || y >= top + outputHeight)
            {
                for (size_t c = 0; c < 3; ++c) image->pixels[target + c] = 35;
                continue;
            }
            const uint32_t x0 = uint64_t(x - left) * width / outputWidth;
            const uint32_t x1 = std::max(x0 + 1, uint32_t(uint64_t(x - left + 1) * width / outputWidth));
            const uint32_t y0 = uint64_t(y - top) * height / outputHeight;
            const uint32_t y1 = std::max(y0 + 1, uint32_t(uint64_t(y - top + 1) * height / outputHeight));
            float sum[3] = {}, alpha = 0.0f;
            // 缩小在线性域做面积平均。仅显式 opacity/alpha 语义才合成透明度，
            // 参数图的打包 alpha 不能把 RGB 藏掉；HDR 固定曝光映射，不逐图自动曝光。
            for (uint32_t sy = y0; sy < y1; ++sy)
                for (uint32_t sx = x0; sx < x1; ++sx)
                {
                    const size_t offset = (size_t(sy) * width + sx) * 4;
                    float values[4];
                    if (hdr) std::memcpy(values, decoded->data.data() + offset * sizeof(float), sizeof(values));
                    else
                    {
                        for (size_t c = 0; c < 3; ++c) values[c] = lookup[decoded->data[offset + c]];
                        values[3] = decoded->data[offset + 3] / 255.0f;
                    }
                    const float a = transparency && !hdr ? values[3] : 1.0f;
                    alpha += a;
                    for (size_t c = 0; c < 3; ++c)
                    {
                        if (!std::isfinite(values[c]) || values[c] < 0.0f)
                            throw std::runtime_error("Thumbnail image contains invalid color values");
                        sum[c] += values[c] * a;
                    }
                }
            const float count = float((x1 - x0) * (y1 - y0));
            const float checker = ToLinear(((x / 12 + y / 12) % 2 ? 100.0f : 65.0f) / 255.0f);
            for (size_t c = 0; c < 3; ++c)
            {
                float value = sum[c] / count;
                if (hdr) value = value / (1.0f + value);
                image->pixels[target + c] = DisplayByte(value + checker * (1.0f - alpha / count));
            }
        }
    return image;
}

std::shared_ptr<const EditorUi::EditorTexturePreview> ReadCache(const std::filesystem::path& path)
{
    try
    {
        const auto bytes = ReadBounded(path, PixelBytes + ContentDigest::Size);
        if (bytes.size() != PixelBytes + ContentDigest::Size) return {};
        const auto hash = ContentHasher::HashBytes(bytes.data() + ContentDigest::Size, PixelBytes);
        if (!std::equal(hash.bytes.begin(), hash.bytes.end(), bytes.begin())) return {};
        auto image = std::make_shared<EditorUi::EditorTexturePreview>();
        image->width = image->height = Side;
        image->pixels.assign(bytes.begin() + ContentDigest::Size, bytes.end());
        return image;
    }
    catch (const std::exception&) { return {}; }
}

void TrimDiskCache(const std::filesystem::path& root)
{
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    {
        const std::string key = it->path().stem().string();
        if (it->path().extension() != ".rgba" || key.size() != 64 ||
            key.find_first_not_of("0123456789abcdef") != std::string::npos || it->is_symlink(error)) continue;
        const auto time = it->last_write_time(error);
        if (!error) files.emplace_back(time, it->path());
    }
    std::sort(files.begin(), files.end());
    for (size_t i = 0; files.size() > MaxDiskEntries && i < files.size() - MaxDiskEntries; ++i)
        std::filesystem::remove(files[i].second, error);
}

Thumbnail Generate(const std::filesystem::path& root, const std::filesystem::path& cache,
    const std::string& path)
{
    std::string error;
    if (!ValidateEditorJsonAssetPath(path, error)) throw std::runtime_error(error);
    const auto descriptor = ReadBounded(ResolveSource(root, path), EditorJsonAssetLimits::MaxFileBytes);
    EditorUi::EditorJsonDocumentSnapshot parsed;
    if (!ParseEditorJsonAsset(path, {reinterpret_cast<const char*>(descriptor.data()), descriptor.size()}, parsed) ||
        !parsed.diagnostic.empty()) throw std::runtime_error("Invalid thumbnail asset descriptor");
    if (parsed.kind != EditorUi::EditorAssetKind::Texture)
        throw std::runtime_error("Rendered thumbnail is not available for this asset type");
    if (parsed.textureColorSpace != "srgb" && parsed.textureColorSpace != "linear")
        throw std::runtime_error("Texture colorSpace must be srgb or linear");
    const auto source = ReadBounded(ResolveSource(root, parsed.textureSource), 64 * 1024 * 1024);
    CanonicalFieldHasher hash("editor-texture-thumbnail-v1");
    hash.AddBytes("descriptor", descriptor.data(), descriptor.size());
    hash.AddBytes("source", source.data(), source.size());
    hash.AddUInt32("size", Side);
    const std::string key = hash.Finalize().ToHex();
    Thumbnail result;
    result.source = "vl://thumbnail/" + key;
    const auto file = cache / (key + ".rgba");
    result.image = ReadCache(file);
    result.diskCacheHit = result.image != nullptr;
    if (result.image) return result;
    const auto json = nlohmann::json::parse(descriptor);
    std::string alpha;
    if (json.contains("channelsDescription") && json["channelsDescription"].is_object() &&
        json["channelsDescription"].contains("a") && json["channelsDescription"]["a"].is_string())
        alpha = json["channelsDescription"]["a"].get<std::string>();
    result.image = MakeImage(source, parsed.textureColorSpace == "srgb", alpha == "opacity" || alpha == "alpha");
    try
    {
        // 缓存损坏或写盘失败不影响本次显示；固定尺寸载荷有独立摘要，原子替换不发布半张图。
        auto digest = ContentHasher::HashBytes(result.image->pixels.data(), PixelBytes);
        std::vector<uint8_t> bytes(digest.bytes.begin(), digest.bytes.end());
        bytes.insert(bytes.end(), result.image->pixels.begin(), result.image->pixels.end());
        WriteBinaryFileAtomically(file, bytes);
        TrimDiskCache(cache);
    }
    catch (const std::exception&) {}
    return result;
}
} // namespace

struct EditorThumbnailLibrary::Impl
{
    struct Cached { Thumbnail value; uint64_t used = 0; };
    struct Result { std::string path; Thumbnail value; uint64_t epoch = 0; };
    std::filesystem::path root, cacheRoot;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool stopping = false, paused = false;
    uint64_t epoch = NextEpoch.fetch_add(1), clock = 0;
    std::vector<std::string> visible;
    std::unordered_map<std::string, Cached> cache;
    std::optional<Result> ready;
    std::optional<ThumbnailRenderRequest> renderRequest;
    bool renderTaken = false;
    uint64_t requestId = 0;
    std::shared_ptr<const Snapshot> snapshot = std::make_shared<Snapshot>();

    bool Wanted(const std::string& path) const
    {
        return std::find(visible.begin(), visible.end(), path) != visible.end();
    }
    std::string Next() const
    {
        if (paused || ready || renderRequest) return {};
        for (const auto& path : visible)
            if (!cache.count(path)) return path;
        return {};
    }
    void Run()
    {
        TrimDiskCache(cacheRoot);
        for (;;)
        {
            std::unique_lock<std::mutex> lock(mutex);
            while (!stopping && Next().empty()) wake.wait(lock);
            if (stopping) return;
            const std::string path = Next();
            const uint64_t generation = epoch;
            lock.unlock();
            Thumbnail value;
            std::optional<EditorUi::EditorAssetKind> renderKind;
            try
            {
                const auto bytes = ReadBounded(ResolveSource(root, path), EditorJsonAssetLimits::MaxFileBytes);
                EditorUi::EditorJsonDocumentSnapshot parsed;
                if (ParseEditorJsonAsset(path, {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, parsed) &&
                    parsed.diagnostic.empty() && (parsed.kind == EditorUi::EditorAssetKind::Mesh ||
                    parsed.kind == EditorUi::EditorAssetKind::MaterialInstance)) renderKind = parsed.kind;
                else value = Generate(root, cacheRoot, path);
            }
            catch (const std::exception& error) { value.diagnostic = error.what(); }
            lock.lock();
            // 快速滚动/刷新期间完成的旧任务不能覆盖当前页；单槽背压限制 GT/GPU 发布速率。
            if (generation == epoch && Wanted(path))
            {
                if (renderKind)
                {
                    renderRequest = ThumbnailRenderRequest{++requestId, generation, path, *renderKind};
                    renderTaken = false;
                }
                else ready = Result{path, std::move(value), generation};
            }
        }
    }
    ~Impl()
    {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
};

EditorThumbnailLibrary::EditorThumbnailLibrary() = default;
EditorThumbnailLibrary::~EditorThumbnailLibrary() = default;

bool EditorThumbnailLibrary::Initialize(const std::filesystem::path& resourceRoot,
    const std::filesystem::path& cacheRoot, std::string& error)
{
    impl.reset();
    try
    {
        auto next = std::make_unique<Impl>();
        next->root = std::filesystem::canonical(resourceRoot);
        if (!std::filesystem::is_directory(next->root)) throw std::runtime_error("Thumbnail resource root is not a directory");
        next->cacheRoot = cacheRoot;
        impl = std::move(next);
        impl->worker = std::thread(&Impl::Run, impl.get());
        error.clear();
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); impl.reset(); return false; }
}

void EditorThumbnailLibrary::RequestVisible(const std::vector<std::string>& paths)
{
    if (!impl) return;
    std::vector<std::string> visible;
    std::set<std::string> unique;
    for (const auto& path : paths)
        if (unique.insert(path).second && visible.size() < MaxEntries) visible.push_back(path);
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->visible == visible) return;
    impl->visible = std::move(visible);
    if (impl->renderRequest && !impl->Wanted(impl->renderRequest->path)) impl->renderRequest.reset();
    for (const auto& path : impl->visible)
        if (impl->cache.count(path)) impl->cache.at(path).used = ++impl->clock;
    impl->wake.notify_all();
}

void EditorThumbnailLibrary::Refresh()
{
    if (!impl) return;
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->epoch = NextEpoch.fetch_add(1);
    impl->ready.reset();
    impl->renderRequest.reset();
    impl->cache.clear();
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->revision = impl->snapshot->revision + 1;
    impl->snapshot = std::move(snapshot);
    impl->wake.notify_all();
}

void EditorThumbnailLibrary::SetPaused(bool paused)
{
    if (!impl) return;
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->paused = paused;
    impl->wake.notify_all();
}

std::optional<ThumbnailRenderRequest> EditorThumbnailLibrary::TakeRenderRequest()
{
    if (!impl) return {};
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->paused || impl->renderTaken || !impl->renderRequest) return {};
    impl->renderTaken = true;
    return impl->renderRequest;
}

bool EditorThumbnailLibrary::IsRenderRequestCurrent(const ThumbnailRenderRequest& request) const
{
    if (!impl) return false;
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->renderRequest && impl->renderRequest->id == request.id &&
        impl->epoch == request.epoch && impl->Wanted(request.path);
}

void EditorThumbnailLibrary::CompleteRenderRequest(const ThumbnailRenderRequest& request,
    std::shared_ptr<const EditorUi::EditorTexturePreview> image, const std::string& error)
{
    if (!impl) return;
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->renderRequest || impl->renderRequest->id != request.id || impl->epoch != request.epoch) return;
    Thumbnail value;
    value.source = "vl://thumbnail/render/" + std::to_string(request.epoch) + "/" + std::to_string(request.id);
    value.image = std::move(image);
    value.diagnostic = error;
    impl->ready = Impl::Result{request.path, std::move(value), request.epoch};
    impl->renderRequest.reset();
    impl->wake.notify_all();
}

void EditorThumbnailLibrary::Poll()
{
    if (!impl) return;
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->ready)
    {
        auto result = std::move(*impl->ready);
        impl->ready.reset();
        if (result.epoch == impl->epoch && impl->Wanted(result.path))
            impl->cache[result.path] = {std::move(result.value), ++impl->clock};
        impl->wake.notify_all();
    }
    while (impl->cache.size() > MaxMemoryEntries)
    {
        auto oldest = impl->cache.end();
        for (auto it = impl->cache.begin(); it != impl->cache.end(); ++it)
            if (!impl->Wanted(it->first) && (oldest == impl->cache.end() || it->second.used < oldest->second.used)) oldest = it;
        if (oldest == impl->cache.end()) break;
        impl->cache.erase(oldest);
    }
    bool changed = false;
    for (const auto& item : impl->snapshot->entries)
        if (!impl->Wanted(item.first)) { changed = true; break; }
    if (!impl->paused)
        for (const auto& path : impl->visible)
            if (impl->cache.count(path) && !impl->snapshot->entries.count(path)) { changed = true; break; }
    if (!changed) return;
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->revision = impl->snapshot->revision + 1;
    size_t added = 0;
    for (const auto& path : impl->visible)
    {
        const auto existing = impl->snapshot->entries.find(path);
        if (existing != impl->snapshot->entries.end()) snapshot->entries.emplace(*existing);
        else if (!impl->paused && added < 2 && impl->cache.count(path))
        {
            snapshot->entries.emplace(path, impl->cache.at(path).value);
            ++added;
        }
    }
    impl->snapshot = std::move(snapshot);
}

std::shared_ptr<const Snapshot> EditorThumbnailLibrary::GetSnapshot() const
{
    return impl ? impl->snapshot : nullptr;
}
} // namespace VL::Editor
