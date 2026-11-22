#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace VL
{

// 一个 timestamp query 槽位（通常对应一个 swapchain image）的录制/提交批次。
// 状态边界：
//   recording  —— 本轮 query 命令已写进（尚未提交的）命令缓冲；
//   open[i]    —— query i 的 begin 已写、end 未写，批次不完整；
//   submitted  —— 本轮命令已随 fence 成功提交，下一次复用槽位才允许读回。
// 被丢弃（Discard/Abort）的帧必须调用 Aborted()：这些 query 从未在 GPU 上写入，
// 下一次复用槽位若仍按 submitted 读取，会得到 VK_NOT_READY 并中断渲染线程。
// 已提交但尚未读回的旧批次不受 Aborted() 影响，仍由下一次 BeginFrame 读回。
class GpuTimestampQueryBatch
{
public:
    explicit GpuTimestampQueryBatch(size_t queryCount)
        : recorded(queryCount, false), open(queryCount, false)
    {
    }

    // 开始新一轮录制。调用方必须先读回上一轮 submitted 批次。
    void BeginRecording() noexcept
    {
        std::fill(recorded.begin(), recorded.end(), false);
        std::fill(open.begin(), open.end(), false);
        recording = false;
    }

    // 同一 query 一帧只允许 begin 一次；返回 false 表示重复或索引越界。
    bool Begin(size_t index) noexcept
    {
        if (index >= recorded.size() || recorded[index])
            return false;
        recorded[index] = true;
        open[index] = true;
        recording = true;
        return true;
    }

    void End(size_t index) noexcept
    {
        if (index < open.size())
            open[index] = false;
    }

    // 只有本轮的每个 begin 都有对应 end 时，批次才是完整可读的。
    void Submitted() noexcept
    {
        submitted = recording && !AnyOpen();
        recording = false;
    }

    // 命令被丢弃：本轮 query 从未提交，禁止下一次读回。
    void Aborted() noexcept
    {
        std::fill(recorded.begin(), recorded.end(), false);
        std::fill(open.begin(), open.end(), false);
        recording = false;
    }

    bool HasSubmitted() const noexcept { return submitted; }
    void Consumed() noexcept { submitted = false; }

    bool WasRecorded(size_t index) const noexcept
    {
        return index < recorded.size() && recorded[index];
    }

    bool IsOpen(size_t index) const noexcept
    {
        return index < open.size() && open[index];
    }

    size_t Size() const noexcept { return recorded.size(); }

private:
    bool AnyOpen() const noexcept
    {
        return std::any_of(open.begin(), open.end(),
            [](bool value) { return value; });
    }

    std::vector<bool> recorded;
    std::vector<bool> open;
    bool recording = false;
    bool submitted = false;
};

} // namespace VL
