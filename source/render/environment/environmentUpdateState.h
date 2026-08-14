#pragma once

#include <cstdint>

#include "world/worldSnapshot.h"

namespace VL
{

// 环境派生资源只跟踪源数据，不把相机状态混入 dirty 判定。
// generation token 用于防止旧任务完成时误清除已经到达的新请求。
class EnvironmentUpdateState
{
public:
    // 每个渲染帧观察一次最新 EnvironmentSnapshot，并只比较会改变环境派生资源的源数据：
    //
    // 输入变化                                      是否 Dirty
    // environment type                             是
    // ProceduralSky 的任一 skyParameters 分量       是
    // HDRI cube 的逻辑 key 或 generation             是
    // environmentIntensity                         否，消费端每帧直接读取
    // prefilteredCube / brdfLut                     否，它们不是本更新链的输入源
    // camera                                        否，不属于 EnvironmentSnapshot
    //
    // observedSnapshot 表示“上一帧观察到的源状态”，不是当前 active 或 pending 代际。
    // dirty 一旦置位会持续保持，直到最新 requestedGeneration 完成；观察到相同快照不会清除它。
    void Observe(const EnvironmentSnapshot& snapshot)
    {
        const bool environmentSourceChanged =
            !hasObservedSnapshot || !AreEqual(observedSnapshot, snapshot);

        // 即使只改变消费端强度，也保留完整最新快照供诊断和后续请求使用。
        observedSnapshot = snapshot;
        hasObservedSnapshot = true;

        if (environmentSourceChanged)
        {
            requestedGeneration++;
            dirty = true;
        }
    }

    void Reset() noexcept
    {
        observedSnapshot = EnvironmentSnapshot();
        requestedGeneration = 0;
        dirty = true;
        hasObservedSnapshot = false;
    }

    bool IsDirty() const
    {
        return dirty;
    }

    // 返回当前最新请求的 generation token，供 Scheduler 冻结快照并启动或替换 pending。
    uint64_t BeginUpdate() const
    {
        return requestedGeneration;
    }

    // 只有最新 generation 才能清除 dirty。若旧 pending 录制期间又观察到新源状态，
    // requestedGeneration 已经推进，旧 generation 完成时必须保留 dirty，等待新请求继续执行。
    void CompleteUpdate(uint64_t generation)
    {
        if (generation == requestedGeneration)
        {
            dirty = false;
        }
    }

    const EnvironmentSnapshot& GetObservedSnapshot() const
    {
        return observedSnapshot;
    }

private:
    static bool AreEqual(const Eigen::Vector4f& left, const Eigen::Vector4f& right)
    {
        // Phase 0 的快照来自场景数据或显式运行时命令，因此当前使用逐分量精确比较。
        // 后续 Time Of Day 若每帧生成连续浮点值，必须在 Observe 上游做节流或量化；
        // 否则每帧都会创建新 generation，并持续取消尚未完成的 pending 更新。
        return (left.array() == right.array()).all();
    }

    static bool AreEqual(const SkyParametersGPU& left, const SkyParametersGPU& right)
    {
        return AreEqual(left.sunDirectionIntensity, right.sunDirectionIntensity) &&
            AreEqual(left.sunColorAngularRadius, right.sunColorAngularRadius) &&
            AreEqual(left.zenithColor, right.zenithColor) &&
            AreEqual(left.horizonColor, right.horizonColor) &&
            AreEqual(left.groundColor, right.groundColor) &&
            AreEqual(left.scatteringControls, right.scatteringControls) &&
            AreEqual(left.cloudControls, right.cloudControls);
    }

    static bool AreEqual(const ResourceHandle& left, const ResourceHandle& right)
    {
        return left.key == right.key && left.generation == right.generation;
    }

    static bool AreEqual(
        const EnvironmentSnapshot& left,
        const EnvironmentSnapshot& right)
    {
        if (left.type != right.type)
        {
            return false;
        }

        // intensity 在 Sky/IBL 消费端统一相乘，不属于重建输入；prefilteredCube
        // 也是输出标识而非环境源，因此两者都不能触发派生资源重建。
        if (left.type == EnvironmentType::ProceduralSky)
        {
            return AreEqual(left.skyParameters, right.skyParameters);
        }

        return AreEqual(left.cube, right.cube);
    }

    // 快照由 RenderScene 按值冻结，并一直作为比较基线直到新请求替换它。
    EnvironmentSnapshot observedSnapshot;
    uint64_t requestedGeneration = 0;
    bool dirty = true;
    bool hasObservedSnapshot = false;
};

} // namespace VL
