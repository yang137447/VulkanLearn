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
    // v1 LUT 继续保留给 anisotropy=0，确保旧 Cloth 的数值路径不因 v2 改造漂移。
    std::shared_ptr<Texture> directionalAlbedoLutTexture;
    // v2 LUT 是独立的 2D array，非零各向异性不得回退读取 v1 资源。
    std::shared_ptr<Texture> anisotropicDirectionalAlbedoLutTexture;
};

} // namespace VL
