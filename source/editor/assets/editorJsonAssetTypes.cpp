#include "editorJsonAssetTypes.h"

namespace VL::EditorUi
{

const char* EditorAssetKindName(EditorAssetKind kind)
{
    switch (kind)
    {
    case EditorAssetKind::MaterialInstance: return "Material Instance";
    case EditorAssetKind::Mesh: return "Mesh";
    case EditorAssetKind::Scene: return "Scene";
    case EditorAssetKind::Texture: return "Texture";
    case EditorAssetKind::Terrain: return "Terrain";
    }
    return "Unknown";
}

const char* EditorAssetKindIcon(EditorAssetKind kind)
{
    switch (kind)
    {
    case EditorAssetKind::MaterialInstance: return "icons/material-instance.png";
    case EditorAssetKind::Mesh: return "icons/mesh.png";
    case EditorAssetKind::Scene: return "icons/scene.png";
    case EditorAssetKind::Texture: return "icons/texture.png";
    case EditorAssetKind::Terrain: return "icons/terrain.png";
    }
    return "";
}

} // namespace VL::EditorUi
