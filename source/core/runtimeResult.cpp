#include "runtimeResult.h"

// This translation unit keeps the runtime result contract in the build so
// compile errors surface wherever the shared loading result type is used.

namespace VL
{

std::string FormatRuntimeError(const RuntimeError& error)
{
    std::string message = error.code + ": " + error.message;
    if (!error.sourcePath.empty())
    {
        message += " [" + error.sourcePath + "]";
    }
    if (!error.sourceNode.empty())
    {
        message += " node=" + error.sourceNode;
    }
    return message;
}

} // namespace VL
