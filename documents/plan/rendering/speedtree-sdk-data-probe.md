# SpeedTree SDK Data Probe

This document defines the first SDK-backed investigation step before VulkanLearn commits to a runtime foliage asset format.

The goal is to use the SpeedTree SDK as the source asset parser, dump the data it exposes, and only then decide which data becomes VulkanLearn runtime geometry, material, LOD, billboard, and wind formats.

## Goal

- Load one SpeedTree runtime asset, such as `.stsdk`, through the official SDK.
- Enumerate the data exposed by SDK APIs without inventing VulkanLearn storage yet.
- Write a human-readable probe JSON under generated resources.
- Compare the dump against a real exported tree before designing `VLFoliageAsset` or `SpeedTreeVertex`.

The probe is an import-time tool. Runtime rendering should not depend on SpeedTree SDK headers or SDK classes.

## Auxiliary References

`RetiredSDK/speedtree` may be used as a historical concept reference only. It is SpeedTree 4.1-era code with unclear redistribution terms and does not define the modern `.stsdk` binary format used by current SpeedTree assets.

Allowed use:

- read public class and struct names to understand old SpeedTree runtime data families
- use it as a checklist for probe sections
- compare historical concepts with data exposed by the official modern SDK

Not allowed for VulkanLearn:

- do not vendor the repository
- do not copy implementation code
- do not treat SpeedTree 4.1 structs as the final runtime format
- do not assume it can parse modern `.stsdk` files

The useful historical checklist from SpeedTree 4.1 is:

- branch and frond geometry were indexed geometry with multiple discrete LODs
- leaves had their own LOD data, centers, card indices, optional mesh data, and leaf rocking/rustling scalars
- billboards were separate 360-degree and horizontal billboard geometry
- wind data used primary/secondary weights and matrix indices
- LOD values carried active LOD indices and alpha/fade values per geometry family

Use this checklist to make sure the modern SDK probe asks the right questions, not to lock VulkanLearn to old SDK memory layouts.

Important TBN finding:

- SpeedTree 4.1 runtime APIs expose decoded `normal`, `binormal`, and `tangent` as float arrays.
- The reference renderer uploads those float vectors directly; its branch shader derives binormal from uploaded normal and tangent to save attributes.
- The Runtime SDK 10 example wind shader publishes the matching Fibonacci unpack formula:

  ```hlsl
  float fZ = 0.99609375 - 0.0078125 * fPacked;
  float fRadius = sqrt(1.0 - fZ * fZ);
  float fTheta = fPacked * 2.39996322973;
  return float3(cos(fTheta) * fRadius, sin(fTheta) * fRadius, fZ);
  ```

- The packed value is the byte index (`0..255`) from the vertex stream. `Normalize="true"` describes the GPU vertex-input conversion; it does not change the on-disk byte or turn the index into a new authored direction. If the shader receives the Oak attribute as a normalized float, it must recover the index with `fPacked = normalizedValue * 255.0` before calling the SDK Fibonacci helper.
- VulkanLearn applies the existing source-to-engine vector conversion `(x, z, -y)` after this source-space unpack. This is now the formal Oak v10 TBN direction rule, not the previous render-only candidate.

### Empirical Packed TBN Decode Attempt (historical)

The current `Oak_Complex_Rules.stsdk` `Standard.lua` stream stores:

```text
normal(1) / binormal(1) / tangent(1) / ao(1)
```

Data analysis against block 0 geometry suggests that each of the three direction bytes is an independent index into the same 256-entry unit-vector table. Grouping vertices by a single byte value and comparing against geometry-derived vectors gives these approximate weighted coherences:

```text
normal byte   -> generated normal:   0.8865
binormal byte -> generated binormal: 0.9102
tangent byte  -> generated tangent:  0.8484
```

The earlier render compatibility candidate approximated a Fibonacci sphere direction table:

```cpp
y = 1.0 - 2.0 * byte / 255.0;
r = sqrt(1.0 - y * y);
angle = -byte * goldenAngle;
direction = float3(r * cos(angle), y, r * sin(angle));
```

where `goldenAngle ~= 2.39996323`. In VulkanLearn this table is interpreted after SpeedTree-to-engine coordinate conversion, so byte `0` maps to engine up and byte `255` maps to engine down.

The v10 validation path preserves the three bytes as raw source values and stores their confirmed UNORM decode (`byte / 255`) separately. The importer now uses the Runtime SDK 10 formula above for the render-space source TBN vectors.

### Runtime SDK 10 wind evidence

The public Runtime SDK 10 manual's example shader and wind pages provide the following confirmed rules:

Reference pages:

