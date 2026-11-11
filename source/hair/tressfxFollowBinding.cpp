#include "hair/tressfxFollowBinding.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace VL::Hair
{
namespace
{
    Eigen::Vector3f SafeNormalize(const Eigen::Vector3f& value)
    {
        const float squared = value.squaredNorm();
        if (!(squared > 1.0e-20f))
            return Eigen::Vector3f::Zero();
        return value / std::sqrt(squared);
    }

    // 逐点切线：末点用前一段，其余用后一段；退化时回退到另一侧。
    Eigen::Vector3f PointTangent(const std::vector<Eigen::Vector3f>& rest,
        size_t base, uint32_t local, uint32_t pointsPerGuide)
    {
        if (local + 1 < pointsPerGuide)
        {
            Eigen::Vector3f tangent = SafeNormalize(
                rest[base + local + 1] - rest[base + local]);
            if (tangent.squaredNorm() > 0.0f)
                return tangent;
        }
        if (local > 0)
            return SafeNormalize(rest[base + local] - rest[base + local - 1]);
        return Eigen::Vector3f(0.0f, -1.0f, 0.0f);
    }

    std::vector<uint32_t> FarthestPointSample(
        const std::vector<Eigen::Vector3f>& roots, uint32_t count)
    {
        Eigen::Vector3f centroid = Eigen::Vector3f::Zero();
        for (const Eigen::Vector3f& root : roots)
            centroid += root;
        centroid /= static_cast<float>(roots.size());

        size_t start = 0;
        float best = std::numeric_limits<float>::max();
        for (size_t i = 0; i < roots.size(); ++i)
        {
            const float distance = (roots[i] - centroid).squaredNorm();
            if (distance < best)
            {
                best = distance;
                start = i;
            }
        }

        std::vector<uint32_t> selected;
        selected.reserve(count);
        std::vector<float> nearest(roots.size(),
            std::numeric_limits<float>::max());
        size_t current = start;
        for (uint32_t step = 0; step < count; ++step)
        {
            selected.push_back(static_cast<uint32_t>(current));
            const Eigen::Vector3f& picked = roots[current];
            size_t next = 0;
            float farthest = -1.0f;
            for (size_t i = 0; i < roots.size(); ++i)
            {
                nearest[i] = std::min(nearest[i], (roots[i] - picked).squaredNorm());
                // 同距离取更小索引，保证结果可复现。
                if (nearest[i] > farthest)
                {
                    farthest = nearest[i];
                    next = i;
                }
            }
            current = next;
        }
        std::sort(selected.begin(), selected.end());
        return selected;
    }
} // namespace

Eigen::Vector3f TressfxChooseFollowReference(const Eigen::Vector3f& tangent)
{
    const Eigen::Vector3f axis = SafeNormalize(tangent);
    if (axis.squaredNorm() <= 0.0f)
        return Eigen::Vector3f::UnitY();
    const float ax = std::abs(axis.x());
    const float ay = std::abs(axis.y());
    const float az = std::abs(axis.z());
    if (ax <= ay && ax <= az)
        return Eigen::Vector3f::UnitX();
    if (ay <= az)
        return Eigen::Vector3f::UnitY();
    return Eigen::Vector3f::UnitZ();
}

TressfxFollowFrame TressfxMakeFollowFrame(
    const Eigen::Vector3f& tangent, const Eigen::Vector3f& reference)
{
    TressfxFollowFrame frame;
    frame.x = SafeNormalize(tangent);
    if (frame.x.squaredNorm() <= 0.0f)
        frame.x = Eigen::Vector3f(0.0f, -1.0f, 0.0f);

    Eigen::Vector3f y = reference - frame.x * reference.dot(frame.x);
    if (!(y.squaredNorm() > 1.0e-10f))
    {
        const Eigen::Vector3f fallback = TressfxChooseFollowReference(frame.x);
        y = fallback - frame.x * fallback.dot(frame.x);
    }
    frame.y = SafeNormalize(y);
    if (frame.y.squaredNorm() <= 0.0f)
        frame.y = TressfxChooseFollowReference(frame.x);
    frame.z = frame.x.cross(frame.y).normalized();
    return frame;
}

TressfxFollowBinding BuildTressfxFollowBinding(
    const std::vector<Eigen::Vector3f>& rootPositions,
    const std::vector<Eigen::Vector3f>& restPositions,
    uint32_t pointsPerGuide,
    uint32_t simulationGuideCount)
{
    if (pointsPerGuide < 2)
        throw std::invalid_argument("TressFX follow binding requires at least 2 points per guide");
    if (rootPositions.empty() ||
        restPositions.size() != rootPositions.size() * pointsPerGuide)
        throw std::invalid_argument("TressFX follow binding input dimensions do not match");
    for (const Eigen::Vector3f& root : rootPositions)
        if (!root.allFinite())
            throw std::invalid_argument("TressFX follow binding roots must be finite");
    for (const Eigen::Vector3f& point : restPositions)
        if (!point.allFinite())
            throw std::invalid_argument("TressFX follow binding rest points must be finite");

    TressfxFollowBinding binding;
    const size_t guideCount = rootPositions.size();
    if (simulationGuideCount == 0 ||
        simulationGuideCount >= guideCount)
        return binding;

    binding.simulationGuideIndices =
        FarthestPointSample(rootPositions, simulationGuideCount);
    binding.followGuideIndex.resize(guideCount);
    binding.offsets.assign(guideCount * pointsPerGuide, Eigen::Vector4f::Zero());
    binding.frameReferences.assign(guideCount, Eigen::Vector4f::Zero());

    // 每个模拟 guide 的参考轴只算一次；绑定到它的 follow 共用同一参考轴。
    std::vector<Eigen::Vector3f> references(guideCount, Eigen::Vector3f::Zero());
    for (uint32_t source : binding.simulationGuideIndices)
    {
        const size_t base = static_cast<size_t>(source) * pointsPerGuide;
        references[source] = TressfxChooseFollowReference(
            PointTangent(restPositions, base, 0, pointsPerGuide));
    }

    for (size_t guide = 0; guide < guideCount; ++guide)
    {
        std::vector<std::pair<float, uint32_t>> candidates;
        candidates.reserve(binding.simulationGuideIndices.size());
        for (uint32_t candidate : binding.simulationGuideIndices)
        {
            candidates.push_back({ (rootPositions[guide] - rootPositions[candidate])
                .squaredNorm(), candidate });
        }
        const size_t take = std::min<size_t>(
            TressfxFollowCandidateCount, candidates.size());
        std::partial_sort(candidates.begin(), candidates.begin() + take,
            candidates.end());
        // 候选内按整根 rest 形状选：根点邻近但方向/长度不同的 guide 会产生
        // 很大的逐点 offset，运动时把发束拉散。同代价时保留根点更近者。
        uint32_t nearest = candidates.front().second;
        double bestCost = std::numeric_limits<double>::max();
        for (size_t candidate = 0; candidate < take; ++candidate)
        {
            const uint32_t source = candidates[candidate].second;
            double cost = 0.0;
            for (uint32_t local = 0; local < pointsPerGuide; ++local)
            {
                const Eigen::Vector3f delta =
                    restPositions[guide * pointsPerGuide + local] -
                    restPositions[static_cast<size_t>(source) * pointsPerGuide + local];
                cost += delta.squaredNorm();
            }
            if (cost < bestCost)
            {
                bestCost = cost;
                nearest = source;
            }
        }
        binding.followGuideIndex[guide] = nearest;
        const Eigen::Vector3f reference = references[nearest];
        binding.frameReferences[guide] =
            Eigen::Vector4f(reference.x(), reference.y(), reference.z(), 0.0f);

        const size_t sourceBase = static_cast<size_t>(nearest) * pointsPerGuide;
        const size_t guideBase = guide * pointsPerGuide;
        for (uint32_t local = 0; local < pointsPerGuide; ++local)
        {
            const Eigen::Vector3f tangent = PointTangent(
                restPositions, sourceBase, local, pointsPerGuide);
            const TressfxFollowFrame frame =
                TressfxMakeFollowFrame(tangent, reference);
            const Eigen::Vector3f delta =
                restPositions[guideBase + local] - restPositions[sourceBase + local];
            binding.offsets[guideBase + local] = Eigen::Vector4f(
                delta.dot(frame.x), delta.dot(frame.y), delta.dot(frame.z), 0.0f);
        }
    }
    return binding;
}
} // namespace VL::Hair
