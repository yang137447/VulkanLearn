# Animation Preflight

P0/P1 audit/import, P2/P3 content/capture, P4 trajectory/features and P5 Motion Matching
tools for the GASP plan. Runtime search and local TRS crossfades use the native
playback clock; C# owns the high-level allowed-clip set.

## Build

From the repository root, with CMake 3.24+ and the existing MinGW toolchain:

```powershell
cmake -S . -B build -DVULKANLEARN_BUILD_ANIMATION_TOOLS=ON
cmake --build build --target vl_animation_probe animation_preflight_tests -j8
ctest --test-dir build -R "^(AnimationSampling|AnimationDependency|UnrealProjectInventory|AnimationFbxInspection|SkeletalImport|SkeletalBlend|SkinRenderHistory|MotionHistory|MotionPrediction|MotionFeatures|MotionDatabase|MotionDatabaseInputs|MotionMatching)\." --output-on-failure
```

The option defaults to OFF and controls tools/tests. The engine fetches ozz-animation 0.16.0 at commit
`6cbdc790123aa4731d82e255df187b3a8a808256`, verified by the archive SHA256 in
`extern/OzzAnimation/CMakeLists.txt`. Only runtime/offline libraries are used;
upstream samples, tools and tests are disabled. The MIT notice is copied to
`build/bin/licenses/ozz-animation.txt`.

## Dependency Report

```powershell
.\build\bin\vl_animation_probe.exe --output artifacts/gasp-p0/ozz-probe.json
```

Reads the pinned library's `pab_skeleton.ozz` and `pab_walk_raw.ozz`, builds a
compressed animation locally, samples 121 times including both endpoints, and
compares model-space joint positions with the raw-keyframe reference. The JSON
contains the hierarchy, every sampled frame, error, duration and BLAKE3 input
digests. Use a new output path for each run; existing files are rejected.

This is a dependency fixture in its original source space. It is not a GASP
character, an FBX import test, VL coordinate validation, or a rendered character.
The .ozz archive reader only handles these trusted dependency fixtures. It is
not an external asset loader or a corruption validator.

## UE Project Inventory

Close the source editor and other writers before capturing the reference:

```powershell
.\build\bin\vl_animation_probe.exe --project "D:/path/GameAnimationSample.uproject" --output artifacts/gasp-p0/gasp-inventory.json
```

The inventory reads the descriptor plus `.uasset`, `.umap`, `.uplugin` and `.ini`
files below Content/Config/Plugins. It records relative paths, byte lengths and
BLAKE3 digests in stable path order. It excludes project-root Saved, Intermediate
and DerivedDataCache. Links resolving outside the project are rejected.

The tool never launches UE or writes into the source project; output must be
outside it. Source paths with non-ASCII names are supported by the inventory.
Filename prefixes are only hints. A real UE Asset Registry audit, GASP revision,
license/source confirmation, skeleton/curve/event/schema inspection and export
are separate P0 evidence. A filesystem inventory does not complete P0.

## UE Asset Audit And Export

`audit_gasp.py` runs inside UE 5.8's PythonScript commandlet. Use a copy containing
the project's descriptor, Content and Config, outside the original project.
It reads the actual Asset Registry classes, exports a fixed mannequin and twelve
locomotion clips, and writes reference-pose/RAW-pose/root samples, float-curve
time/value keys, notify structures, sync markers and selected schema/database/
Chooser/AnimBlueprint text. It never calls asset/project save.

Set `VL_GASP_AUDIT_OUTPUT` to a new directory outside the isolated project:

```powershell
$env:VL_GASP_AUDIT_OUTPUT = "D:/audit/new-output"
& "C:/Software/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" `
  "D:/audit/project-copy/gasp.uproject" `
  "-run=pythonscript" "-script=D:/YYBWorkSpace/GitHub/VulkanLearn/tool/animation/audit_gasp.py" `
  "-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities" `
  "-AllowCommandletRendering" "-RenderOffscreen" "-unattended" "-nosound" "-nop4" "-nosplash"
```

