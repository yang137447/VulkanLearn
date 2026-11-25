#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <iostream>
#include <tuple>
#include <vector>

#include <Eigen/Geometry>

#include "hair/tressfxSimulationParams.h"
#include "render/backend/rendererBackendVulkan.h"

namespace VL::Hair
{
// UBO/着色器数组容量，不是策略上限：RodGpuParams::roots / capsules 和
// shader 的 mat4 roots[9] / capsules[9][8] 都按它定长。改它必须同时改
// shader 布局并重新验证 sizeof/offsetof 静态断言，因此策略不要直接用它。
inline constexpr uint32_t RodGpuMaxSteps = 8;
inline constexpr uint32_t RodGpuSweeps = 16;
// 单帧可退休仿真时间的不变式（retirement ceiling）：每帧最多把仿真推进这么多。
// 它由「步长 × 每帧策略子步上限」共同满足，默认 1/120 s × 4 = 33.3 ms，
// 与旧的 1/240 s × 8 完全相同。单独改其中一项会破坏这个上限，因此默认值
// 由下面的 static_assert 在编译期锁死；VL_ROD_* 覆盖只用于诊断 A/B。
inline constexpr double RodGpuRetirementCeilingSeconds = 1.0 / 30.0;
inline constexpr double RodGpuDefaultStepHz = 120.0;
inline constexpr uint32_t RodGpuDefaultSubstepCap = 4;
static_assert(RodGpuDefaultSubstepCap / RodGpuDefaultStepHz ==
    RodGpuRetirementCeilingSeconds, "default step x substep cap must keep the 33.3 ms ceiling");
// 子步长。默认 1/120 s；诊断开关 VL_ROD_STEP_HZ 可覆盖（例如 240 复原旧步长）。
// 做成函数而不是 constexpr，是为了让环境变量在首次调用时解析，并避免跨 TU
// 的静态初始化顺序问题（所有调用点都在函数体内）。
inline double RodGpuStepSeconds()
{
    static const double seconds = [] {
        const char* value = std::getenv("VL_ROD_STEP_HZ");
        const double hertz = value != nullptr ? std::strtod(value, nullptr) : RodGpuDefaultStepHz;
        return 1.0 / (hertz > 0.0 ? hertz : RodGpuDefaultStepHz);
    }();
    return seconds;
}
// 每帧策略子步上限。与 RodGpuMaxSteps（UBO 容量）分开：默认 4 × 1/120 s
// = 33.3 ms，保持退休上限不变。VL_ROD_SUBSTEP_CAP 可覆盖（配合
// VL_ROD_STEP_HZ=240 VL_ROD_SUBSTEP_CAP=8 可完整复原旧的 8 × 1/240 行为）；
// 上限被夹在 [1, RodGpuMaxSteps]，否则会越界写 roots/capsules 与 shader 数组。
inline uint32_t RodGpuSubstepCap()
{
    static const uint32_t cap = [] {
        const char* value = std::getenv("VL_ROD_SUBSTEP_CAP");
        long requested = static_cast<long>(RodGpuDefaultSubstepCap);
        if (value != nullptr && value[0] != '\0')
        {
            // 只在整串都是合法整数时采用；拼写错误不能让上限静默掉到 1。
            char* end = nullptr;
            const long parsed = std::strtol(value, &end, 10);
            if (end != value && *end == '\0')
                requested = parsed;
        }
        if (requested < 1)
            return 1u;
        return requested >= static_cast<long>(RodGpuMaxSteps) ?
            RodGpuMaxSteps : static_cast<uint32_t>(requested);
    }();
    return cap;
}
// V3 带接触的“硬帧”判据：上一批读回里某个 guide 用了超过这么多轮牛顿，就说明
// 它还在首个瞬态里。此时单帧只派发 1 个 substep，把瞬态摊到多帧——2000 guide
// 实测「满子步上限 × 每 guide 数十轮」会让单次 dispatch 超过 OS 的提交上限
// （vk::DeviceLostError）。稳态（0–8 轮）仍然派发到每帧策略子步上限。
inline constexpr uint32_t RodGpuV3HardFrameIterations = 12;
inline constexpr uint32_t RodGpuV3HardFrameSteps = 1;
// 对象级「硬帧」判据的稳健化（默认路径）：统计上一批读回里牛顿轮数超过
// RodGpuV3HardFrameIterations 的 guide 数，只有超过 guide 总数的 1%（至少 1 个）
// 才降为单 substep。旧的 max-over-guides 判据会让一个永久不收敛的 guide 把其余
// 上千 guide 一起钉在 1 substep——bind pose 实测因此长期只有 1.04 substep/帧。
inline constexpr uint32_t RodGpuV3HardFrameGuidePermille = 10;
inline constexpr uint32_t RodGpuV3HardFrameGuideFloor = 1;
// 时间债上限：超过部分主动丢弃（见 RecordRod 的 debt ceiling）。每帧最多退休
// RodGpuRetirementCeilingSeconds = 33.3 ms（默认 1/120 s × 4），而每帧最多加入
// 100 ms，所以没有上限时 debt 只能单调增长，唯一泄压阀是 Reset（一次可见的头发跳变）。
// 0.5 s 的含义：允许半秒的追赶余量，超出的仿真时间直接放弃，宁可头发比角色慢。
inline constexpr double RodGpuMaxDebtSeconds = 0.5;
inline constexpr uint32_t RodGpuGuideVec4Count = 32 * 5 + 31 * 8 + 4;
inline constexpr uint32_t RodGpuReferenceVec4Count = 96;

// 只读一次环境变量：VL_ROD_GATE_MODE=legacy 恢复旧的对象级 max 判据，仅用于 A/B
// 对比；默认走上面的 guide 计数判据。进程内字符串常量只解析一次，热路径零成本。
inline bool RodGpuLegacyGateMode()
{
    // 默认改用「超阈值 guide 计数」判据（非 legacy）。
    // 实测依据：启用真实 SDF 接触后，2000 guides 里仅 1–5 个未收敛（0.05–0.25%），
    // 而 legacy 判据取 guide 最大值，把整个对象钉在 1 个子步：每帧只推进 4.2 ms
    // 仿真时间，而帧长 43 ms，头发以约 10% 实速运动，看起来就是「不跟头皮走」。
    // 切到计数判据后同一场景跑满 4 个子步：仿真推进 4.2 → 16.7 ms/帧（约 39% 实速），
    // 每毫秒仿真成本 10.74 → 3.47 ms，代价是帧时间 44.7 → 57.9 ms。
    // 业主判据是「允许少量穿插，整体动态对即可」，因此这个交换是对的；
    // 头发跟不上动画就是「动态不对」，比帧率更不可接受。
    // 回到旧判据：VL_ROD_GATE_MODE=legacy。字符串常量只解析一次，热路径零成本。
    static const bool legacy = [] {
        const char* mode = std::getenv("VL_ROD_GATE_MODE");
        return mode != nullptr && mode[0] == 'l' && mode[1] == 'e';
    }();
    return legacy;
}



struct alignas(16) RodGpuParams
{
    Eigen::Matrix4f hairToWorld = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f worldToHair = Eigen::Matrix4f::Identity();
    std::array<Eigen::Matrix4f, RodGpuMaxSteps + 1> roots{};
    Eigen::Vector4f gravityStep = Eigen::Vector4f::Zero();
    Eigen::Vector4f material = Eigen::Vector4f(0.0005f, 1300.0f, 1.0e6f, 0.4e6f);
    Eigen::Vector4f support = Eigen::Vector4f(0.01f, 0.5f, 9.8f, 0.0f);
    Eigen::Vector4f timing = Eigen::Vector4f::Zero();
    std::array<uint32_t, 4> counts{};
    std::array<uint32_t, 4> flags{};
    std::array<std::array<GuideColliderGpu, 8>, RodGpuMaxSteps + 1> capsules{};
    Eigen::Vector4f windDrag = Eigen::Vector4f::Zero();
    Eigen::Vector4f probeAcceleration = Eigen::Vector4f::Zero();
};
static_assert(sizeof(RodGpuParams) == 3136);
static_assert(offsetof(RodGpuParams, gravityStep) == 704);
static_assert(offsetof(RodGpuParams, counts) == 768);
static_assert(offsetof(RodGpuParams, capsules) == 800);

struct RodGpuBuffer
{
    RendererBackendVulkan* backend = nullptr;
    vk::Buffer buffer = nullptr;
    vk::DeviceMemory memory = nullptr;
    vk::DeviceSize size = 0;
    void* mapped = nullptr;

