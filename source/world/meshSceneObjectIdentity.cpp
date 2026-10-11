#include "world/meshSceneObjectIdentity.h"

namespace VL
{

std::string MeshSceneObjectIdentity(const WorldMeshObject& object, const std::string& objectName)
{
    return object.sceneObjectIdentity.empty() ? objectName : object.sceneObjectIdentity;
}

} // namespace VL
