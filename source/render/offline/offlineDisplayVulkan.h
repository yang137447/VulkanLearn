#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "core/renderSession.h"

namespace VL
{
class RendererBackendVulkan;

enum class OfflineAov { Combined, Direct, Indirect };

struct OfflineDisplayUploadState
{
    uint64_t generation = 0;
    int completedSamples = 0;
    OfflineAov aov = OfflineAov::Combined;
    bool initialized = false;
    bool preview = false;
    size_t validCount = 0;
};

inline bool IsOfflineFilmExtentCompatible(int filmWidth, int filmHeight,
    uint32_t displayWidth, uint32_t displayHeight) noexcept
{
    if (filmWidth <= 0 || filmHeight <= 0 || displayWidth == 0 || displayHeight == 0 ||
        static_cast<uint64_t>(filmWidth) > displayWidth ||
        static_cast<uint64_t>(filmHeight) > displayHeight)
        return false;

    const uint64_t width = static_cast<uint32_t>(filmWidth);
    const uint64_t height = static_cast<uint32_t>(filmHeight);
    if (width * displayHeight == height * displayWidth) return true;

    // 奇数窗口的半分辨率无法严格同宽高比；仅接受各轴 floor/ceil(窗口/2)，
    // 不把任意比例失配误判成合法预览尺寸。
    const uint32_t floorWidth = displayWidth / 2;
    const uint32_t floorHeight = displayHeight / 2;
    if ((width == floorWidth || width == displayWidth - floorWidth) &&
        (height == floorHeight || height == displayHeight - floorHeight))
        return true;

    // 仅接受同一 divisor 下各轴向上取整的预览，不容忍任意宽高比失配；
    // 分开计算商和余数，避免大窗口加 divisor - 1 溢出。
    constexpr uint32_t divisors[] = {4, 8, 16};
    for (const uint32_t divisor : divisors)
    {
        const uint32_t previewWidth = displayWidth / divisor + (displayWidth % divisor != 0);
        const uint32_t previewHeight = displayHeight / divisor + (displayHeight % divisor != 0);
        if (width == previewWidth && height == previewHeight) return true;
    }
    return false;
}

// 目标像素中心投影到源 film；仅在已验证 0 <= pixel < displayExtent、
// 0 < filmExtent <= displayExtent 时调用，整数运算避免大尺寸乘二溢出。
inline uint32_t OfflineFilmSourceCoordinate(uint32_t pixel,
    uint32_t displayExtent, uint32_t filmExtent) noexcept
{
    const uint64_t numerator = uint64_t(pixel) * filmExtent;
    const uint64_t whole = numerator / displayExtent;
    const uint64_t remainder = numerator % displayExtent;
    return static_cast<uint32_t>(whole +
        (2 * remainder + filmExtent >= 2 * uint64_t(displayExtent)));
}

// 每个 image slot 只记上传身份；同一任务的同一样本批次是不可变的。
inline bool NeedsOfflineFilmUpload(const Offline::FilmSnapshot& film,
    uint32_t width, uint32_t height, OfflineAov aov, uint64_t latestGeneration,
    const OfflineDisplayUploadState& uploaded) noexcept
{
    if (!IsOfflineFilmExtentCompatible(film.width, film.height, width, height))
        return false;
    const size_t pixelCount = size_t(film.width) * size_t(film.height);
    return film.direct.size() == pixelCount && film.indirect.size() == pixelCount &&
        film.completedSamples > 0 &&
        film.generation >= latestGeneration &&
        (!uploaded.initialized || film.generation != uploaded.generation ||
         film.completedSamples >= uploaded.completedSamples) &&
        (!uploaded.initialized || uploaded.generation != film.generation ||
         uploaded.completedSamples != film.completedSamples || uploaded.aov != aov);
}

// RT 保留最近一次有效完整 film，供尚未追上新代的 swapchain slot 补传；
// 输入缺席或旧批次不能替换已展示的新视角。
inline std::shared_ptr<const Offline::FilmSnapshot> SelectOfflineDisplayFilm(
    std::shared_ptr<const Offline::FilmSnapshot> incoming,
    const std::shared_ptr<const Offline::FilmSnapshot>& retained,
    uint32_t width, uint32_t height) noexcept
{
    const uint64_t latestGeneration = retained ? retained->generation : 0;
    const OfflineDisplayUploadState retainedState = retained
        ? OfflineDisplayUploadState{retained->generation, retained->completedSamples,
            OfflineAov::Combined, true}
        : OfflineDisplayUploadState{};
    if (incoming && NeedsOfflineFilmUpload(*incoming, width, height,
        OfflineAov::Combined, latestGeneration, retainedState))
        return incoming;
    return retained;
}

struct OfflineDisplaySource
{
    std::shared_ptr<const Offline::FilmSnapshot> film;
    std::shared_ptr<const Offline::PreviewSnapshot> preview;