    ~RodGpuBuffer()
    {
        if (backend && buffer)
        {
            if (mapped)
                backend->UnmapMemory(memory);
            backend->DestroyBuffer(buffer, memory);
        }
    }

    void Create(RendererBackendVulkan& renderer, vk::DeviceSize bytes,
        bool host, const std::string& name)
    {
        backend = &renderer;
        size = bytes;
        std::tie(buffer, memory) = renderer.CreateBuffer(bytes,
            vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eUniformBuffer |
                vk::BufferUsageFlagBits::eTransferSrc |
                vk::BufferUsageFlagBits::eTransferDst,
            host ? vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent
                 : vk::MemoryPropertyFlagBits::eDeviceLocal, name);
        if (host)
            mapped = renderer.MapMemory(memory, bytes);
    }

    vk::DescriptorBufferInfo Info() const
    {
        return vk::DescriptorBufferInfo(buffer, 0, size);
    }
};

struct RodGpuPose
{
    Eigen::Matrix4f root = Eigen::Matrix4f::Identity();
    std::vector<Eigen::Matrix4f> palette;
};

struct RodGpuPoseInterval
{
    double begin = 0.0;
    double end = 0.0;
    RodGpuPose first;
    RodGpuPose last;
};

inline Eigen::Matrix4f RodGpuInterpolateRigid(
    const Eigen::Matrix4f& first, const Eigen::Matrix4f& last, float fraction)
{
    Eigen::Quaternionf initial(first.block<3, 3>(0, 0));
    Eigen::Quaternionf final(last.block<3, 3>(0, 0));
    Eigen::Matrix4f result = Eigen::Matrix4f::Identity();
    result.block<3, 3>(0, 0) =
        initial.normalized().slerp(fraction, final.normalized()).toRotationMatrix();
    result.block<3, 1>(0, 3) = first.block<3, 1>(0, 3) * (1.0f - fraction) +
        last.block<3, 1>(0, 3) * fraction;
    return result;
}

inline bool RodGpuIsRigid(const Eigen::Matrix4f& matrix)
{
    const Eigen::Matrix3f rotation = matrix.block<3, 3>(0, 0);
    return matrix.allFinite() &&
        (rotation.transpose() * rotation - Eigen::Matrix3f::Identity()).norm() < 1.0e-3f &&
        std::abs(rotation.determinant() - 1.0f) < 1.0e-3f;
}

struct RodGpuObject
{
    std::weak_ptr<RenderableObject> owner;
    RodGpuBuffer state;
    RodGpuBuffer sdfHistory;
    RodGpuBuffer references;
    RodGpuBuffer frameDeltas;
    RodGpuBuffer lowParts;
    RodGpuBuffer failureMatrix;
    RodGpuBuffer failureCandidate;
    uint32_t captureFailureSlot = UINT32_MAX;
    bool failureEvidenceWritten = false;
    // SDF 历史缓冲在设备本地，创建时没有零初始化；只有 rodStoreSdf 真正写入过
    // 才能被下一帧的 surface-velocity 采样读取。这个标志把“可读”变成显式状态，
    // 防止首帧或 SDF 失效帧读到未初始化内容算出的越界索引。
    bool sdfHistoryValid = false;
    std::array<RodGpuBuffer, GuideSimulationParamBufferCount> params;
    std::array<RodGpuBuffer, GuideSimulationParamBufferCount> samples;
    std::array<RodGpuBuffer, GuideSimulationParamBufferCount> implicitParams;
    std::array<vk::DescriptorSet, GuideSimulationParamBufferCount> solveSets{};
    // V3 + SDF 接触用的独立求解集：比 solveSets 多 SdfGridParams/SdfGrid/SdfHistory。
    std::array<vk::DescriptorSet, GuideSimulationParamBufferCount> solveSdfSets{};
    std::array<vk::DescriptorSet, GuideSimulationParamBufferCount> followSets{};
    std::array<vk::DescriptorSet, GuideSimulationParamBufferCount> sdfHistorySets{};
    std::array<RodGpuBuffer, GuideSimulationParamBufferCount> readback;
    std::array<bool, GuideSimulationParamBufferCount> readbackPending{};
    std::array<uint64_t, GuideSimulationParamBufferCount> readbackFrame{};
    std::array<uint64_t, GuideSimulationParamBufferCount> readbackGeneration{};
    // 与 readbackFrame 同槽记录该帧实际派发的 substep 数，供诊断 CSV 精确对齐
    // 「这一次读回属于多少个子步」——不能事后读当前 steps，那已经是更晚的帧了。
    std::array<uint32_t, GuideSimulationParamBufferCount> readbackSteps{};
    vk::DescriptorPool pool = nullptr;
    RendererBackendVulkan* backend = nullptr;
    vk::Buffer current = nullptr;
    uint32_t paletteCapacity = 0;
    uint32_t guides = 0;
    uint32_t algorithmVersion = 1;
    uint32_t profileMathVersion = 1;
    uint32_t kineticMathVersion = 1;
    uint32_t numericEncodingVersion = 1;
    Eigen::Vector3f fixedWorldOrigin = Eigen::Vector3f::Zero();
    uint64_t world = 0;
    uint64_t lastFrame = UINT64_MAX;
    uint64_t simulationGeneration = 0;
    uint32_t lastResetReason = 0;
    double receivedSeconds = 0.0;
    double dispatchedSeconds = 0.0;
    double debtSeconds = 0.0;
    // 累计被 debt ceiling 主动丢弃的仿真秒数。它只增不减（ClearTimeline 归零），
    // 用来区分「头发比角色慢」是本帧负载造成的，还是丢弃策略造成的。
    double droppedSeconds = 0.0;
    double acceptedSeconds = 0.0;
    uint32_t gpuStatus = 0;
    // 上一批读回里单 guide 的最大牛顿轮数（UINT32_MAX = 还没有读回，按“硬帧”处理，
    // 因此 reset 后的头两帧一定走保守的 1 substep）。见 RodGpuV3HardFrameIterations。
    uint32_t maxIterationsLastFence = UINT32_MAX;
    // 上一批读回里「牛顿轮数超过 RodGpuV3HardFrameIterations」的 guide 数
    // （UINT32_MAX 同样是“还没有读回”的保守哨兵）。默认门判据是它，旧判据是
    // maxIterationsLastFence；两者都在 ClearTimeline 里重置。
    uint32_t hardGuidesLastFence = UINT32_MAX;
    RodGpuPose previousPose;
    RodGpuPose referencePose;
    bool poseInitialized = false;
    bool initialized = false;
    bool reset = true;
    bool reported = false;
    std::deque<RodGpuPoseInterval> intervals;

