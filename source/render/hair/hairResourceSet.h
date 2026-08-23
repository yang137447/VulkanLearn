#pragma once

#include <memory>
#include <string>

#include "render/hair/hairAssets.h"

class Texture;

namespace VL
{

// 一个 World generation 的 Hair LUT 资源包；metadata 与 GPU texture 必须来自同一
// sourceIdentity，发布后只通过 const 句柄消费，不能在 active World 中原地改写。
class HairResourceSet
{
public:
    const HairAzimuthalLutMetadata& GetLutMetadata() const noexcept
    {
        return lutMetadata;
    }

    std::string sourceDigest;
    std::string sourceIdentity;
    std::shared_ptr<Texture> azimuthalLutTexture;

private:
    HairAzimuthalLutMetadata lutMetadata;

    friend class HairResourceLoader;
};

} // namespace VL
