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
- This repository does not provide the modern `.stsdk` `Standard.lua` one-byte `normal/binormal/tangent` unpack formula.
- Therefore it is useful as proof that runtime rendering should preserve official authored TBN, but it is not enough to decode the current `ubyte normal(1) / binormal(1) / tangent(1)` stream.
- Accurate modern TBN decoding still needs the SpeedTree SDK decoded vertex data or the matching modern SDK shader/sample unpack implementation.

### Empirical Packed TBN Decode Attempt

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

The byte order matches a Fibonacci sphere direction table:

```cpp
y = 1.0 - 2.0 * byte / 255.0;
r = sqrt(1.0 - y * y);
angle = -byte * goldenAngle;
direction = float3(r * cos(angle), y, r * sin(angle));
```

where `goldenAngle ~= 2.39996323`. In VulkanLearn this table is interpreted after SpeedTree-to-engine coordinate conversion, so byte `0` maps to engine up and byte `255` maps to engine down.

This is now used as an experimental decode path in `SpeedTreeSourceAdapter`: decoded source normal, tangent, and binormal are preserved in `SpeedTreeVertexAux`. Current render `Vertex` data uses decoded source normals where available, then generates tangent/sign through the shared MikkTSpace path so ordinary mesh and SpeedTree normal-map decoding use the same tangent-space convention. This should be treated as a verified hypothesis for the current asset, not a final replacement for official SDK validation.

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
- packed source TBN and wind bytes are preserved as raw SpeedTree auxiliary data; source normals use an experimental Fibonacci-sphere decode path, while render tangent/sign are generated with MikkTSpace until the official SDK result is available

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
