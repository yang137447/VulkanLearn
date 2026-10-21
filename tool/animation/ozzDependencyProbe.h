#pragma once

#include <array>
#include <vector>

#include <nlohmann/json.hpp>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>

namespace VL::AnimationPreflight
{
using JointPosition = std::array<float, 3>;
using ModelPositions = std::vector<JointPosition>;

// 开发工具的数值检查入口：消费已构建的骨架/动画与归一化时刻，输出源空间关节位置。
// 不负责资源导入、坐标换基、播放时钟、蒙皮或运行时 World；调用者保证骨骼语义映射一致。
std::vector<ModelPositions> SampleModelPositions(
    const ozz::animation::Skeleton& skeleton,
    const ozz::animation::Animation& animation,
    const std::vector<float>& ratios);

// 只读取锁定 ozz 依赖中的可信夹具，比较原始关键帧与压缩采样；不接收外部 archive。
nlohmann::json BuildDependencyReport();
}
