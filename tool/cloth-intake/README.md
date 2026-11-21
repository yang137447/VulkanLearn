# 3d66 Cloth Intake

## Offline showcase validation

For paper-model validation, use the existing untextured Fig.11/13 scenes first:

```powershell
py -3 tool/validation/offline/render_ashikhmin_paper_cases.py --samples 64
```

This renders Satin, Velvet, Slanted Velvet black/red with zero bounces and one
shared display calibration, then checks the paired inputs and appearance.
Current defaults match realtime: 5000 lux, EV100 9, compensation 0 and manual
display multiplier 1 yield resolved exposure 0.001627604, with ACES + sRGB.
The old EV0/manual 0.0012/Reinhard images are historical records. These are local calibration
conditions, not measured original-paper radiometry. Run/export records are in
`artifacts/ashikhmin-paper/`.

Regenerate the shared realtime stage and the three explicit offline derivatives:

```powershell
py -3 tool/cloth-intake/build_cloth_showcase_stage.py
py -3 tool/cloth-intake/render_offline_showcase.py --samples 32 --bounces 3
```

Add `--capture-realtime` to capture the generated `<variant>_direct.json`
control scenes with environment lighting disabled. `--realtime-only` refreshes
those screenshots without rerendering the offline films.

The second command launches the existing VL application with `--offline`, waits
for completed samples, exports linear Combined/Direct/Indirect PFM plus display
PPM and input manifest, then requests clean shutdown. Run logs and export paths
are recorded in `artifacts/cloth-offline/`. Use `--variants velvet` for one model,
`--bounces 0` for direct-only transport and `--timeout` for a slower machine.
An existing built `build-cloth-csharp/bin/main_game.exe` is required; `--exe`
selects another build.

The scenes are `Maps/SC_cloth_showcase/SC_cloth_showcase_<variant>_offline.json`.
They share the source FBX, TRS, stage geometry, centered browsing camera and
SunPatch. Cloth slots use the paper CPU closures, original maps and explicitly
selected imported fiber tangents. Environment lighting is disabled. Wall/floor
and noncloth slots explicitly use Lambert instead of realtime PBR; floor
specular/reflections are therefore absent. Exact ray shadows also differ from
the realtime shadow map. The offline PPM and viewport use ACES fitted + sRGB,
matching realtime mode 3. Historical Reinhard exports retain their old mapping.
For matched camera/light/material inputs and disabled Bloom, run:

```powershell
py -3 tool/validation/offline/compare_ashikhmin_runtime.py --samples 64
```

This creates generated CPU/GPU control pairs with EV100 9, compensation 0,
one 5000-lux light, no environment or textures, no indirect bounces and
offline manual multiplier 1. Results are in `artifacts/cloth-parity/`.
These images evaluate the paper models on this garment, not the original paper
figures, and do not add individual fiber geometry.

This folder converts one 3d66 clothing `.max` archive into VulkanLearn runtime
assets. It keeps the source hierarchy deterministic so a garment can be traced
back to its FBX, material and texture slots.

## Source Inventory

The supplied archive `23736651_43260341af6ea9796488b5339720131c.rar` contains:

- one 3ds Max 2018 / V-Ray 5 `.max` scene (about 596 MB)
- 233 texture images (about 457 MB)
- 53 top-level groups, `3d66Group001` through `3d66Group053`
- 260 `Editable_Poly` objects named `3d66-Editable_Poly-23736651-001` through
  `-260`

The `.max` contains no separate FBX/OBJ model files. Assimp does not read the
3ds Max scene format, so geometry export requires a machine with 3ds Max and its
FBX exporter installed.

Inventory the extracted images before export:

```powershell
py -3 tool/cloth-intake/inventory_3d66_textures.py `
  --texture-dir artifacts/incoming/23736651 `
  --output ..\VukanLearnResources\Maps\SC_cloth_showcase\source_texture_inventory.csv
```

The current set has 190 unique images out of 233, 110 images at 2048×2048, four
at 4096×4096, and five with alpha.

## Naming

| Output | Pattern |
| --- | --- |
| Source FBX | `SM_cloth_<group>.fbx` |
| Runtime mesh | `SM_cloth_<group>.json` |
| Material instance | `MI_cloth_<group>_m<material>.json` |
| Texture asset | `T_cloth_<group>_m<material>_<role>_<ordinal>.json` |
| Single-garment scene | `SC_cloth_<group>.json` |
| Overview scene | `SC_cloth_showcase.json` |