Do not use `-nullrhi` for skeletal-mesh FBX export: UE 5.8.2 asserted on
`SkinnedMeshComponent::MeshObject`, including with material baking disabled.
Allowing commandlet rendering requires a working GPU/RHI and may compile
engine shaders on first use. Material baking is explicitly disabled; materials
are not converted to VL. Read both process status and `audit.json`; a file
existing does not imply successful export. Capability failures are recorded.

`asset-export-complete` refers only to the script's fixed export list. It does
not certify all GASP metadata, source licensing, curve interpolation/tangents,
virtual-bone reconstruction, AnimBlueprint behavior, coordinate conversion,
or VL rendering. Struct/text dumps use UE's representation, not a VL format.
RAW reference evaluation disables retargeting and retains root motion.
`AnimPoseSpaces.WORLD` denotes component/model space in this API.

`audit_gasp_traversal.py` reuses the same commandlet invocation. Set
`VL_GASP_TRAVERSAL_OUTPUT` to a new directory and optionally
`VL_GASP_TRAVERSAL_CLIPS` to a comma-separated AnimSequence name list; it audits
all Traversal clips and exports only the selected FBX. P8 exports
`M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot` and
`M_Neutral_Traversal_Vault_1_0_stand_F_Lfoot`; P10 exports
`M_Neutral_Traversal_Climb_Start_2_5_stand_F_Lfoot`、
`M_Neutral_Traversal_Climb_Start_2_5_walk_F_Lfoot` 和
`M_Neutral_Traversal_Climb_Start_2_5_run_F_Lfoot`.

`audit_gasp_catch.py` is the read-only Catch companion. It enumerates
`AM_M_Neutral_Traversal_Catch_*`, exports each montage T3D with UE's own
`ObjectExporterT3D`, resolves the referenced `AnimSequence` from
`SlotAnimTracks(... AnimReference=...)`, and exports those FBX files. Set three
new output paths outside the isolated project:

```powershell
$env:VL_GASP_CATCH_OUTPUT  = "D:/audit/catch/ue-catch-audit.json"
$env:VL_GASP_CATCH_T3D_DIR = "D:/audit/catch/t3d"
$env:VL_GASP_CATCH_FBX_DIR = "<resources>/Maps/SC_gasp/Source"
& "C:/Software/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" `
  "D:/audit/project-copy/gasp.uproject" `
  "-run=pythonscript" "-script=D:/YYBWorkSpace/GitHub/VulkanLearn/tool/animation/audit_gasp_catch.py" `
  "-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities" `
  "-AllowCommandletRendering" "-RenderOffscreen" "-unattended" "-nosound" "-nop4" "-nosplash"
```

The report path must not exist yet; all three paths must stay outside the UE
project. The script never saves packages. Catch montage ids are not always
`AM_` + clip id (for example `AM_..._Catch_Cliff_high_standF` plays
`M_..._Catch_Cliff_high_stand`), so the clip is taken from the T3D reference
rather than the filename; `import_gasp_montages.py` uses the same rule.

## Sequence And Montage Export

`export_gasp_ue_raw.py` (UE commandlet) produces the raw animation dump used by
the JSON converters. `VL_GASP_ANIM_CLIPS` selects the mesh asset whose
`skinning.clips[]` list is read, `VL_GASP_ANIM_OUTPUT` is the new dump path, and
the optional `VL_GASP_ANIM_CLIP_IDS` (comma-separated clip ids) restricts the
batch:

```powershell
$env:VL_GASP_ANIM_CLIPS    = "<resources>/Maps/SC_gasp/Meshes/SM_mannequin.json"
$env:VL_GASP_ANIM_CLIP_IDS = "M_Neutral_Traversal_Catch_Hurdle_high_stand,..."
$env:VL_GASP_ANIM_OUTPUT   = "artifacts/catch/ue-anim-raw-catch.json"
# same commandlet invocation as above with -script=.../export_gasp_ue_raw.py
```

Then, from the repository root:

```powershell
py -3 tool/animation/export_gasp_sequences.py `
  --raw artifacts/catch/ue-anim-raw-catch.json --clips <comma-separated ids> --continue-on-error

