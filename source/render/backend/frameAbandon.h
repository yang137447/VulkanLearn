#pragma once

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace VL
{
inline thread_local bool taskFrameWorkerActive = false;

class TaskFrameWorkerScope
{
public:
    TaskFrameWorkerScope() : previous(taskFrameWorkerActive) { taskFrameWorkerActive = true; }
    ~TaskFrameWorkerScope() { taskFrameWorkerActive = previous; }
private:
    bool previous;
};

enum class AcquiredFramePhase
{
    Recording,
    Discarded,
    RecoverySubmitted,
    Recovered,
    Submitted
};

// 只记录 acquire 后的单次处置；GPU 操作由 backend 执行后才推进对应状态。
class AcquiredFrameDisposition
{
public:
    AcquiredFramePhase Phase() const noexcept { return phase; }
    void Discarded()
    {
        Require(AcquiredFramePhase::Recording);
        phase = AcquiredFramePhase::Discarded;
    }
    void RecoverySubmitted()
    {
        Require(AcquiredFramePhase::Discarded);
        phase = AcquiredFramePhase::RecoverySubmitted;
    }
    void Recovered()
    {
        Require(AcquiredFramePhase::RecoverySubmitted);
        phase = AcquiredFramePhase::Recovered;
    }
    void Submitted()
    {
        Require(AcquiredFramePhase::Recording);
        phase = AcquiredFramePhase::Submitted;
    }

private:
    void Require(AcquiredFramePhase expected)
    {
        if (phase != expected)
            throw std::logic_error("Acquired frame already disposed or invalid recovery order");
    }
    AcquiredFramePhase phase = AcquiredFramePhase::Recording;
};

struct FrameAbortResult
{
    uint64_t recoveryEpoch = 0;
    uint32_t frameIndex = 0;
    uint32_t imageIndex = 0;
    bool swapchainRecreateRequired = false;
};

template<class Record, class ResetFence, class Submit, class Present>
auto CompleteDiscardedFrame(AcquiredFrameDisposition& disposition,
    Record&& record, ResetFence&& resetFence, Submit&& submit, Present&& present)
{
    if (disposition.Phase() != AcquiredFramePhase::Discarded)
        throw std::logic_error("Recovery requires discarded scene commands");
    record();
    resetFence();
    submit();
    disposition.RecoverySubmitted();
    auto result = present();
    disposition.Recovered();
    return result;
}

// 只在空恢复提交及 present 返回后抛出；真实 Vulkan 异常不包装成此类型。
class TaskFrameAborted final : public std::exception
{
public:
    TaskFrameAborted(FrameAbortResult result, std::string faultId)
        : result(result), faultId(std::move(faultId))
    {
        if (!result.recoveryEpoch || this->faultId.empty())
            throw std::invalid_argument("Recovered test frame requires epoch and explicit fault id");
    }
    const char* what() const noexcept override { return "Explicit test frame abandoned and synchronization recovered"; }
    FrameAbortResult result;
    std::string faultId;
};

class DescriptorFrameSkipped final : public std::exception
{
public:
    explicit DescriptorFrameSkipped(FrameAbortResult result) : result(result)
    {
        if (!result.recoveryEpoch)
            throw std::invalid_argument("Descriptor retry requires recovered acquire synchronization");
    }
    const char* what() const noexcept override { return "Retryable descriptor frame skipped after acquire recovery"; }
    FrameAbortResult result;
};
}
