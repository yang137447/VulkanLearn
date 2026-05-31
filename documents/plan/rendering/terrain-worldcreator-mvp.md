# Terrain World Creator MVP

This document captures the current first-step terrain intake path for World Creator style data.

## Goal

The terrain system is intentionally small:

- load a heightmap exported by terrain tools such as World Creator
- generate one CPU mesh at scene-load time
- render it through the existing `RenderableObject`, `SceneObject`, and `MaterialInstance` path
- keep terrain material textures in the existing `MI_*.json` and `T_*.json` asset system

This is not yet a streaming terrain, clipmap, virtual texture, or tiled LOD system.

## Scene Object

Scene files can reference a terrain descriptor with a `terrain` object:

```json
{
    "name": "Terrain_Main",
    "type": "terrain",
    "terrainPath": "terrains/TR_worldcreator_demo.json",
    "position": [0.0, 0.0, 0.0],
    "rotation": [0.0, 0.0, 0.0],
    "scale": [1.0, 1.0, 1.0]
}
```

The scene node owns transform only. Terrain source data belongs in `terrains/TR_*.json`.

## Terrain Descriptor

```json
{
    "name": "TR_worldcreator_demo",
    "type": "terrain",
    "heightmapPath": "textures/terrain/HM_worldcreator_demo.png",
    "materialInstancePath": "materials/MI_terrain_worldcreator_demo.json",
    "size": [512.0, 512.0],
    "heightScale": 80.0,
    "heightOffset": 0.0,
    "uvScale": [8.0, 8.0],
    "flipY": false,
    "heightChannel": "r"
}
```

Current loader behavior:

- `heightmapPath` resolves through `CommonFunction::Path`, so shared external assets are preferred.
- 8-bit and 16-bit LDR heightmaps are normalized to `0..1`.
- HDR and EXR heightmaps read float values directly.
- final vertex height is `height * heightScale + heightOffset`.
- the generated mesh is centered around local origin on X/Z.
- normal and UV data are generated on CPU.

## World Creator Export Notes

Recommended first export shape:

- heightmap: 16-bit PNG when possible
- albedo/normal/roughness/etc: normal VulkanLearn texture assets referenced by material instance JSON
- one terrain descriptor per imported terrain block

If a World Creator project exports tiled terrain, RAW heightfields, splat maps, or layer weights, convert or describe those assets explicitly before adding runtime support. The MVP keeps source data correctness in JSON and avoids hidden runtime guessing.

## Future Steps

Likely next steps, when needed:

- tiled terrain descriptors
- layer/splat map bindings in a dedicated terrain material
- generated tangent aligned to local slope
- CPU decimation or import-time mesh baking
- GPU tessellation or meshlet/LOD path