`<group>` is a zero-padded three-digit ID. `<material>` is a zero-padded
two-digit index in the exported FBX material-slot order. `<role>` is one of
`albedo`, `normal`, `pbr`, `emission` or `alpha`. `<ordinal>` disambiguates
multiple source maps that resolve to the same role.

## Export In 3ds Max

1. Open `3d66.com_23736651.max` in 3ds Max 2018 or newer.
2. Open the MAXScript editor and run
   `tool/cloth-intake/export_3d66_groups.ms`.
3. The script writes into
   `D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/cloth-rendering/VukanLearnResources/Maps/SC_cloth_showcase`:
   - `Source/Models/SM_cloth_<group>.fbx`
   - `Source/Textures/T_cloth_<group>_m<material>_<slot>_<ordinal>.<ext>`
   - `groups.csv`
   - `materials.csv`
   - `textures.csv`
4. Do not commit the 596 MB `.max` file. Commit only the referenced FBX,
   textures and generated manifests.

The exporter selects one top-level group at a time, preserves node transforms,
exports Y-up with smoothing groups and tangents, and records material order.
The runtime uses that order to bind `materialSlots`; do not sort the CSV rows by
hand.

## Generate Runtime Assets

Run from the code worktree:

```powershell
py -3 tool/cloth-intake/postprocess_3d66_exports.py
```

The default generated-scene model scale is `100`, because Assimp normalizes
FBX geometry to metres while VulkanLearn scene distances are authored in
centimetres. Override it after inspecting the first export:

```powershell
py -3 tool/cloth-intake/postprocess_3d66_exports.py --model-scale 100
```

The script:

- validates every FBX exists before writing a mesh asset
- creates one material instance per exported FBX material
- maps `diffuse`/`albedo`/`basecolor` slots to `albedoMap`
- maps normal slots to `normalMap`
- packs roughness/metallic/AO into one RGB `pbrParamMap`
- maps emissive and opacity slots when present
- enables only the matching `USE_*_MAP` macros per material instance
- keeps all cloth materials forward-only
- generates one preview scene per garment and one overview scene
- writes `showcase_manifest.json` with the complete source mapping
- uses a procedural studio sky instead of a shadow-casting backdrop

Every generated material instance uses
`M_clothAshikhminSatin`, `M_clothAshikhminVelvet` or
`M_clothAshikhminSlantedVelvet` and binds
`Common/Textures/T_clothAshikhminLut.json`.

## Closure Assignment

The 3d66 material names do not identify the fabric closure. The default is
Satin. To assign Velvet or Slanted Velvet, create a JSON map:

```json
{
  "001:m01": "velvet",
  "012:m01": "slanted_velvet"
}
```

Then run:

```powershell
py -3 tool/cloth-intake/postprocess_3d66_exports.py `
  --closure-map Maps/SC_cloth_showcase/closure_map.json
```

Keys also accept `"001"` for every material in a group or `"001:01"` without
the `m` prefix.

## Normal Maps

VulkanLearn consumes the MikkTSpace tangent handedness produced by the mesh
importer and does not flip normal-map G in the shader. If the 3ds Max source was
authored with DirectX-style Y- normal maps, pass:

```powershell
py -3 tool/cloth-intake/postprocess_3d66_exports.py --flip-normal-green
```

Keep the flag consistent for the whole collection; mixing conventions inside one
scene is a visual defect, not a shader fallback.

## Validation

After generation, build and smoke every output:

```powershell
cmake --build build-prebuilt --target main_editor main_game -j 4
build-prebuilt/bin/main_editor.exe --initial-scene Maps/SC_cloth_showcase/SC_cloth_showcase.json --exit-after-frames 2
build-prebuilt/bin/main_editor.exe --initial-scene Maps/SC_cloth_showcase/SC_cloth_001.json --exit-after-frames 2
```

Then verify:

- every `SM_*.json` reports the same material-slot count as its FBX
- every `MI_*.json` resolves its `T_*.json` texture assets
- the overview scene shows the expected group count
- normal direction, UV orientation and alpha coverage are visually checked

## Current Blocker

The current development machine has no 3ds Max installation, so the 53 FBX
meshes cannot be produced here. The scripts and naming contract are ready; the
first valid run must start from a 3ds Max machine with the source `.max`.