py -3 tool/animation/import_gasp_montages.py --clips <comma-separated ids>
```

`--clips` restricts either converter to a subset of the SM clip ids (duplicates,
empty values and ids not in the SM are rejected). `--continue-on-error` lets
`export_gasp_sequences.py` finish the remaining clips and report per-clip
failures instead of aborting at the first one; it is for batch work, not for
accepting a broken asset. Both converters refuse to overwrite existing outputs
unless `--force` is passed.

Full re-export note: run `export_gasp_ue_raw.py` **without**
`VL_GASP_ANIM_CLIP_IDS` so that the dump covers every clip in the current
`SM_mannequin.json` (31 clips with the Catch set). Generating the dump from a
stale 22-clip mesh silently drops the new clips. Then run both converters with
`--force` and compare the regenerated files byte-for-byte before committing; the
Catch round produced 31/31 sidecars and 19/19 montages unchanged.

## FBX Inspection And Comparison

```powershell
.\build\bin\vl_animation_probe.exe --fbx D:/audit/clip.fbx --output artifacts/gasp-p0/clip-inspection.json
```

Uses the existing Assimp dependency with FBX pivot preservation disabled,
axis correction disabled, empty-bone removal disabled and zero postprocess
flags. Animation-only files legitimately carry Assimp's INCOMPLETE scene flag;
inspection retains that flag instead of applying the static-mesh rejection.
Reports all nodes, stable node/parent indices, matrices, skin-weight statistics and
animation TRS keys. No GlobalScale, weight normalization/truncation, bone removal
or runtime publication occurs. This is an audit tool, not the P1 importer.

`compare_gasp_export.py --audit <UE-output> --inspection <FBX-inspection-dir>
--output <new-report.json> [--figure <new.png>]` compares five UE RAW model-space
poses per clip, source duration and root translations sampled at 60 Hz.
Each FBX inspection filename must match its clip stem. Requires numpy/scipy;
the optional sagittal figure also requires matplotlib.
The fixed candidate basis is `(UE.X, -UE.Y, UE.Z)` in source centimeters.
The comparison reports failures instead of fitting a transform to hide them.
Virtual bones (`VB `) are listed separately; they are not physical FBX joints.
The numerical thresholds are 0.1 cm, 0.1 degree and 0.00001 seconds. Mesh reference
poses and bind-matrix residuals are also compared. An authored source track must
survive export; bones without a source track use their bind pose and still
participate in the pose comparison. Passing is
limited to these sampled properties, not vertex skinning, weight remapping, curve
evaluation, full animation behavior or the final VL coordinate policy.

## P1 Native CPU Import

```powershell
.\build\bin\vl_animation_probe.exe --import-gasp artifacts/gasp-p0/ue-audit-06 --output artifacts/gasp-p1/cpu-import.json
& "C:/Program Files/Python314/python.exe" tool/animation/compare_gasp_import.py `
  --audit artifacts/gasp-p0/ue-audit-06 --imported artifacts/gasp-p1/cpu-import.json `
  --output artifacts/gasp-p1/comparison.json --figure artifacts/gasp-p1/comparison.png
```

Use new output paths. This fixed audit entry calls the reusable native
`vl_animation_assets` importer, preserves the full influence stream through
MikkTSpace, builds ozz skeleton/clips and emits poses/root samples in VL space.
The fixed policy is `VL = (FBX.X, FBX.Z, -FBX.Y) = (UE.X, UE.Z, UE.Y)`,
centimeters, Y-up, model forward +Z. No Actor/mesh component yaw is applied.
Raw root TRS sampling replaces the compressed root lane to preserve trajectory
precision; body joints use compressed ozz sampling. Missing tracks/components
use the mesh rest pose.

The comparison checks 12 clips against UE RAW references at fixed thresholds:
0.1 cm, 0.1 degree, 0.00001 seconds. Curves and notify metadata are preserved,
not executed or asserted to have UE-equivalent interpolation. Curve keys outside
the clip duration are retained. Full `.copy` files remain inputs with BLAKE3
digests. This report itself does not create a World or render an animated character.

## P2 Formal Level And CPU Reference

The formal `Maps/SC_gasp` assets are maintained directly. The one-time
`build_gasp_preview.py --formal` promotion was retired on 2026-10-20 (see
`documents/contracts/animation-preflight-tools.md`); the audit manifest remains
under `Generated/Import/SC_gasp`.

```powershell
.\build\bin\main.exe --initial-scene Maps/SC_gasp/SC_gasp.json --dev-ui
```

Meshes, Materials and Source belong to Maps/SC_gasp; the original UE project is
never used as an output directory.
Double-click SM_mannequin in Content Browser to use the skeletal preview controls.
The default clip is Idle (index 0); Walk is index 1 and retains 800 cm of root travel.
In a level, use `animation pause`, `animation clip 1`, `animation seek 0.25`,
and `animation play` in Console. The native clock is independent of C# playback.

```powershell
.\build\bin\vl_animation_probe.exe --skin-gasp artifacts/gasp-p0/ue-audit-06 `
  --time 0.25 --output artifacts/gasp-p2-cpu025.json
py -3 tool/animation/build_gasp_skin_reference.py --reference artifacts/gasp-p2-cpu025.json `
  --resources ../VukanLearnResources --name GaspCpu025
