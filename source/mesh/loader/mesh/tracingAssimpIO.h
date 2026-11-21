#pragma once

#include <assimp/DefaultIOSystem.h>
#include "core/tracingIfstream.h"

class TracingAssimpIO final : public Assimp::DefaultIOSystem
{
public:
    Assimp::IOStream* Open(const char* path, const char* mode = "rb") override
    {
        const int previousErrno = errno;
        auto* stream = Assimp::DefaultIOSystem::Open(path, mode);
        VL::RuntimeFileTrace::ObserveStreamOpen(path, stream != nullptr,
            "Assimp.DefaultIOSystem.Open", previousErrno);
        return stream;
    }
};
