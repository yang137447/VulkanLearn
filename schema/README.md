# Schema Usage

Keep schema binding explicit and narrow.

Resource paths below are relative to `config/config.json -> resourcePath`. The runtime asset root has three
top-level directories — `Common/`, `Maps/`, `Generated/` — not a `resources/` tree.

- Bind `scene.schema.json` only to `Maps/SC_*/SC_*.json`.
- Bind `model_descriptor.schema.json` to `Common/Meshes/SM_*.json`, `Maps/SC_*/Meshes/SM_*.json`
  and the corresponding `TF_*.json` paths. TF keeps `type: "tressfx"` and has an independent
  editor/document kind. `tressfx_behavior.schema.json` supplies shared definitions for
  `simulationDefaults`, placement modes/overrides, skinning and external hairCollision;
  it is referenced by model and scene schemas, not bound to standalone assets.
- Bind `material_instance.schema.json` only to `Common/Materials/**/MI_*.json` and
  `Maps/SC_*/Materials/**/MI_*.json`.
- Bind `texture-asset` JSON to `Common/Textures/T_*.json` and `Maps/SC_*/Textures/T_*.json`
  (format contract: `documents/contracts/texture-asset-json-v1.md`).
- Bind `render_graph.schema.json` only to `config/renderGraphConfig.json`.
- Bind `retarget_profile.schema.json` only to `Common/Retarget/RT_*.json` and
  `Maps/SC_*/Retarget/RT_*.json`. It checks JSON shape; the production loader
  additionally checks skeleton identity, joint names, unique mappings/offsets,
  unit quaternions and the prohibition on locomotion-root rotation offsets.
  Optional `preview` SK paths belong to the Retarget tab; the asset worker
  validates root-relative paths and matching skeleton identities. Optional
  `preview.targetModelPath` enables paired animation; GT checks that the SM
  uses the opened RT and obtains its non-retargeted sourceModel. These fields
  do not replace runtime SM skeleton references.
- `hair_azimuthal_lut.schema.json` belongs to a generator input whose implementation was deleted on
  2026-09-12; it is retained for the paper-based rebuild. Do not bind it to any live asset.
- Do not bind any schema to broad patterns like `*.json`.

**There is no `terrain_descriptor.schema.json`.** Terrain has no runtime implementation in this
repository: `SceneAssetValidator` rejects a `terrain` scene object, and no `TR_*.json` exists in the
resource root. Terrain survives only in the editor's asset layer (`EditorAssetKind::Terrain`, and
`objectType == "terrain"` in material-instance navigation resolution). See `AGENTS.md`.

Validate model descriptors from the configured resource root:

```powershell
py -3 tool/validate_model_descriptors.py
```

Validate that every real scene satisfies `scene.schema.json` (this also guards the
direction that matters most — a schema stricter than `SceneAssetValidator` would
reject scenes the engine loads):

```powershell
py -3 tool/validate_scene_schema.py
```

`scene.schema.json` is rebuilt from `source/scene/validation/sceneAssetValidator.cpp`
and `source/world/loading/worldBuilder.cpp`, not from the previous schema. Two
policies in it are deliberate and easy to get wrong:

- **Object variants are self-contained.** Composing `allOf[nameAndType, …]` does not
  work while both halves declare `additionalProperties: false` — each half then
  rejects the other's fields and no object matches any branch. Keep `name` and `type`
  listed directly in each variant.
- **Scene objects allow unknown fields.** The validator reads the fields it knows and
  never rejects an extra key (`SC_car_showcase` carries a `shadow` key on its
  directional light and loads fine), so the schema must not be stricter.

`SC_scene01` is a documented, intentional exception: it keeps a legacy `sunLight`
object whose type the validator rejects, and the scene's own `description` says so.
`tool/validate_scene_schema.py` lists it in `INTENTIONAL_EXCEPTIONS` rather than
silently skipping it.

Override the resource root when needed:

```powershell
py -3 tool/validate_model_descriptors.py --resource-root D:\YYBWorkSpace\GitHub\VukanLearnResources
```

Recommended VS Code workspace settings:

```json
{
    "json.schemas": [
        {
            "fileMatch": [
                "/Maps/SC_*/SC_*.json"
            ],
            "url": "./schema/scene.schema.json"
        },
        {
            "fileMatch": [
                "/Common/Meshes/SM_*.json",
                "/Maps/SC_*/Meshes/SM_*.json",
                "/Common/Meshes/TF_*.json",
                "/Maps/SC_*/Meshes/TF_*.json"
            ],
            "url": "./schema/model_descriptor.schema.json"
        },
        {
            "fileMatch": [
                "/Common/Materials/**/MI_*.json",
                "/Maps/SC_*/Materials/**/MI_*.json"
            ],
            "url": "./schema/material_instance.schema.json"
        },
        {
            "fileMatch": [
                "/config/renderGraphConfig.json"
            ],
            "url": "./schema/render_graph.schema.json"
        }
    ]
}
```
