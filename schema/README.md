# Schema Usage

Keep schema binding explicit and narrow.

Resource paths below are relative to `config/config.json -> resourcePath`. The runtime asset root has three
top-level directories — `Common/`, `Maps/`, `Generated/` — not a `resources/` tree.

- Bind `scene.schema.json` only to `Maps/SC_*/SC_*.json`.
- Bind `model_descriptor.schema.json` only to `Common/Meshes/SM_*.json` and `Maps/SC_*/Meshes/SM_*.json`.
- Bind `material_instance.schema.json` only to `Common/Materials/**/MI_*.json` and
  `Maps/SC_*/Materials/**/MI_*.json`.
- Bind `texture-asset` JSON to `Common/Textures/T_*.json` and `Maps/SC_*/Textures/T_*.json`
  (format contract: `documents/contracts/texture-asset-json-v1.md`).
- Bind `render_graph.schema.json` only to `config/renderGraphConfig.json`.
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
                "/Maps/SC_*/Meshes/SM_*.json"
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
