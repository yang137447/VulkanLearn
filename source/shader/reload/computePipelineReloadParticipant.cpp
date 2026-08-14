#include "shader/reload/computePipelineReloadParticipant.h"

#include <sstream>
#include <stdexcept>

namespace VL
{

void ComputePipelineReloadParticipant::ValidateCandidateAbi(
    const ComputeShaderArtifact& candidate) const
{
    const ComputeShaderArtifact& active = GetActiveArtifact();
    if (candidate.abiSignature == active.abiSignature)
    {
        return;
    }

    std::ostringstream stream;
    stream << "Compute shader reload rejected because ABI changed: "
           << GetShaderName();
    for (const std::string& difference :
         active.abiSignature.DescribeDifferences(candidate.abiSignature))
    {
        stream << "\n  " << difference;
    }
    throw std::runtime_error(stream.str());
}

} // namespace VL