```

The CPU report evaluates the fixed Walk clip and retains every influence.
The reference builder creates a static OBJ/SM/MI/Scene under Generated/Validation,
using the formal level's lights, camera and ground. Reverse the final CW index
adaptation before exporting OBJ because the static importer applies it again.
The fixture is for external image comparison; it is not a new runtime test flag.

`capture_gasp_p2.py --output artifacts/gasp-p2-captures` runs serial production
launches for two poses, single/dual thread, paused repeat, static CPU Beauty/Normal,
velocity and editor layout. Requires numpy/Pillow. It checks full BMP writes and stderr.
CPU comparison excludes the scene-name HUD and covers the character/shadow ROI;
the preset allows at most 0.1% ROI pixels above four RGB bytes of error.
Capture processes are terminated after completion and do not prove graceful teardown.

## P3 DefaultLevel And Materials

`Maps/SC_gasp/SC_gasp_playground.json` is the separate playable DefaultLevel.
`SC_gasp.json` remains the P2 skinning review scene. In the editor, use Play,
WASD, Left Shift, Space and mouse look; the character HUD shows its C# state
and native clip feedback. P5 replaces the former discrete locomotion requests
with native Motion Matching and local TRS crossfades. Optional static-support
foot grounding defaults off; full UE FootPlacement, inertialization and stride
warping remain unimplemented.

`export_gasp_level.py` reads visible LOD0 static sections, authored placements,
collision enablement and PlayerStart from the isolated project's DefaultLevel.
Set `VL_GASP_LEVEL_OUTPUT` to a new directory, then use the same commandlet
invocation above with this script and `ProceduralMeshComponent` enabled.
`build_gasp_level.py --export <directory> --resources <resource-root>` creates
the separate scene and an initial review palette; it refuses existing outputs.

`export_gasp_materials.py` resolves dynamic/constant instance inheritance,
exports all prototype source textures, dependent material/function graphs,
component bounds/transforms, sky and Logo metadata. Set
`VL_GASP_MATERIAL_OUTPUT` to a new directory and run it in the isolated copy
with commandlet rendering enabled. The public landscape render-target exporter
requires `Editor.AsyncAssetCompilationFinishAll` before readback; a successful
UE return alone does not establish valid height pixels.

```powershell
& "C:/Program Files/Python314/python.exe" tool/animation/import_gasp_materials.py `
  --export artifacts/gasp-p3-material-export06 --resources ../VukanLearnResources `
  --asset-editor build/bin/vl_asset_edit.exe
