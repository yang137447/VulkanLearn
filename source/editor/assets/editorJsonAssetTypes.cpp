#include "editorJsonAssetTypes.h"

namespace VL::EditorUi
{

const char* EditorAssetKindName(EditorAssetKind kind)
{
    switch (kind)
    {
    case EditorAssetKind::MaterialInstance: return "Material Instance";
    case EditorAssetKind::Mesh: return "Mesh";
    case EditorAssetKind::Tressfx: return "TressFX";
    case EditorAssetKind::Scene: return "Scene";
    case EditorAssetKind::Texture: return "Texture";
    case EditorAssetKind::Terrain: return "Terrain";
    case EditorAssetKind::Blueprint: return "Blueprint";
    case EditorAssetKind::Retarget: return "Retarget";
    }
    return "Unknown";
}

const char* EditorAssetKindIcon(EditorAssetKind kind)
{
    switch (kind)
    {
    case EditorAssetKind::MaterialInstance: return "icons/material-instance.png";
    case EditorAssetKind::Mesh: return "icons/mesh.png";
    case EditorAssetKind::Tressfx: return "icons/mesh.png";
    case EditorAssetKind::Scene: return "icons/scene.png";
    case EditorAssetKind::Texture: return "icons/texture.png";
    case EditorAssetKind::Terrain: return "icons/terrain.png";
    case EditorAssetKind::Blueprint: return "icons/blueprint.png";
    case EditorAssetKind::Retarget: return "icons/blueprint.png";
    }
    return "";
}

} // namespace VL::EditorUi
