#include "hair/tressfxRootWeights.h"

#include <string>
#include <utility>

namespace VL::Hair
{
namespace
{
RuntimeResult<TressfxRootWeightDerivation> RootWeightFailure(
    const char* code, std::string message)
{
    return RuntimeResult<TressfxRootWeightDerivation>::Failure(
        RuntimeError{code, std::move(message), {}, {}});
}
} // namespace

RuntimeResult<TressfxRootWeightDerivation> BuildTressfxRootWeights(
    const TressfxRootWeightInput& input)
{
    if (input.roots == nullptr || input.roots->empty())
        return RootWeightFailure("TressFXRootWeights.InvalidInput",
            "guide root positions are empty");
    if (input.body == nullptr)
        return RootWeightFailure("TressFXRootWeights.InvalidInput",
            "body skin source is empty");

    SdfProxySkinTransfer transfer;
    try
    {
        // RecordUnmatched：个别根点超距或最近顶点权重非正时留空，交调用方按根
        // 回退 rootBone；“全部未匹配”在下面统一判失败，不在这里逐点抛。
        transfer = BuildTressfxProxySkinTransfer(*input.roots, *input.body,
            input.maxNearestDistanceCm, TressfxSkinTransferPolicy::RecordUnmatched);
    }
    catch (const std::exception& error)
    {
        // 转移只对输入非法（空视图、非有限坐标、权重/影响范围损坏）抛异常；
        // 这里转成结构化失败，让加载期/spawn 用同一份文案边界处理。
        return RootWeightFailure("TressFXRootWeights.InvalidInput", error.what());
    }

    TressfxRootWeightDerivation derivation;
    try
    {
        derivation.strandBones = TressfxStrandBoneWeightsFromTransfer(transfer);
    }
    catch (const std::exception& error)
    {
        // CSR word 流损坏属于实现不变量被破坏；归入输入失败，避免半发布结果。
        return RootWeightFailure("TressFXRootWeights.InvalidInput", error.what());
    }
    derivation.maxNearestDistanceCm = transfer.maxNearestDistanceCm;
    derivation.unmatchedCount = static_cast<uint32_t>(transfer.unmatchedVertices.size());
    derivation.jointCount = transfer.jointCount;
    if (derivation.unmatchedCount == input.roots->size())
        return RootWeightFailure("TressFXRootWeights.NoMatch",
            "no guide root matched the source mesh within " +
            std::to_string(input.maxNearestDistanceCm) + " cm");
    derivation.matchedCount =
        static_cast<uint32_t>(input.roots->size()) - derivation.unmatchedCount;
    return RuntimeResult<TressfxRootWeightDerivation>::Success(std::move(derivation));
}
} // namespace VL::Hair