```

Requires numpy/scipy/Pillow and a completed material export. The importer refuses
existing dedicated texture/material/mesh output directories and audit manifests.
Inspect failed runs before retrying; it does not roll back previously written files.
It reads production scene capabilities, authors component references/new objects
and environment fields outside those capabilities, and modifies/validates/saves
the supported scene description through one production asset-editor session.

Formal images/descriptors live in `Source/Textures/Level` and `Textures/Level`;
material and mesh descriptors use `Materials/LevelTextured` and
`Meshes/LevelTextured`. PNG rows are reversed offline to cancel VL's single
material upload flip; HDR environment rows remain unchanged. Color spaces follow
the resolved UE texture settings, including the sRGB roughness source.
The two Engine default textures are preserved even though their hidden source
component is absent from the visible level geometry.

`M_gaspGrid` preserves three projected grid layers, object/world alignment,
top colors and paint detail. Its dedicated vertex entry uses the unused color
varying for the source interpolated local normal, preserving nonuniform-scale
projection semantics without changing the shared vertex ABI.
Transparent source colors use ordinary PBR alpha blending; constant emission is
represented by linear 8-bit 1x1 images. The Logo uses a static alpha plane in
place of the source deferred decal. The axis-aligned landscape height field is
baked to ordinary static triangles and collision, not a terrain runtime object.
Lighting is a VL review setup, with no Lumen/exposure equivalence claim.
UE Blueprint interactions, widgets and NPC behavior are not executed.

The current review settings are sun 10 lux, environment intensity 2 and EV100 3.
Launch the playable level with C# enabled:

```powershell
.\build\bin\main.exe --initial-scene Maps/SC_gasp/SC_gasp_playground.json --dev-ui
```

`capture_gasp_p3.py --resources ../VukanLearnResources --output artifacts/new-captures`
creates fixed-camera scenes under Generated/Validation and captures overview,
Logo, detail, BaseColor, Normal and Roughness through the production loader.
Fixing the camera before loading avoids same-frame camera/screenshot snapshot
latency. It requires numpy/Pillow, refuses existing output directories and
terminates its own capture processes; it does not establish input acceptance
or graceful teardown. The public Landscape export is a resampled height field,
not a raw-heightmap identity guarantee.

## P4 Trajectory And Feature Database

In the playable level, Play and enable Motion in the viewport toolbar
(or enter `motiondebug 1`). Cyan is achieved postphysics root history;
amber is a prediction using current movement intent, without future collision.
Green marks sampled joints. Pause freezes history; Stop resets it.
The skeletal model tab displays raw feature channels for the selected clip/time.

```powershell
.\build\bin\vl_animation_probe.exe --build-motion-db Maps/SC_gasp/MotionMatching/DB_gasp.json `
  --resource-root ../VukanLearnResources --output artifacts/new-db-report.json
py -3 tool/animation/browse_motion_database.py `
  ../VukanLearnResources/Generated/MotionMatching/<identity>.json --output artifacts/new-browser.html
py -3 tool/animation/verify_gasp_motion_database.py --help
```

The report gives the cache path and identity. The browser is standalone HTML
with clip/frame selection, 42 raw/normalized/weight channels and trajectory/joint plots.
The formal source has 12 explicit members; the current output has 1261 frames.
Rebuilding unchanged inputs produces the same identity and payload digest.
Input bytes/schema/member order invalidate the cache; corrupted payloads are rebuilt.
P4's build/browser tools do not execute runtime search or blending. P5 binds the
same database statistics to runtime queries. Optional static-support foot
grounding defaults off; full UE FootPlacement, inertialization and stride
warping remain unimplemented.
Space, velocity, loop extension, history and prediction limits are specified in
[the feature contract](../../documents/contracts/animation-motion-features.md).

```powershell
& "C:/Program Files/Python314/python.exe" tool/animation/capture_gasp_p4.py `
  --resources ../VukanLearnResources --output artifacts/new-p4-captures `
  --dotnet artifacts/dotnet-sdk/dotnet.exe
```

Capture requires a .NET 8 SDK (a runtime-only PATH dotnet is insufficient),
numpy/Pillow and a C# enabled build. The local regular CPython installation above
supports these packages; the free-threaded `py -3` environment currently cannot import numpy.
The isolated fixture runs circular C# input through Jolt, captures one/two-thread overlays,
restores config bytes in finally and terminates its own processes.
It does not prove manual input or graceful teardown.

## P5 Motion Matching Acceptance

```powershell
& "C:/Program Files/Python314/python.exe" tool/animation/capture_gasp_p5.py `
  --resources ../VukanLearnResources --output artifacts/new-p5-captures `
  --build build-vscode/development --dotnet artifacts/dotnet-sdk/dotnet.exe
