#pragma once

#include <chrono>
#include <exception>
#include <future>
#include <optional>
#include <utility>

namespace VL
{

// GT 持有的单槽 CPU 任务：换请求只作废结果，轮询不等待；析构收口唯一 worker。
// 输入由缩略图会话按值冻结，任务不得访问 Vulkan 或活动资源缓存。
template<class Result>
class EditorThumbnailJob
{
public:
    template<class Function, class Request>
    bool Start(Function function, Request request)
    {
        if (future.valid()) return false;
        future = std::async(std::launch::async, function, std::move(request));
        current = true;
        return true;
    }

    void Invalidate() { current = false; }
    bool HasTask() const { return future.valid(); }
    void Wait() const { if (future.valid()) future.wait(); }

    std::optional<Result> Poll()
    {
        if (!future.valid() ||
            future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return {};
        if (!current)
        {
            // 陈旧失败不能污染最新请求；get 同时释放 future 的共享状态。
            try { future.get(); }
            catch (const std::exception&) {}
            return {};
        }
        return future.get();
    }

private:
    std::future<Result> future;
    bool current = false;
};

} // namespace VL
