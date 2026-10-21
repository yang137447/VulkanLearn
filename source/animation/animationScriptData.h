#pragma once

#include <map>
#include <string>
#include <vector>
#include <Eigen/Core>
#include <nlohmann/json.hpp>

#include "motionWarping.h"

namespace VL::Animation
{
struct SkeletalAsset;
class SkeletalPlayback;
struct Montage;
struct FiredNotify;

// C# 提交面（catalog / feedback / PlayMontage）需要的纯数据转换。
// 这些函数不读文件、不持有 World/播放时钟；worldAnimation 只做 modelPath 索引与 JSON 编排，
// 模块测试可以直接构造 SkeletalAsset/SkeletalPlayback 验证。
//
// catalog 的资产段：只发布带 Sequence sidecar 且确有曲线的 clip（控制一次性 payload 体积），
// montages 发布完整 sections/notifies（含 warp 元数据，但不含 staticPose：它只由原生
// 执行层消费，C# 不需要）。旧 clip id 列表仍由 World::ScriptAnimationCatalog 单独发布，
// 两者不互相替换。
nlohmann::json BuildAnimationScriptAssets(const SkeletalAsset& asset);

// feedback 的 notifies 数组：本 tick 触发的事件，作者原区间不裁剪。
nlohmann::json BuildAnimationNotifyFeedback(const std::vector<FiredNotify>& notifies);

// 从 Montage 的 MotionWarping notifyState 构造窗口（顺序 = notifies 数组顺序）。
// 执行层支持 None|Bone|Static provider（Static 消费资产 staticPose）、Linear easing、
// 旋转方法 Slerp（仅 warpRotation=true 时检查）、target 键集合完全匹配；不支持时抛
// std::invalid_argument，诊断包含 montage id 与 section/easing/provider/target/
// rotationMethod。section 链也在这里整体校验：空 section、非法 link 方法、未知
// nextSection、链接落点不在目标 section 内都拒绝。
// target 的 rotation 会复制进窗口，但只有窗口 warpRotation=true 时才由
// MotionWarpRuntime 消费；缺省表示该 target 没有提交朝向（旧的纯位置调用）。
std::vector<MotionWarpWindow> BuildMontageWarpWindows(
    const Montage& montage, const std::map<std::string, MotionWarpTarget>& targets);

// 在候选播放器上应用 Montage：Play(derivedClipId) → 从入口时间装配播放范围 →
// SetMontageNotifies(notifies,id,length) → Seek(entryTime) → SetMotionWarpWindows。
// startTime 是入口起始时间（秒、clip-local，缺省 0 = 从 section 0 起点播放）：
// 把它视作一次初始 seek——之前时刻的 notify 不补发、根位移只从 startTime 起积分，
// 结束时间早于 startTime 的 warp 窗口不再激活。正值必须落在 section 0 的
// [startTime, endTime) 内并留出播放余量，否则带 montage id/范围/请求值拒绝。
// 多 section（或单段自连接）时调用 SetMontageSections，由 SkeletalPlayback 在段末按
// nextSection/linkMethod 推进；入口只截短第一段，后续段仍按链推进。单段无链接仍是
// 单段播放范围（不再固定 [0,length]）。
// 失败抛异常且不改变 player（内部先复制候选再提交）。
// 注意顺序：Play→Select 会清除旧播放范围与旧 Montage notify 覆盖层，因此范围/notify 必须在
// Play 之后设置；先设范围再 Play 会被旧路径的既有清理语义丢弃。
void PlayMontage(SkeletalPlayback& player, const Montage& montage,
    const std::map<std::string, MotionWarpTarget>& targets, bool loop, float rate,
    bool inPlace, float startTime = 0);
}
