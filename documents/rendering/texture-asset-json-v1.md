# Texture Asset JSON V1

## Goal

This document defines the first version of texture asset JSON files.

The goal is to move texture loading settings out of material instances, while keeping the format small and readable. A material instance should decide which texture is bound to which shader slot. A texture asset JSON should describe how a source image is loaded and sampled.

This is not intended to become a full Unreal-style texture importer in V1.

## Current Problem

Material instances currently bind textures directly by image path:

```json
"textures": {
    "albedoMap": "textures/T_Sifi_Head_01_BaseColor.tga",
    "normalMap": "textures/T_Sifi_Head_01_Normal.tga",
    "pbrParamMap": "textures/T_Sifi_Head_01_Param.tga"
}
```

The loader then infers texture behavior from the shader binding name:

- `normalMap` is treated as linear normal data
- `pbrParamMap` is treated as linear packed data
- other textures default to sRGB color

This works for the current content, but it makes texture loading rules implicit and easy to mix up when more assets are added.

## Texture JSON Format

Example:

```json
{
    "name": "T_Sifi_Head_01_Param",
    "type": "texture",
    "source": "textures/datas/T_Sifi_Head_01_Param.tga",
    "colorSpace": "linear",
    "mipmaps": true,
    "filter": "linear",
    "wrapMode": "repeat",
    "channelsDescription": {
        "r": "roughness",
        "g": "metallic",
        "b": "ao",
        "a": "unused"
    }
}
```

Field order should stay stable:

1. `name`
2. `type`
3. `source`
4. `colorSpace`
5. `mipmaps`
6. `filter`
7. `wrapMode`
8. `channelsDescription`

## Fields

### `name`

Required.

Human-readable texture asset name. It is intended for logs, debug names, tools, and future editor UI.

### `type`

Required.

Must be:

```json
"type": "texture"
```

This lets the loader verify that the JSON file is a texture asset description.

### `source`

Required.

Path to the real source image, relative to the project resource root used by `CommonFunction::Path`.

Texture descriptor files live directly under `resources/textures/` and use `T_*.json` names. Raw image assets live under `resources/textures/datas/`.

Example:

```json
"source": "textures/datas/T_Sifi_Head_01_Param.tga"
```

### `colorSpace`

Required.

Controls how the image data is interpreted when uploaded.

Supported values:

- `srgb`
- `linear`

Expected behavior:

- `srgb` maps to `TextureIO::LoadOptions::Transfer::SRGB`
- `linear` maps to `TextureIO::LoadOptions::Transfer::Linear`

This is separate from texture filtering. `colorSpace` controls color decoding. It does not control interpolation.

### `mipmaps`

Optional.

Default:

```json
"mipmaps": true
```

Controls whether the uploaded texture should generate mip levels.

### `filter`

Optional.

Default:

```json
"filter": "linear"
```

Supported values:

- `linear`
- `nearest`

Expected behavior:

- `linear` uses linear sampling
- `nearest` uses nearest-point sampling

This is separate from `colorSpace`. For example, a texture can be linear data and still use linear filtering:

```json
{
    "colorSpace": "linear",
    "filter": "linear"
}
```

### `wrapMode`

Optional.

Default:

```json
"wrapMode": "repeat"
```

Supported values:

- `repeat`
- `clamp`

Expected behavior:

- `repeat` maps to `vk::SamplerAddressMode::eRepeat`
- `clamp` maps to `vk::SamplerAddressMode::eClampToEdge`

V1 uses one wrap mode for both U and V. If per-axis control is needed later, it should be added deliberately instead of being exposed by default.

### `channelsDescription`

Optional.

Purely descriptive in V1. It does not participate in loading, shader binding validation, or runtime channel remapping.

Use it to document packed textures:

```json
"channelsDescription": {
    "r": "roughness",
    "g": "metallic",
    "b": "ao",
    "a": "unused"
}
```

This field is useful for maps such as PBR parameter textures, masks, and other packed data textures. It is normally unnecessary for base color, normal, or emission textures.

## File Layout

Texture assets use this layout:

```txt
resources/textures/T_Sifi_Head_01_Param.json
resources/textures/datas/T_Sifi_Head_01_Param.tga
```

The JSON descriptor is the asset that material instances reference. The image in `datas/` is source data used by the descriptor.

## Material Instance References

Material instances must reference texture asset JSON files:

```json
"textures": {
    "pbrParamMap": "textures/T_Sifi_Head_01_Param.json"
}
```

Direct image paths are not supported in material instances:

```json
"textures": {
    "pbrParamMap": "textures/datas/T_Sifi_Head_01_Param.tga"
}
```

The loader should reject direct image paths. Texture loading settings must come from the texture asset JSON.

## Runtime Mapping

The texture asset JSON should map to runtime options as follows:

| JSON field | Runtime meaning |
| --- | --- |
| `source` | image file passed to `TextureIO::Load` |
| `colorSpace` | `TextureIO::LoadOptions::Transfer` |
| `mipmaps` | device mip generation |
| `filter` | Vulkan sampler filter |
| `wrapMode` | Vulkan sampler address mode |
| `channelsDescription` | no runtime effect in V1 |

The texture cache key must include all fields that affect GPU resource creation or sampler state:

```txt
assetPath + source + colorSpace + mipmaps + filter + wrapMode
```

Without these fields in the cache key, the loader could incorrectly reuse one texture object for different sampling settings. `assetPath` is included so debug object names continue to point at the texture JSON that the material actually referenced.

## Explicitly Out Of V1

The following fields are intentionally not part of Texture JSON V1:

- `semantic` or `usage`
  - not added in V1; `colorSpace` is enough for the first migration step
- `flipY`
  - stays as an engine-side import convention for now
- full `sampler`
  - too low-level and too easy to misuse
- compression settings
  - should wait for a DDS/KTX2 or offline compression pipeline
- texture group, LOD group, or streaming settings
  - should wait until the renderer needs asset memory policy
- normal green-channel flipping
  - can be added later if assets with different normal conventions are introduced
- editor-side color adjustment
  - brightness, saturation, hue, and similar import adjustments are outside the current renderer scope

## Implementation Plan

1. Add a small texture asset description parser.
2. Extend texture creation options to include `colorSpace`, `mipmaps`, `filter`, and `wrapMode`.
3. Update `SceneLoader::LoadMaterialInstance` so material texture entries must reference `T_*.json` files.
4. Add `T_*.json` files for existing project textures.
5. Move raw image files under `resources/textures/datas/`.
6. Update material instances to reference texture asset JSON files.

## Design Boundary

Texture JSON owns image loading and sampling settings.

Material Instance JSON owns shader-slot binding:

```txt
texture asset -> how the image is loaded
material instance -> where the texture is bound
shader reflection -> which bindings actually exist
```

This keeps the first version explicit without turning the texture asset format into a full editor/importer system.