```

Build the C# enabled executable and bridge first. The tool compiles a separate
fixture assembly, creates scenes only below `Generated/Validation/<output-name>/`,
and runs each idle/start/walk/stop/reverse/run/jump/land input scenario in a new
process. It uses the production `Animation.Match` API and Character/Jolt body.
The fixture only restricts allowed clips and action locks; it does not author a
query vector, choose a frame, or reproduce search logic.
It is an independent controller fixture, not execution of the formal player
controller or a manual keyboard test. Movement starts only after the real
native history/query is ready, so initial shader/environment stalls cannot
silently skip the start or prewalk input phase.
The fixture camera follows actual character translation with a fixed world
viewing direction, keeping the character left of the candidate table even
when a failed selection delays the screenshot.
Like the formal controller, grounded actual horizontal speed below 2 cm/s
uses Idle/Stop candidates; actual movement retains the full six/seven-member
Walk/Run families. Airborne and landing states take priority.

Each case writes native feedback to `.trace.json`: database identity, query
readiness, selected clip/time, decision, search count, playback clock, allowed
set and achieved physical position/velocity. The screenshot is requested through
the production Console after feedback reaches the requested stage; the report
also records the last observed feedback and elapsed game time since that trigger.
Managed feedback is delivered after the native evaluation it describes, and RT
captures a frozen render snapshot. The adjacent trace therefore cannot prove
that the image shows that exact clip/frame; inspect the image's Selected row
separately rather than treating log proximity as exact screenshot pairing.
`--pause-animation-before-capture` sends the existing `animation pause` Console
command immediately before `screenshot`. This freezes the native player/matcher
for a more stable table capture; Character/Jolt continues, and it does not make
the managed feedback and captured frame an exact pair.
The native Motion overlay carries query/selection/top candidates and cost terms;
the managed trace contains the finite summary, not a separate candidate table.
The report separates capture completion from behavior acceptance. Start must
actually select Walk_Start, Stop must select Walk_Stop, Reverse must select a
left/right Turn, and Walk/Run/Jump/Land must select the corresponding family
within that input stage. A selected clip merely belonging to the allowed set
does not pass. Missing actions retain a fallback screenshot and trace, continue
to later cases, and produce an unsuccessful overall exit. Candidate sets stay
the same as the formal controller rather than being narrowed to force a result.
Walk additionally waits at least four gameplay seconds and requires 0.75
continuous gameplay seconds of the native WalkLoop selection. Its failure
deadline is six seconds, allowing the authored nonlooping Start tail to finish;
an earlier occasional loop does not satisfy this steady movement check.
`--stages blocked --threads 1,2 --sizes 1280x720` adds an isolated static wall
and keeps requesting 200 cm/s into it. Acceptance waits four gameplay seconds
and requires one continuous second of achieved horizontal speed below 2 cm/s
with native Idle selected. The trace records desired/achieved velocity and a
derived physical blockage observation. The native query's `blocked` flag and
red frozen future positions remain in the production overlay/module evidence;
the finite CLR feedback does not expose them.

The default matrix covers one/two render threads at 1280x720 for every scenario,
plus Walk table layouts at 1920x1080 and 900x700. Use `--stages walk --threads 1
--sizes 1280x720` for a smaller capture. Input timings use gameplay seconds;
variable render frame timing can change sampled results, so these screenshots
do not establish frame-rate-independent selection determinism. The pure native
module tests own exact cost and stable-tie validation.

Requires Windows console injection, Pillow, a .NET 8 SDK and matching native/
managed binaries. The injector runs in an isolated helper process because
AttachConsole changes standard-handle state. No source UE files or formal Maps
assets are written. Config bytes are restored in `finally`, and every owned
engine process is terminated and waited after capture. Forced termination is
reported explicitly and does not count as graceful teardown or manual input.

For the P6 steady cadence comparison, add `--match-speed` to enable native
speed matching in this same fixture. The default remains disabled. The option
passes through the scene's `MatchSpeed` property and production `Animation.Match`;
trace records the native playback rate and speed-matching diagnostics. Both
Run comparison modes require 0.75 continuous seconds of a moving loop,
with a six-second deadline. Use only Run at one size/thread count for a focused
comparison; these images do not measure stance-foot drift or establish exact
feedback/render-frame pairing.