    ~RodGpuObject()
    {
        if (backend && pool)
        {
            std::cout << "[RodGPU] destroy descriptor pool=0x" << std::hex <<
                reinterpret_cast<uintptr_t>(static_cast<VkDescriptorPool>(pool)) <<
                std::dec << " world=" << world << std::endl;
            backend->DestroyDescriptorPool(pool);
        }
    }

    void ClearTimeline(const RodGpuPose& pose)
    {
        intervals.clear();
        previousPose = pose;
        referencePose = pose;
        poseInitialized = true;
        receivedSeconds = 0.0;
        dispatchedSeconds = 0.0;
        debtSeconds = 0.0;
        droppedSeconds = 0.0;
        acceptedSeconds = 0.0;
        gpuStatus = 0;
        ++simulationGeneration;
        reset = true;
        sdfHistoryValid = false;
        // reset 之后重新进入瞬态：把硬帧判据恢复成保守值（未知）。
        maxIterationsLastFence = UINT32_MAX;
        hardGuidesLastFence = UINT32_MAX;
        readbackSteps.fill(0);
        // 失败证据按 reset epoch 重新采集：否则每个对象生命周期内只能拿到一次，
        // 而 reset 之后的状态是新起点，值得再抓一份。
        failureEvidenceWritten = false;
    }

    RodGpuPose Sample(double time) const
    {
        if (intervals.empty())
            return previousPose;
        const RodGpuPoseInterval* interval = &intervals.back();
        for (const RodGpuPoseInterval& candidate : intervals)
            if (time <= candidate.end + 1.0e-10)
            {
                interval = &candidate;
                break;
            }
        const float fraction = static_cast<float>(std::clamp(
            (time - interval->begin) / (interval->end - interval->begin), 0.0, 1.0));
        RodGpuPose pose;
        pose.root = RodGpuInterpolateRigid(interval->first.root, interval->last.root, fraction);
        pose.palette.resize(interval->last.palette.size());
        for (size_t joint = 0; joint < pose.palette.size(); ++joint)
            pose.palette[joint] = RodGpuInterpolateRigid(
                interval->first.palette[joint], interval->last.palette[joint], fraction);
        return pose;
    }
};
}
