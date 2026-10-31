#include "animationScriptData.h"

#include "animationMontage.h"
#include "skeletalPlayback.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace VL::Animation
{
namespace
{
// section 链接时间落在目标窗口边界的容差（秒），与导入器/播放范围使用同一量级。
constexpr float SectionLinkTolerance = 1e-3f;

std::string MontageSectionName(const Montage& montage)
{
    return montage.sections.empty() ? std::string() : montage.sections.front().name;
}

// 执行层拒绝矩阵：数据层允许保留、但执行层无法正确播放的组合在装配期显式失败，
// 不静默降级为“能播就播”。诊断固定带 montage id 与 section，便于 C#/日志定位资产。
// 装配期校验必须覆盖 Tick 会用到的全部前置条件——播放期间不抛异常。
void ValidateMontageExecution(const Montage& montage)
{
    if (montage.sections.empty())
    {
        throw std::invalid_argument("Animation montage '" + montage.id +
            "' has no sections");
    }

    // ---- section 链：时间区间、link 方法、nextSection 引用与落点 ----
    // 链接语义见 ResolveMontageSectionLinkTime（UE FAnimLinkableElement 的
    // Absolute/Relative/Proportional 换算，AnimLinkableElement.cpp:148-194）。
    for (const MontageSection& section : montage.sections)
    {
        const std::string label = "Animation montage '" + montage.id + "' section '" +
            section.name + "'";
        if (!std::isfinite(section.startTime) || !std::isfinite(section.endTime) ||
            section.startTime < 0 || !(section.endTime > section.startTime))
        {
            throw std::invalid_argument(label + " has an invalid time range");
        }
        if (!section.linkMethod.empty() && section.linkMethod != "Absolute" &&
            section.linkMethod != "Relative" && section.linkMethod != "Ratio")
        {
            throw std::invalid_argument(label + " uses unknown link method '" +
                section.linkMethod +
                "'; supported link methods are Absolute/Relative/Ratio");
        }
        if (section.nextSection.empty()) continue;

        const MontageSection* next = nullptr;
        for (const MontageSection& candidate : montage.sections)
        {
            if (candidate.name == section.nextSection) next = &candidate;
        }
        if (next == nullptr)
        {
            throw std::invalid_argument(label + " references unknown next section '" +
                section.nextSection + "'");
        }
        const float linkTime = ResolveMontageSectionLinkTime(section);
        if (!std::isfinite(linkTime))
        {
            throw std::invalid_argument(label +
                " resolves a non-finite next section time");
        }
        // 落点必须在目标 section 的 [start,end) 内：Tick 用该值作为下一段时钟起点，
        // 落在窗口外会让播放范围与时钟不一致（UE 5.8 runtime 直接跳到下一段起点，
        // 不使用 NextTime；这里执行的是数据层保留的 NextLink 语义）。
        if (linkTime < next->startTime - SectionLinkTolerance ||
            linkTime > next->endTime - SectionLinkTolerance)
        {
            throw std::invalid_argument(label + " links to time " +
                std::to_string(linkTime) + ", outside next section '" + next->name +
                "' [" + std::to_string(next->startTime) + ", " +
                std::to_string(next->endTime) + "]");
        }
    }

    // ---- warp notify 的执行矩阵 ----
    const std::string section = MontageSectionName(montage);
    for (const MontageNotify& notify : montage.notifies)
    {
        if (!notify.warp) continue;
        const MontageWarp& warp = *notify.warp;
        if (warp.provider != "None" && warp.provider != "Bone" && warp.provider != "Static")
        {
            throw std::invalid_argument("Animation montage '" + montage.id + "' section '" +
                section + "' warp target '" + warp.target + "' uses provider '" +
                warp.provider + "', which is not supported");
        }
        // UE WarpPointAnimProvider=Static 消费 Montage 资产里的静态 warppoint 变换；
        // 缺 staticPose 或退化四元数无法求 offset，装配期拒绝。
        if (warp.provider == "Static")
        {
            if (!warp.staticPose)
            {
                throw std::invalid_argument("Animation montage '" + montage.id + "' section '" +
                    section + "' warp target '" + warp.target +
                    "' uses the Static provider without a staticPose");
            }
            const MontageStaticPose& pose = *warp.staticPose;
            for (const float value : pose.location)
            {
                if (!std::isfinite(value))
                    throw std::invalid_argument("Animation montage '" + montage.id +
                        "' section '" + section + "' warp target '" + warp.target +
                        "' has a non-finite staticPose location");
            }
            const float rotationNorm = pose.rotation[0] * pose.rotation[0] +
                pose.rotation[1] * pose.rotation[1] + pose.rotation[2] * pose.rotation[2] +
                pose.rotation[3] * pose.rotation[3];
            if (!std::isfinite(rotationNorm) || !(rotationNorm > 1e-8f))
                throw std::invalid_argument("Animation montage '" + montage.id +
                    "' section '" + section + "' warp target '" + warp.target +
                    "' has a degenerate staticPose rotation");
        }
        if (warp.easing != "Linear")
        {
            throw std::invalid_argument("Animation montage '" + montage.id + "' section '" +
                section + "' warp target '" + warp.target + "' uses easing '" + warp.easing +
                "'; only Linear is supported");
        }
        // UE 默认 RotationMethod=Slerp（最短路径球面插值）；其它方法需要
        // RotationType/WarpMaxRotationRate 等尚未进入 VL 数据的参数，执行层不能
        // 静默降级。只在 bWarpRotation 打开时检查；关闭时 rotationMethod 不参与执行。
        if (warp.warpRotation && warp.rotationMethod != "Slerp")
        {
            throw std::invalid_argument("Animation montage '" + montage.id + "' section '" +
                section + "' warp target '" + warp.target + "' uses rotation method '" +
                warp.rotationMethod + "'; only Slerp is supported");
        }
    }
}
}

nlohmann::json BuildAnimationScriptAssets(const SkeletalAsset& asset)
{
    auto result = nlohmann::json::object();

    // curves 只有“clip 有 sidecar 且至少一条曲线”时才出现；没有曲线的 clip 不发布空数组，
    // 避免 22 个 clip 的 catalog 中塞入无消费价值的结构。
    auto curves = nlohmann::json::object();
    const auto& animation = asset.AnimationAsset();
    for (const Clip& clip : animation.clips)
    {
        if (clip.sequence.curves.empty()) continue;
        auto list = nlohmann::json::array();
        for (const FloatCurve& curve : clip.sequence.curves)
            list.push_back({{"name", curve.name}, {"times", curve.times}, {"values", curve.values}});
        curves[clip.id] = std::move(list);
    }
    result["curves"] = std::move(curves);

    auto montages = nlohmann::json::array();
    for (const Montage& montage : animation.montages)
    {
        auto sections = nlohmann::json::array();
        for (const MontageSection& section : montage.sections)
            sections.push_back({{"name", section.name}, {"startTime", section.startTime},
                {"endTime", section.endTime}, {"nextSection", section.nextSection},
                {"nextTime", section.nextTime}, {"linkMethod", section.linkMethod}});
        auto notifies = nlohmann::json::array();
        for (const MontageNotify& notify : montage.notifies)
        {
            nlohmann::json value{{"kind", notify.kind}, {"track", notify.track},
                {"name", notify.name}, {"time", notify.time}, {"duration", notify.duration}};
            if (notify.warp)
            {
                const MontageWarp& warp = *notify.warp;
                // 只发布 C# 反馈/诊断需要的字段；staticPose 只由原生执行层消费
                // （BuildMontageWarpWindows → MotionWarpWindow），不进入 catalog，
                // C# 侧无需新增字段。
                value["warp"] = {{"target", warp.target}, {"provider", warp.provider},
                    {"bone", warp.bone},
                    {"subtractRemainingRootMotion", warp.subtractRemainingRootMotion},
                    {"ignoreZAxis", warp.ignoreZAxis}, {"warpRotation", warp.warpRotation},
                    {"rotationTimeMultiplier", warp.rotationTimeMultiplier},
                    {"rotationMethod", warp.rotationMethod},
                    {"easing", warp.easing}};
            }
            notifies.push_back(std::move(value));
        }
        montages.push_back({{"id", montage.id}, {"sequencePath", montage.sequencePath},
            {"derivedClipId", montage.derivedClipId}, {"length", montage.length},
            {"slot", montage.slot}, {"sections", std::move(sections)},
            {"notifies", std::move(notifies)}});
    }
    result["montages"] = std::move(montages);
    return result;
}

nlohmann::json BuildAnimationNotifyFeedback(const std::vector<FiredNotify>& notifies)
{
    auto result = nlohmann::json::array();
    for (const FiredNotify& notify : notifies)
        result.push_back({{"name", notify.name}, {"track", notify.track},
            {"kind", notify.kind}, {"phase", notify.phase}, {"time", notify.time},
            {"startTime", notify.startTime}, {"duration", notify.duration},
            {"interrupted", notify.interrupted}});
    return result;
}

std::vector<MotionWarpWindow> BuildMontageWarpWindows(
    const Montage& montage, const std::map<std::string, MotionWarpTarget>& targets)
{
    ValidateMontageExecution(montage);

    // 先收集 distinct target 名并做集合相等校验，再构造窗口；缺/多都拒绝，
    // 避免 C# 传错键时静默使用默认位置。
    std::set<std::string> required;
    for (const MontageNotify& notify : montage.notifies)
        if (notify.warp) required.insert(notify.warp->target);
    for (const std::string& name : required)
        if (targets.find(name) == targets.end())
            throw std::invalid_argument("Animation montage '" + montage.id +
                "' requires warp target '" + name + "'");
    for (const auto& entry : targets)
        if (required.find(entry.first) == required.end())
            throw std::invalid_argument("Animation montage '" + montage.id +
                "' does not declare warp target '" + entry.first + "'");

    std::vector<MotionWarpWindow> windows;
    windows.reserve(required.size());
    for (const MontageNotify& notify : montage.notifies)
    {
        if (!notify.warp) continue;
        const MontageWarp& warp = *notify.warp;
        const MotionWarpTarget& target = targets.at(warp.target);
        MotionWarpWindow window;
        window.targetName = warp.target;
        window.targetLocation = target.location;
        window.startTime = notify.time;
        window.endTime = notify.time + notify.duration;
        window.subtractRemainingRootMotion = warp.subtractRemainingRootMotion;
        window.ignoreVertical = warp.ignoreZAxis;
        // UE WarpPointAnimProvider：Bone 消费骨名；Static 消费 Montage 资产里的
        // staticPose；None 两者都不用。Static 在动画/组件空间求值（见 MotionWarpWindow
        // 与 MotionWarpRuntime::Apply），不经过 catalog/C#。
        if (warp.provider == "Bone")
        {
            window.warpPointBone = warp.bone;
        }
        else if (warp.provider == "Static" && warp.staticPose)
        {
            const MontageStaticPose& pose = *warp.staticPose;
            window.hasStaticWarpPoint = true;
            window.staticWarpPointLocation = Eigen::Vector3f(
                pose.location[0], pose.location[1], pose.location[2]);
            // 数据层是 UE 四元数 (x,y,z,w)，Eigen 构造函数是 (w,x,y,z)；Validate 已校验
            // 范数可归一。
            window.staticWarpPointRotation = Eigen::Quaternionf(pose.rotation[3],
                pose.rotation[0], pose.rotation[1], pose.rotation[2]).normalized();
        }
        // 旋转字段逐窗口复制；目标旋转由 target 提交，缺省（hasRotation=false）时
        // 即使 warpRotation=true 也不改 yaw，等价保留旧的纯位置调用行为。
        window.warpRotation = warp.warpRotation;
        window.rotationTimeMultiplier = warp.rotationTimeMultiplier;
        window.rotationMethod = warp.rotationMethod;
        window.hasTargetRotation = target.hasRotation;
        window.targetRotation = target.rotation;
        windows.push_back(std::move(window));
    }
    return windows;
}

void PlayMontage(SkeletalPlayback& player, const Montage& montage,
    const std::map<std::string, MotionWarpTarget>& targets, bool loop, float rate,
    bool inPlace, float startTime)
{
    if (loop)
        throw std::invalid_argument("Animation montage '" + montage.id +
            "' does not support looping playback");
    if (!std::isfinite(rate) || rate <= 0 || rate > 4)
        throw std::invalid_argument("Animation montage '" + montage.id +
            "': rate must be in (0,4]");
    // 先完成所有校验与候选构造；任何拒绝都保持当前播放器（worldAnimation 也使用 candidate 模式）。
    std::vector<MotionWarpWindow> windows = BuildMontageWarpWindows(montage, targets);
    if (montage.derivedClipId.empty())
        throw std::invalid_argument("Animation montage '" + montage.id +
            "' has no derived clip id; the Montage must be bound to a loaded sequence sidecar");
    const auto& clips = player.Asset().AnimationAsset().clips;
    const auto clip = std::find_if(clips.begin(), clips.end(),
        [&](const Clip& value) { return value.id == montage.derivedClipId; });
    if (clip == clips.end())
        throw std::invalid_argument("Animation montage '" + montage.id +
            "' requires the loaded clip '" + montage.derivedClipId + "'");
    // Montage 的播放段必须由 sidecar 派生；没有 sidecar 的 clip 不能承载 Montage notify/曲线。
    if (clip->sequence.id != montage.derivedClipId)
        throw std::invalid_argument("Animation montage '" + montage.id +
            "' requires a sequence sidecar for clip '" + montage.derivedClipId + "'");

    // section 链在 Tick 中切换播放范围；装配期先证明每一段都落在已加载片段内，
    // 否则轨道内部会在播放中途被迫拒绝（Tick 不抛异常）。
    const bool chained = montage.sections.size() > 1 ||
        !montage.sections.front().nextSection.empty();
    if (chained)
    {
        const float clipDuration = clip->runtime->duration();
        for (const MontageSection& section : montage.sections)
        {
            if (section.endTime > clipDuration + 1e-3f)
            {
                throw std::invalid_argument("Animation montage '" + montage.id +
                    "' section '" + section.name + "' ends outside the loaded clip '" +
                    montage.derivedClipId + "'");
            }
        }
    }

    // ---- 入口起始时间 ----
    // startTime 缺省 0 表示“从 section 0 起点播放”，与引入入口时间之前逐位一致：
    // section 0 从 0.2 开始时，旧的 PlayMontage 调用（startTime=0）仍从 0.2 进入。
    // 因此 0 是“未指定”哨兵；显式正值才必须落在 section 0 的 [start,end) 内，并给
    // 一次性播放留出至少 1 ms，否则装配出的范围会立刻完成。
    const MontageSection& firstSection = montage.sections.front();
    constexpr float MinEntryPlayback = 1e-3f;
    const float entryEnd = std::min(firstSection.endTime, clip->runtime->duration());
    float entryTime = firstSection.startTime;
    if (startTime > 0)
    {
        if (startTime < firstSection.startTime ||
            startTime > entryEnd - MinEntryPlayback)
        {
            throw std::invalid_argument("Animation montage '" + montage.id +
                "' startTime " + std::to_string(startTime) + " is outside section '" +
                firstSection.name + "' playable range [" +
                std::to_string(firstSection.startTime) + ", " +
                std::to_string(entryEnd) + "); at least " +
                std::to_string(MinEntryPlayback) + " s must remain");
        }
        entryTime = startTime;
    }
    else if (startTime < 0 || !std::isfinite(startTime))
    {
        throw std::invalid_argument("Animation montage '" + montage.id +
            "' startTime " + std::to_string(startTime) +
            " must be finite and nonnegative");
    }

    SkeletalPlayback candidate(player);
    // Play→Select 会清理旧播放范围与旧 Montage 覆盖层；监听源必须在 Play 之后重新设置。
    candidate.Play(montage.derivedClipId, false, rate, inPlace);
    // 入口时间只截短第一段的播放范围（取 max 兼容 section 0 不从 0 开始的老资产）；
    // 之后仍由 section 链推进。单段无链接保持原来的单段路径。
    if (chained) candidate.SetMontageSections(montage.sections, entryTime);
    else candidate.SetPlaybackRange(entryTime, firstSection.endTime);
    candidate.SetMontageNotifies(montage.notifies, montage.id, montage.length);
    // 视作一次初始 seek：清空活动 notifyState、不补发 entryTime 之前的历史事件；
    // 第一个 Tick 只扫描 [entryTime, entryTime+dt)，根位移也从 entryTime 起积分。
    candidate.Seek(entryTime);
    candidate.SetMotionWarpWindows(std::move(windows));
    player = std::move(candidate);
}
}