    uint64_t Generation() const noexcept
    {
        return film ? film->generation : preview ? preview->generation : 0;
    }
};

inline bool IsOfflinePreviewUsable(const Offline::PreviewSnapshot& preview,
    uint32_t width, uint32_t height) noexcept
{
    if (!IsOfflineFilmExtentCompatible(preview.width, preview.height, width, height))
        return false;
    const size_t count = size_t(preview.width) * size_t(preview.height);
    return preview.validCount > 0 && preview.validCount <= count &&
        preview.valid.size() == count && preview.direct.size() == count &&
        preview.indirect.size() == count;
}

// 完整 film 优先于同代预览；不同代只接受更新的任务，避免旧视角回摆。
inline OfflineDisplaySource SelectOfflineDisplaySource(
    OfflineDisplaySource retained,
    std::shared_ptr<const Offline::FilmSnapshot> film,
    std::shared_ptr<const Offline::PreviewSnapshot> preview,
    uint32_t width, uint32_t height) noexcept
{
    if (film && IsOfflineFilmExtentCompatible(film->width, film->height, width, height))
    {
        const size_t count = size_t(film->width) * size_t(film->height);
        if (film->completedSamples > 0 && film->direct.size() == count &&
            film->indirect.size() == count &&
            (film->generation > retained.Generation() ||
             (film->generation == retained.Generation() &&
              (retained.preview ||
               (retained.film &&
                film->completedSamples > retained.film->completedSamples)))))
            retained = {std::move(film), {}};
    }
    if (preview && IsOfflinePreviewUsable(*preview, width, height) &&
        (preview->generation > retained.Generation() ||
         (retained.preview && preview->generation == retained.Generation() &&
          preview->validCount > retained.preview->validCount)))
        retained = {{}, std::move(preview)};
    return retained;
}

inline bool NeedsOfflinePreviewUpload(const Offline::PreviewSnapshot& preview,
    uint32_t width, uint32_t height, OfflineAov aov,
    const OfflineDisplayUploadState& uploaded) noexcept
{
    return IsOfflinePreviewUsable(preview, width, height) &&
        (!uploaded.initialized || uploaded.generation < preview.generation ||
         (uploaded.generation == preview.generation && uploaded.preview &&
          (uploaded.validCount < preview.validCount ||
           (uploaded.validCount == preview.validCount && uploaded.aov != aov))));
}

// 行优先种子、固定左右上下顺序的多源 BFS：并列距离稳定选择同预览的有效像素。
inline std::vector<size_t> OfflinePreviewNearestValid(const Offline::PreviewSnapshot& preview)
{
    if (preview.width <= 0 || preview.height <= 0 || preview.validCount == 0) return {};
    const size_t count = size_t(preview.width) * size_t(preview.height);
    if (preview.valid.size() != count) return {};
    std::vector<size_t> nearest(count, std::numeric_limits<size_t>::max());
    std::vector<size_t> queue;
    queue.reserve(count);
    for (size_t pixel = 0; pixel < count; ++pixel)
        if (preview.valid[pixel])
        {
            nearest[pixel] = pixel;
            queue.push_back(pixel);
        }
    if (queue.empty()) return {};
    const size_t stride = static_cast<size_t>(preview.width);
    for (size_t head = 0; head < queue.size(); ++head)
    {
        const size_t pixel = queue[head];
        const size_t neighbors[] = {
            pixel % stride ? pixel - 1 : count,
            pixel % stride + 1 < stride ? pixel + 1 : count,
            pixel >= stride ? pixel - stride : count,
            pixel + stride < count ? pixel + stride : count};
        for (size_t neighbor : neighbors)
            if (neighbor < count && nearest[neighbor] == std::numeric_limits<size_t>::max())
            {
                nearest[neighbor] = nearest[pixel];
                queue.push_back(neighbor);
            }
    }
    return nearest;
}

inline Offline::Vec3 OfflinePreviewColor(const Offline::PreviewSnapshot& preview,
    size_t source, OfflineAov aov)
{
    return aov == OfflineAov::Direct ? preview.direct[source]
        : aov == OfflineAov::Indirect ? preview.indirect[source]
        : preview.direct[source] + preview.indirect[source];
}

// RT 独占显示资源；输入是 worker 发布的不可变线性 film，不负责积分或 World 访问。
// 每个 acquired swapchain image 对应独立上传资源，其 fence 保护映射内存复用。
class OfflineDisplayVulkan
{
public:
    void Initialize(RendererBackendVulkan& backend);
    void Shutdown();
    void Record(vk::CommandBuffer commandBuffer, uint32_t imageIndex,
                std::shared_ptr<const Offline::FilmSnapshot> film,
                std::shared_ptr<const Offline::PreviewSnapshot> preview,
                OfflineAov aov, float exposure);
    void Record(vk::CommandBuffer commandBuffer, uint32_t imageIndex,
                std::shared_ptr<const Offline::FilmSnapshot> film,
                OfflineAov aov, float exposure)
    {
        Record(commandBuffer, imageIndex, std::move(film), {}, aov, exposure);
    }

private:
    struct Frame
    {
        vk::Buffer staging;
        vk::DeviceMemory stagingMemory;
        void* mapped = nullptr;
        vk::Image image;
        vk::DeviceMemory imageMemory;
        vk::ImageView view;
        vk::Sampler sampler;
        vk::DescriptorSet descriptor;
        OfflineDisplayUploadState uploaded;
    };
    void CreatePipeline();
    RendererBackendVulkan* backend = nullptr;
    vk::DescriptorSetLayout descriptorLayout;
    vk::DescriptorPool descriptorPool;
    vk::PipelineLayout pipelineLayout;
    vk::Pipeline pipeline;
    vk::PipelineCache pipelineCache;
    vk::RenderPass renderPass;
    std::vector<vk::Framebuffer> framebuffers;
    std::vector<Frame> frames;
    OfflineDisplaySource retainedSource;
    std::vector<size_t> previewNearest;
};
} // namespace VL
