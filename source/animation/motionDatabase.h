#pragma once

#include "motionFeatures.h"
#include <memory>

namespace VL::Animation
{
// 固定输入构建的只读特征表和统计；clip/frame 顺序稳定。没有搜索、业务筛选或 GPU 对象。
struct MotionDatabase
{
    FeatureSchema schema;
    std::vector<std::string> clipIds;
    std::vector<bool> looping;
    struct Frame
    {
        uint32_t clip = 0;
        float time = 0;
        std::vector<float> features;
    };
    std::vector<Frame> frames;
    std::vector<double> means, scales;
    std::vector<float> Normalize(const std::vector<float>& features) const;
};

MotionDatabase BuildMotionDatabase(const SkeletonMesh& mesh, const std::vector<Clip>& clips,
    const FeatureSchema& schema, const std::vector<std::pair<size_t, bool>>& members);
nlohmann::json MotionDatabaseJson(const MotionDatabase& database);
MotionDatabase ParseMotionDatabase(const nlohmann::json& json);
}
