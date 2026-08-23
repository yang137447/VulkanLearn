#pragma once

#include <memory>
#include <string>

class Texture;

namespace VL
{

class ClothResourceSet
{
public:
    std::string sourceDigest;
    std::shared_ptr<Texture> directionalAlbedoLutTexture;
};

} // namespace VL