- [Runtime SDK 10: What's new](https://docs.unity3d.com/speedtree-runtime-sdk/manual/whats-new.html)
- [Runtime SDK 10: Wind overview](https://docs.unity3d.com/speedtree-runtime-sdk/manual/wind-overview.html)
- [Runtime SDK 10: Example wind shader](https://docs.unity3d.com/speedtree-runtime-sdk/manual/example-wind-shader.html)
- [Runtime SDK 10: CWindStateMgr](https://docs.unity3d.com/speedtree-runtime-sdk/manual/cwindstatemgr-in-core.html)
- [Runtime SDK 10: Wind shader configuration](https://docs.unity3d.com/speedtree-runtime-sdk/manual/wind-shader-configuration.html)
- [Runtime SDK 10: Coordinate systems](https://docs.unity3d.com/speedtree-runtime-sdk/manual/coordinate-systems.html)

- `SpeedTreeWind.h` is shared between C++ and HLSL/GLSL. The v10 symbol names use the `RuntimeSdk` suffix (`WindRuntimeSdk`, `SWindInputRuntimeSdk`, `SWindStateRuntimeSdk`, and related branch/ripple structures).
- `branch1` and `branch2` are independent vertex inputs. Each has `weight`, Fibonacci-packed `direction`, and an integer-packed noise offset; branch1 also carries ripple and branch2 carries blend in the Standard packer.
- The offset is decoded as `UnpackInteger3(normalizedByte * 255, vec3(9, 9, 3))` and multiplied by the model tree extents. The integer unpack is confirmed; Oak's exact SDK-provided global extents and runtime wind state are still not decoded from the v10 root table.
- The CPU `CWindStateMgr` produces a time-varying `SWindStateRuntimeSdk` containing wind direction/strength, shared/branch1/branch2 state, ripple state, bounding box, and branch stretch limits. The vertex shader combines that state with the per-vertex attributes; there is no offline point-cache requirement.
- Runtime SDK 10 also documents fixes for alternate coordinate systems and branch-level chaos, so those behaviors must remain in the v10 shader port rather than being approximated by a generic sinusoid.
- Version boundary: the Oak file is a v10.0 STSDK asset, but the Runtime SDK 10 shader manual explicitly calls the sample backend “SpeedTree Games 9” wind. v10 changes the Runtime SDK packaging, names, and platform support; it does not introduce a separate v10 wind equation. Our implementation must therefore align the v10 packer/file contract with the Runtime SDK Games 9 wind functions.

### Oak v10 authored wind table

The Oak binary investigation now has a named wind layout instead of an anonymous root-table guess.

Evidence used together:

- Runtime SDK 9/10 documentation defines the `Common`, `Shared`, `Branch1`, `Branch2`, and `Ripple` configuration families.
- UE 5.7's local `SpeedTreeDataBuffer/TreeReader9.h` describes the nested Games 9 data-buffer field order. It is used only as a local schema reference; no licensed implementation is copied into VulkanLearn.
- Oak root-table entry `10` contains a 20-field table whose nested sizes and values match that schema exactly.

Confirmed Oak mapping:

```text
root 3  -> source-space bounds, six float32 values
root 10 -> authored Games 9 / Runtime SDK wind configuration

wind[0]  -> Common
wind[1]  -> Shared
wind[2]  -> Branch1
wind[3]  -> Branch2
wind[4]  -> Ripple
wind[10] -> SharedStartHeight
wind[11] -> Branch1StretchLimit
wind[12] -> Branch2StretchLimit
wind[13] -> unexposed; semantic not confirmed by TreeReader9
wind[14] -> unexposed; semantic not confirmed by TreeReader9
wind[15] -> DoShared
wind[16] -> DoBranch1
wind[17] -> DoBranch2
wind[18] -> DoRipple
wind[19] -> DoShimmer
```

Each Shared/Branch table contains five **20-point** curves in this order:

```text
Bend, Oscillation, Speed, Turbulence, Flexibility, Independence
```

Ripple contains four 20-point curves plus two scalars:

```text
Planar, Directional, Speed, Flexibility, Shimmer, Independence
```

This corrects an important legacy assumption: the Oak Runtime SDK wind curves have 20 authored samples. UE's legacy `FSpeedTreeWind` uses ten samples for older wind, but that is not the Oak v10 data contract.

Fields 13 and 14 must not currently be interpreted as confirmed asset values.
Their positions resemble `SConfigRuntimeSdk::m_bLodFade` and
`SConfigRuntimeSdk::m_fWindIndependence`, but the public Games 9
`TreeReader9.h` deliberately exposes fields 10-12 and then 15-19 without
accessors for 13/14. Oak stores zero in both positions. More importantly, the
Modeler 10.2 standard `invoke_wind.h` does not read either value: it omits LOD
fade and supplies `SWindOptionsRuntimeSdk::m_fWindIndependence = 1.0` directly.
VulkanLearn therefore treats `1.0` as a renderer integration constant matching
the 10.2 reference shader, not as an authored per-species field.

### Modeler 10.2 cross-asset probe

Three Modeler 10.2 exports were inspected in addition to the original 10.0 Oak:

- `Bush_Desktop.stsdk`
- `Palm_Desktop.stsdk`
- `palm.stsdk`

All three files report version `10.2`, retain the 41-entry root table, and have
exactly one root matching the Runtime SDK wind signature: root `10`, with 20
top-level fields and nested field counts `12, 6, 6, 6, 6`. Each file contains
the same 19 wind curves, and every curve contains 20 float samples. This makes
root `10` and the 19-by-20 curve layout a cross-species, cross-minor-version
observation for the inspected 10.0 and 10.2 Runtime SDK exports.

These particular Bush and Palm exports do **not** contain authored Runtime SDK
motion values. Their source `.spm` files have `Settings:Mode = 2`, while the
Runtime SDK wind samples use mode `1`. The exported Runtime SDK tables therefore
contain enabled flags but default scalar values and nineteen all-zero curves.
They validate the binary schema, geometry/vertex packing, and no-motion runtime
path, but they do not validate propagation of species-specific wind curves.
`samples/Games/Conifer/Conifer.spm` is the available non-Oak sample already in
Runtime SDK wind mode and is the next required export for that content check.

The numeric validation tool now preserves the fixed Oak vertex and attachment
regressions for `Oak_Complex_Rules.stsdk`, while selecting representative
per-section vertices for other species. Oak 10.0 and all three 10.2 files pass
the SDK/VulkanLearn CPU-state and vertex-equation comparisons. The adapter
accepts only the inspected `10.0` and `10.2` versions; other 10.x minor versions
remain rejected until their layouts are inspected.

Oak values relevant to the first runtime implementation:

```text
source bounds min = (-27.146841, -28.575600, -3.400510)
source bounds max = ( 30.944800,  27.012800, 57.557000)

strength response     = 9.3
direction response    = 9.3
shared start height   = 0.3335
branch1 stretch limit = 30.0
branch2 stretch limit = 30.0

shared independence  = 1.0
branch1 independence = 0.0341300443
branch2 independence = 0.0682600886
ripple independence  = 0.085325107
ripple shimmer       = 0.5

DoShared  = true
DoBranch1 = true
DoBranch2 = true
DoRipple  = true
DoShimmer = true
```

Runtime SDK 10 omits the old serialized `Common.CurrentStrength` field in this Oak table. This is correct: current strength is live state owned by `CWindStateMgr`, while the `.stsdk` table supplies authored response curves and feature configuration. VulkanLearn therefore initializes current strength separately instead of reading past the twelve-field Common table.

## UE 5.7 Reference Track

Local UE 5.7 provides a useful, engine-integrated SpeedTree reference. It should be treated as a behavior and data-layout reference, not as code to copy into VulkanLearn.

Local engine root:

```text
D:\sofeware\Epic Games\UE_5.7
```

Useful files and assets:

```text
Engine\Plugins\Editor\SpeedTreeImporter
Engine\Plugins\Editor\SpeedTreeImporter\Source\SpeedTreeImporter\Private\SpeedTreeImportFactory.cpp
Engine\Plugins\Editor\SpeedTreeImporter\Content\SpeedTree9
Engine\Shaders\Private\SpeedTreeCommon.ush
Engine\Shaders\Private\MaterialTemplate.ush
Engine\Source\Runtime\Engine\Public\SpeedTreeWind.h
Engine\Source\Runtime\Engine\Private\SpeedTreeWind.cpp
```

Observed importer support:

- `.srt` as SpeedTree
- `.st` as SpeedTree v8
- `.st9` as SpeedTree v9

For v9 imports, UE routes through the SpeedTree GameEngine9 runtime loader and creates a `UStaticMesh`. The importer uses SpeedTree9 master material assets as templates:

```text
/SpeedTreeImporter/SpeedTree9/SpeedTreeMaster
/SpeedTreeImporter/SpeedTree9/SpeedTreeBillboardMaster
```

Important material behavior:

- importer creates material instances per SpeedTree material name
- material slots are added to `StaticMesh->GetStaticMaterials()`
- billboard LODs use the billboard master template
- two-sided or billboard materials map to a two-sided foliage shading model
- wind feature switches are driven by the imported wind config, including branch1, branch2, ripple, shimmer, shared motion, camera-facing handling, and branch2 availability

Important SpeedTree9 vertex UV packing used by UE:

```text
UV0: Diffuse UV
UV1: Branch1Pos, Branch1Dir
UV2: Branch1Weight, RippleWeight
UV3: Lightmap UV
UV4: Branch2Pos, Branch2Dir      if Branch2 data exists
UV5: Branch2Weight, unused       if Branch2 data exists
UV6+: camera-facing anchor data  if facing geometry exists
```

This is highly relevant for VulkanLearn because it proves a production engine keeps SpeedTree branch/ripple wind data as per-vertex attributes and feeds the real deformation through the vertex shader, rather than relying on offline point-cache animation.

UE runtime wind has two halves:

- CPU side: `FSpeedTreeWind` updates smoothed strength, direction, gust, oscillation times, and a packed shader table.
- GPU side: `SpeedTreeCommon.ush` consumes the SpeedTree uniform buffer and vertex UV data for global, branch, leaf, frond, and rolling wind.

Useful uniform groups to mirror conceptually:

- `WindVector`
- `WindGlobal`
- `WindBranch`
- `WindBranchTwitch`
- `WindBranchWhip`
- `WindBranchAnchor`
- `WindBranchAdherences`
- `WindTurbulences`
- `WindLeaf1Ripple`
- `WindLeaf1Tumble`
- `WindLeaf1Twitch`
- `WindLeaf2Ripple`
- `WindLeaf2Tumble`
- `WindLeaf2Twitch`
- `WindFrondRipple`
- `WindRollingBranch`
- `WindRollingLeafAndDirection`
- `WindRollingNoise`
- `WindAnimation`

VulkanLearn should not mirror UE names blindly in runtime JSON, but the probe should dump enough data to fill equivalent concepts.

## Asset Entry Decision

SpeedTree `.stsdk` files enter VulkanLearn through the existing mesh asset descriptor path, not as a new scene object type.

Scene JSON remains format-agnostic:

```json
{
  "type": "mesh",
  "modelPath": "models/speedtree/SM_oak_complex_rules.json"
}
```

The `SM_xxx.json` file keeps the existing flat `name / type / modelDataPath` shape. A SpeedTree source is selected with `type = "speedtree"`:

```json
{
  "name": "Oak Complex Rules",
  "type": "speedtree",
  "modelDataPath": "models/datas/Oak_Complex_Rules.stsdk",
  "materialSlots": [
    {
      "name": "BarkBase",
      "materialInstancePath": "materials/foliage/MI_oak_bark.json"
    },
    {
      "name": "LeafSummer",
      "materialInstancePath": "materials/foliage/MI_oak_leaf_summer.json"
    }
  ]
}
```

Rules:

- `.stsdk` is the source asset format.
- `SM_xxx.json` is the VulkanLearn asset entry and validation boundary.
- `type = "speedtree"` dispatches to `SpeedTreeParserCore` or a future SDK-backed importer.
- `materialSlots` must be a present, non-empty ordered array.
- for `type = "speedtree"`, `materialSlots` count must match the material count parsed from `.stsdk`; array order matches parsed SpeedTree material order, and `name` values are readable config/debug labels that do not participate in mapping.
- SpeedTree texture paths may still be parsed from `.stsdk`, but VulkanLearn material instance binding is explicit through `materialSlots`.
- no `importCache` field is used in the first version; data is parsed directly from `.stsdk`.
- runtime rendering consumes VulkanLearn-owned mesh sections, material slots, and wind semantics.
- `.st9` remains a UE reference/probe input unless explicitly added later as another mesh asset `type`.

## Custom Parser Track

VulkanLearn can develop its own experimental `.stsdk` parser, but it should grow from observed data and local tests rather than copied SDK implementation code.

### Current Runtime Import Notes

The first runtime-facing `.stsdk` importer lives behind `SpeedTreeSourceAdapter` and is intentionally narrower than the probe goal:

- it parses the observed geometry blocks directly from `.stsdk`
- it preserves source material slot names for `materialSlots` count validation
- it imports only the highest-detail 3D geometry block for now
- it skips lower LOD blocks and billboard blocks until a real LOD/billboard policy exists
- it converts SpeedTree source coordinates during parsing: source `X, Y, Z-up` becomes VulkanLearn `X, Y-up, Z` with `(x, z, -y)`
- it applies the same conversion to `lodPosition` and vector-like data that is decoded as vectors
- it reverses each triangle's index order during import to match VulkanLearn's current `vk::FrontFace::eClockwise` pipeline convention, mirroring the existing Assimp `ModelLoader` behavior
- packed source TBN and wind bytes are preserved as raw SpeedTree auxiliary data; Oak v10 stores the confirmed UNORM decode beside the raw bytes, uses the Runtime SDK 10 Fibonacci direction unpack for source normals and branch directions, decodes branch noise offsets into normalized integer coordinates, and generates tangent/sign through the shared MikkTSpace path
- current SpeedTree material sampling treats `_Normal.rgb` as tangent-space normal and `_Normal.a` as gloss; the shader converts gloss to VulkanLearn roughness with `roughness = 1.0 - gloss`

For `Oak_Complex_Rules.stsdk`, the current observed blocks are:

```text
block 0: vertexCount 109606, stride 28, indexCount 298086, sections 6
block 1: vertexCount 57906,  stride 28, indexCount 126885, sections 6
block 2: vertexCount 31560,  stride 28, indexCount 65124,  sections 6
block 3: vertexCount 78,     stride 16, indexCount 156,    sections 1
```

The runtime importer selects the largest `stride 28` block as the temporary highest LOD. This keeps the first visual test focused on one complete tree instead of stacking every LOD and billboard card in the scene.

The first custom parser layer is intentionally shallow and is implemented as a C++ library under external dependencies:

```text
extern/SpeedTreeParser/src/speedTreeParserCore.cpp
extern/SpeedTreeParser/include/SpeedTreeParser/speedTreeParserCore.h
```

It extracts:

- file header
- first offset table, if it matches the observed `SpeedTreeSDK____` layout
- readable strings
- material names
- texture names
- embedded `SpeedTreeVertexPacker` XML
- embedded `SpeedTreeTexturePacker` XML

This layer is useful because modern `.stsdk` files can embed readable packer metadata even before we understand the full binary layout. For `Oak_Complex_Rules.stsdk`, this already exposes packed semantics such as `position`, `lod_position`, `normal`, `binormal`, `tangent`, `ao`, `wind_branch1_*`, `wind_branch2_*`, `wind_ripple`, and `blend`.

The second custom parser layer should only start after several assets are compared:

- identify what each offset-table entry points to
- classify binary chunks by strings, XML blocks, and numeric ranges
- find geometry buffers and index buffers
- decode vertex streams according to the embedded vertex packer XML
- compare decoded bounds against Modeler/SDK output if available

Rules:

- keep parser notes and assumptions next to the tool
- mark guessed fields as guessed in JSON output
- never make the renderer depend on guessed binary fields
- do not copy RetiredSDK implementation code into VulkanLearn
- prefer official SDK probe output as ground truth when available

## Output Location

Recommended output:

```text
<resourcePath>/generated/speedtree_probe/
  ST_test_tree.probe.json
  ST_test_tree.vertex_streams.csv
```

The JSON is for structured inspection. The optional CSV is for quickly checking per-vertex stream values, pivot-like data, wind weights, UV channels, and color channels in a spreadsheet.

## Probe Sections

### Source Asset

Dump enough identity information to make imports reproducible:

- source file path
- SDK version, if available
- tree name or SDK tree identifier
- coordinate system assumptions
- geometry scalar / unit scale
- bounds / extents
- vertex packing name

This data is mostly import metadata. Runtime may only need bounds, scale, and a stable asset id.

### Materials

For each SDK material, dump:

- material index
- material name
- texture references exposed by SDK
- alpha test / opacity hints
- two-sided or leaf-card hints, if exposed
- geometry type hints, if exposed
- original SDK material fields that affect shader selection

Later conversion target:

```text
SDK material -> material slot -> VulkanLearn MI_*.json
```

Do not decide `MI_*.json` field names from guesses. First inspect what the SDK actually exposes for bark, leaves, fronds, billboards, and cutout materials.

### LODs

For each LOD, dump:

- LOD index
- LOD distance / scalar / transition data exposed by SDK
- draw call count
- draw calls that belong to this LOD
- whether this LOD contains 3D geometry, billboard geometry, or both

Runtime format decisions depend on this dump:

- whether VulkanLearn stores per-LOD mesh sections
- whether billboard LOD is a separate asset path
- whether LOD transitions need cross-fade, dither, or hard switch first

### Draw Calls / Geometry Groups

For each SDK draw call or geometry group, dump:

- draw call index
- LOD index
- material index and material name
- geometry type: branch, frond, leaf, billboard, grass, or SDK-specific category
- vertex count
- index count
- vertex range / index range, if available
- section or group name, if available
- render-state hints that affect culling, alpha test, or billboard rendering
- whether the group maps conceptually to branch, frond, leaf mesh/card, 360 billboard, or horizontal billboard

This is the likely source for VulkanLearn `MeshSection`.

### Vertex Streams

This is the most important probe area. Dump the stream layout before designing a final vertex type.

For each draw call / geometry group, dump:

- attribute semantic name exposed by SDK
- component type and component count
- byte offset / stride, if available
- min / max value for numeric streams
- first few sample values

Look especially for:

- position
- LOD position or packed LOD position
- normal
- tangent and handedness
- bitangent or binormal
- UV channels
- vertex color channels
- ambient occlusion or baked lighting
- primary and secondary wind weights
- wind direction, offset, phase, ripple, rock, rustle, or blend terms
- wind matrix indices or equivalent packed selectors
- branch level / leaf group
- anchor / pivot / pivot offset
- ripple / tumble / frond data
- billboard-specific attributes

The first VulkanLearn `SpeedTreeVertex` should be designed only after these stream names and value ranges are known.

For the current `Oak_Complex_Rules.stsdk` shallow preprobe, readable strings already show a standard vertex packer with:

- `position(3) / texcoord_u(1)`
- `lod_position(3) / texcoord_v(1)`
- `normal(1) / binormal(1) / tangent(1) / ao(1)`
- `wind_branch1_weight(1) / wind_branch1_dir(1) / wind_branch1_offset(1) / wind_ripple(1)`
- `wind_branch2_weight(1) / wind_branch2_dir(1) / wind_branch2_offset(1) / blend(1)`

This confirms that branch wind data is present in the source asset at least as packed vertex stream semantics. The official SDK probe still needs to resolve the actual buffers, ranges, and draw-call ownership.

### Wind Config

Dump the SDK wind configuration and runtime wind state separately.

Configuration dump:

- Modeler-authored wind preset or config fields exposed by SDK
- wind mode / quality / LOD options
- branch, leaf, frond, ripple, tumble, gust, and turbulence fields if exposed
- any shader constant layout metadata exposed by SDK

Runtime state dump:

- wind direction
- wind strength
- time
- gust state
- generated shader constants from the SDK wind manager
- wind matrix table or equivalent branch/frond transform data, if exposed
- leaf rock/rustle scalar or angle arrays, if exposed

The import format should preserve authored wind intent. The runtime GPU buffer can be a later compressed VulkanLearn layout.

### Billboards / Impostors

First probe only:

- billboard count
- atlas texture references
- plane orientation data
- per-billboard UV rects
- billboard normal / ambient data, if exposed
- LOD link to billboard draw calls

Do not include billboards in the first runtime mesh path until static 3D geometry and material slots are stable.

### Instances / Forest Data

Single-tree `.stsdk` import may not contain forest instance placement. If the SDK sample or source asset includes instance/population data, dump:

- position
- orientation basis
- scale
- per-instance random seed or variation
- LOD value
- LOD transition

Keep this separate from base tree data. Runtime instance buffers will likely be their own system.

## Probe JSON Shape

This is not the final runtime format. It is intentionally verbose.

```json
{
  "type": "speedTreeSdkProbe",
  "source": {
    "path": "sourceAssets/speedtree/ST_test_tree.stsdk",
    "sdkVersion": "",
    "treeName": "",
    "vertexPackingName": "",
    "unitScale": 1.0,
    "bounds": {
      "min": [0.0, 0.0, 0.0],
      "max": [0.0, 0.0, 0.0]
    }
  },
  "materials": [],
  "lods": [],
  "drawCalls": [],
  "vertexStreams": [],
  "wind": {
    "config": {},
    "stateSamples": []
  },
  "billboards": [],
  "instances": []
}
```

## Implementation Boundary

Recommended code ownership:

```text
source/speedtree/
  speedTreeSdkBridge.h/.cpp
  speedTreeSdkProbe.h/.cpp
  speedTreeSdkProbeTypes.h
```

Rules:

- SDK headers stay inside `source/speedtree/`.
- `SceneLoader`, `RenderSystem`, `MaterialInstance`, and mesh runtime code must not include SpeedTree SDK headers.
- Probe types should be SDK-free plain structs that can be serialized to JSON.
- The first probe may be built behind a CMake option if the SDK is not installed on every machine.

## Acceptance

- A local SDK build can load one `.stsdk` source asset.
- The probe outputs materials, LODs, draw calls, vertex streams, and wind data to JSON.
- Missing SDK support does not break the normal VulkanLearn build.
- The final runtime asset format remains undecided until the first real probe JSON is reviewed.

### Validation build baseline

- VulkanLearn and `speedtree_wind_validation` are currently configured and accepted with the `MinGW Makefiles` generator and `C:\Software\mingw64\bin\g++.exe`.
- This MinGW/GCC path is the authoritative build baseline for the current SpeedTree wind audit.
- Clang is not the active project toolchain. A failure reproduced only with Clang is a compiler-portability follow-up, not a blocker for the current MinGW implementation acceptance.
- Use `cmake --build build --target speedtree_wind_validation -j` followed by `ctest --test-dir build -R speedtree_wind_numeric --output-on-failure` for the numeric validation path.

## Current Wind Runtime Implementation

The Oak v10 path now uses the public Runtime SDK Games 9 wind contract at runtime:

- the importer preserves the two packed branch streams as normalized GPU attributes
- GLSL recovers the Fibonacci direction index and `UnpackInteger3(offset * 255, float3(9, 9, 3))`
- source bounds are converted to VulkanLearn coordinates and uploaded as runtime min/max values for height and offset calculations
- v10 branch offsets are already expanded to tree units before applying branch independence; the legacy v9 height re-scaling is not applied
- each imported mesh source owns a wind profile keyed by its model cache identity; all instances and sections of that species share one CPU state while different species retain independent bounds, stretch limits, and authored curves
- the per-object UBO contains the profile's live wind direction/strength and sampled Shared, Branch1, Branch2, and Ripple states, selected from the draw packet's profile key
- all active profiles advance once at the frame boundary so the base and shadow passes consume the same wind sample
- deformation order is Ripple -> Branch2 -> Branch1 -> Shared, matching Modeler v10.2.0 `SpeedTreeWind.h::WindRuntimeSdk()`
- instance decorrelation uses the separate runtime `wind independence` option at the Modeler `invoke_wind.h` value of `1.0`; it is not the authored Shared independence value
- Branch motion uses an anchor plus length-preserving reconstruction; Shared motion uses the squared height weight
- `CWindStateMgr` strength response, Gust, xorshift32 seed, 20-point curve sampling, and four independent three-dimensional noise-position integrators follow the local Modeler v10.2.0 header
- direction response retains the authored response duration but uses an engine-side shortest-arc quaternion interpolation; exact opposite directions choose a deterministic axis closest to world up instead of passing through the Runtime SDK midpoint singularity
- the procedural fallback uses `QNoise(xy * 20) - 0.5`, `QNoise(yx * 10)`, and a zero third channel
- `speedtree_wind_validation` compares the official CPU state against VulkanLearn over seven fixed times with a fixed direction, compares four fixed Oak vertices after Ripple, Branch2, Branch1, and Shared, and separately validates 90-degree, 180-degree, and mid-transition direction retargeting

The validator's CPU state side executes the installed official header directly.
The vertex side uses two independent CPU evaluators: a source-space transcription
of `WindRuntimeSdk()` and a Y-up transcription of VulkanLearn's GLSL. This proves
the coordinate conversion and formula mapping for the fixed inputs, but it does
not yet read back the actual SPIR-V shader result from the GPU.

### Attachment validation rule

Wind-time attachment checks must not pair arbitrary spatially nearby vertices.
SpeedTree Cluster Branch geometry is an alpha card, so transparent card corners
and crossing card surfaces can be close to bark without being an authored root.
The Oak regression therefore identifies a root by the packed semantics first:

- `Ripple == 0` and `Branch2 weight == 0` on the Cluster Branch side
- identical `Branch1(weight, direction, offset)` on the Cluster Branch and Bark sides
- a small rest-space search radius used only after the semantic filter

The visual check then samples the Cluster Branch base-color alpha with the same
`0.1` clip threshold and chooses the nearest visible sample in each root-owned
component. This prevents transparent card padding or a nearby branch crossing
from being reported as a dynamic seam. For Oak, 88 semantic roots and 79
alpha-visible roots stay below `0.05` model units of growth at the validation
state (worst observed growth `0.02422225`).

The response fields use the SpeedTree authoring meaning: they are durations for a
0-to-100% strength change (and a 180-degree direction change), not exponential
rates. Gust Rise and Fall are scalar multipliers of the strength response time;
smaller scalars therefore produce faster transitions. The renderer must preserve
this interpretation before adding any further spatial wind approximation.

The implementation intentionally keeps live state renderer-owned and separate
from the authored `.stsdk` table. Each imported base-tree asset contributes a
profile keyed by its model cache identity. All sections and instances of that
species share one CPU wind state, while different species keep independent
bounds, stretch limits, authored curves, and sampled GPU state. Renderer-level
weather controls are broadcast to every active profile.

### Wind data ownership

The word `Common` in `SConfigCommon` means a common base structure used by the
different SpeedTree wind algorithms. It does **not** mean one scene-global copy.
The Runtime SDK manual states that every base-tree `CCore` owns its own
`CWindStateMgr`, and all instances of that base tree share that object.
`CForest` merely broadcasts weather commands to those per-base-tree managers.

| Ownership | Data | Update frequency | Consumer | Cross-species sharing |
|---|---|---|---|---|
| Renderer / weather controller | target strength, target direction, wind enabled, gust enabled/force request, absolute clock | command or frame | broadcast to every active base-tree wind manager | Yes as input commands, not as computed state |
| Renderer integration policy | Runtime SDK noise function/texture, 20-sample interpolation rule, GUI speed scale `0.1`, instance independence option `1.0` | initialization / constant | CPU sampler and vertex shader | Yes for the 10.2 standard integration |
| Base-tree authored profile | strength/direction response, gust parameters, 19 x 20 curves, Shared/Branch/Ripple independence values, shimmer, shared start height, stretch limits, feature flags, source bounds | asset load | that base tree's `CWindStateMgr` and shader state | No; shared only by instances of the same base tree |
| Base-tree live state | current/target/interpolation strength and direction, gust envelope and RNG state, four noise positions, sampled Shared/Branch/Ripple values | once per frame per active base tree | all draws and instances using that profile | No; one live state per base tree |
| Instance | model transform and world position; optional integration-specific LOD state | per instance / visibility update | local wind direction, global noise offset, geometry placement | No |
| Vertex | Branch1/Branch2 weights, packed directions and noise offsets, ripple weight, LOD position and TBN attributes | immutable vertex data | Runtime SDK vertex deformation | No |

The 20-point curves are therefore authored per base tree. The curve **shape and
values** are not universal; only the fact that Runtime SDK mode uses 20 samples,
the sampling equation, and the meaning/order of the 19 curves are common.

The Games 9 `TreeReader9` schema exposes `CurrentStrength` at field 15, but this
Oak v10 asset's Common table has only twelve fields and omits it. Unity's current
SpeedTree 9 importer also does not copy the field into its runtime wind config
when it is present. VulkanLearn follows the same ownership rule: current
strength is live weather/runtime state rather than part of the immutable
base-tree profile.

Runtime wind controls follow the input-boundary rule defined in
`documents/architecture/coding-guidelines.md`. The `windstrength` command
validates finite `[0, 1]` input before publishing a runtime command; the
downstream wind profile and simulation code trust that contract and do not
repeat the same range check.

## Gust Algorithm Evidence

The direct implementation evidence is the local Modeler v10.2.0 baseline at
`C:/Software/SpeedTree_10.2.0_extracted/{app}`. Its
`standard_shaders/SpeedTreeWind.h` contains the complete `CWindStateMgr`
and Runtime SDK vertex deformation source. The official SDK 9 documentation
provides supporting semantic descriptions:

- gusts are discrete events, not a periodic sine wave
- each event chooses a strength in the configured gust strength/variance range
- strength rises from the base value to the gust peak
- the peak remains for the configured duration
- strength falls back to the base value
- `Rise` and `Fall` scalars control the rise/fall durations independently
- a duration of `0.0` still has an effect because rise and fall time are not counted in peak duration
- frequency controls how often a new event is scheduled
- the simulation is advanced with absolute elapsed seconds and is intentionally continuous rather than looping

The local UE 5.7 `FSpeedTreeWind` implementation remains an additional Legacy
engine reference, not the authority for Oak Runtime SDK 10. The matching v10
header establishes the actual xorshift32 random sequence, trigger comparison,
rise/hold/fall envelope, and `noisePosition -= direction * deltaTime * 0.1 * speed`
integration used by the current VulkanLearn path.

References:

- [SpeedTree SDK 9 Advanced Wind](https://docs.speedtree.com/doku.php?id=advancewind)
- [SpeedTree SDK 9 Wind Overview](https://docs9.speedtree.com/sdk/doku.php?id=wind-overview)
- [SpeedTree SDK 9 CWindStateMgr](https://docs9.speedtree.com/sdk/doku.php?id=cwindstatemgr-in-core)
- [SpeedTree Modeler Games Wind](https://docs9.speedtree.com/modeler/doku.php?id=windgames)

Local behavior reference (not copied as runtime code):

- `D:\sofeware\Epic Games\UE_5.7\Engine\Source\Runtime\Engine\Private\SpeedTreeWind.cpp`
- `D:\sofeware\Epic Games\UE_5.7\Engine\Source\Runtime\Engine\Public\SpeedTreeWind.h`

The UE source is an engine-integrated Legacy implementation and must not override
the matching Runtime SDK v10 evidence. A periodic sinusoid remains explicitly
rejected because it contradicts the verified event/duration/rise/fall state
machine.
