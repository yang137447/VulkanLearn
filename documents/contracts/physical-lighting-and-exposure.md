# Physical Lighting And Exposure

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、测试或 	ool/architecture-boundary-audit.ps1 的门禁规则；不含对未来行为的承诺 —— 那些在 documents/rationale/ 或 documents/plan/。

This is the current scene-authoring contract for physical light units.

## World Length

`1 world unit = 1 cm`.

Scene positions, camera clipping planes, shadow distances, area-light sizes,
and imported mesh world lengths are authored in centimeters. A scene must
declare `"worldUnit": "cm"`; legacy scenes without that declaration are
rejected instead of being guessed.

Camera flight speed is authored in cm/s. The controller default is 1000 cm/s
(the former 10 meter-like units/s); the runtime UI range is 50..10000 cm/s.
Movement integrates `speed * deltaSeconds` directly into centimeter positions,
without a second runtime unit conversion. Mouse rotation is angular and is
not multiplied by 100.

## Light Units

| Scene object | Required `intensityUnit` | Canonical runtime meaning |
| --- | --- | --- |
| `directionalLight` | `lux` | Perpendicular illuminance |
| `pointLight` | `cd` or `lm` | Candela after optional lumen conversion |
| `spotLight` | `cd` or `lm` | Candela after cone solid-angle conversion |
| `squareAreaLight` | `lm` by convention, `cd` accepted | Emitted radiance after area conversion |

`Unitless` is not supported. Every explicit light must provide
`intensityUnit`; missing or unknown units fail scene preflight.

World snapshots store the canonical point/spot light value as candela.
The GPU upload boundary multiplies only point/spot intensity by `100^2`,
so shaders evaluate attenuation directly from centimeter distances without
converting per-pixel distances to meters. The existing `length` then square
calculation is retained. Directional intensity remains lux; offline inputs
remain canonical.
The existing near-field softening of `1e-4 m^2` is retained as `1 cm^2`:
`I_cd / (r_m^2 + 1e-4) = (I_cd * 10000) / (r_cm^2 + 1)`.
Spot angles are half-angles in degrees in JSON and radians
in the GPU snapshot. The inner cone is fully lit, the outer cone is zero, and
the transition is smooth.

Point and spot `radius` is now consumed by the real-time evaluator as a UE-style
local-light window: the existing `1 / (d^2 + 1)` core is multiplied by
`clamp(1 - (d^2 / r^2)^2, 0, 1)^2`. `radius <= 0` means an unbounded light and
keeps the pre-window behavior. It is an artist attenuation window, not a
photometric quantity.

Optional C# Blueprint light components accept only canonical units: directional
`lux`, point/spot `cd`. They share the existing snapshot and GPU conversion
boundary. Their cone half-angles use degrees in authoring/C# and radians in
snapshots; parent scale affects position, not intensity or cone angles.
Component directional lights do not replace the authored primary CSM sun.
See [C# Blueprint runtime](csharp-blueprint-runtime.md) for fields and lifetime rules.

## Static Point Light Pulse

`pointLight` may declare an optional periodic pulse:

```json
{
    "type": "pointLight",
    "intensity": 25.0,
    "pulse": {
        "period": 1.0,
        "duration": 0.08,
        "phase": 0.0,
        "peakIntensity": 250.0
    }
}
```

`period`, `duration`, `phase`, and `peakIntensity` are required when `pulse`
exists. `period` and `duration` are seconds with `period > 0` and
`0 < duration <= period`; `phase` is a finite second offset and may be
negative; `peakIntensity` is additive candela and must be in
`[0, 1e12]`. Only `pointLight` accepts `pulse`; other light types reject the
field instead of silently ignoring it.

The CPU evaluates the pulse from renderer animation time:

```text
local = wrap(time + phase, period)
factor = local < duration ? (1 - local / duration)^2 : 0
effectiveIntensity = baseIntensity + peakIntensity * factor
```

`baseIntensity` is always present, so the light remains at its authored level
between pulses. The curve starts at the full peak and decays smoothly to zero
at the pulse boundary. The factor is evaluated at the GPU-upload boundary and
only the upload copy is modified; the frozen `LightSnapshot` keeps the authored
base intensity. The point/spot candela-to-centimeter conversion runs after the
effective intensity is computed.

Time comes from `RenderSystem::animationTimeSeconds`, advanced by the same
clamped frame delta used for exposure. It is wall-clock render time, not
simulation particle time: the pulse continues while Play is paused and has no
particle birth/death readback. Use `pulse` for a static scene light that must
flash without a particle system; for effects with a Niagara Light Renderer use
the GPU particle light path below. Editor thumbnails upload static lights with
time 0.

## GPU Particle Lights

A particle emitter with `renderMode: "light"` owns a normal GPU particle pool
(at most 64 particles), advanced by the shared `particleSimulate` compute with
the same fixed-step, burst, shape, gravity/drag, color tint and deterministic
random rules as other emitters. A dedicated `particleLightEmit` compute then
reads the live particles and writes point lights into a separate per-swapchain
`ParticleLightSSBO`; `RenderSystem` no longer calls `ParticleLightRuntime`:

```json
{
    "name": "muzzle_light",
    "modules": [
        { "type": "emitter", "fields": {
            "maxParticles": 2,
            "renderMode": "light",
            "light": { "intensity": 300.0, "radius": 700.0 }
        } }
    ]
}
```

`light.intensity` is candela and `light.radius` is centimeters; both must be
finite and positive. `maxParticles` is capped at 64 per light emitter. All light
emitters share one fixed 64-slot output buffer per swapchain image; slots are
assigned in stable emitter-key order (first-fit), so overflow truncation does
not depend on map iteration order. The shader color/alpha comes from
`EvaluateColorCurve(lifeT)` (author `colorKeys` when present, otherwise the
`colorStart`/`colorEnd` endpoints), multiplied by the instance color tint;
alpha fades intensity, and the final scalar is
`light.intensity * (1 - lifeT) * alpha`. The GPU emits
`intensity * 100^2` to match the existing `LightGPU` centimeter-distance
convention.

The output buffer has a `uvec4 header` (`count` high-water mark, capacity 64)
followed by 32-byte `ParticleLightGPU` entries. The buffer is cleared on the
host every frame after the image fence and before simulation; `particleLightEmit`
writes `atomicMax(header.count, slot + 1)` and unique per-particle slots.
Deferred and forward lighting read the same buffer through set 0 binding 4 and
merge it into the existing point-light loop; there is no second copy in
`RenderScene::lights`. GPU particle lights do not cast shadows; scene point/spot
lights can (see Local Light Shadows). Limits: no light textures; the
offline particle compositor still skips light
emitters. Editor particle thumbnails use the same GPU path when their session
sets the light buffer. See [particle system](particle-system.md) §Light for the
full authoring and shape contract.

## Local Light Shadows

Scene `pointLight` and `spotLight` objects may declare
`"shadow": { "castShadows": true, "bias": 2.0 }` or the shorthand
`"shadow": true` (`bias` is centimeters and optional; the default is 2 cm, and
`castShadows` defaults to false). `radius` is the attenuation range in
centimeters: it defaults to 1000 cm (10 m), must be finite and `> 0`, and the
shader window `clamp(1 - (d^2/r^2)^2, 0, 1)^2` makes the light contribute only
inside that range. Up to eight
shadow-casting local lights are selected per frame by a stable importance key
(camera distance, radius, intensity, then `lightId`). Each selected light owns
one slice of `localShadowMap`, a `D32_SFLOAT` `texture2DArray` with 1024²
slices, so point and spot lights share one atlas type. Local shadow draws are
culled per light: point lights skip casters whose world AABB does not reach the
light radius, and spot lights additionally test the cone; WPO/SpeedTree casters
skip this culling because their authored bounds do not include deformation.
Mesh particle emitters with `castShadows: true` are additional casters in both
the local atlas and CSM; sprite/ribbon particles do not cast.
`LocalShadow` material pipelines are part of the same transactional shader
reload commit as Surface/Shadow:

- Spot: a standard perspective view-projection from the light position,
  direction, outer cone angle (`fov = 2 * coneOuter`), near 1 cm and far = the
  light radius. The shader reconstructs the same matrices from the GPU light
  record and samples with 3×3 PCF.
  The receiver applies a normal offset plus a slope-scaled depth bias
  (`bias * clamp(1 + 1.5*tanθ, 1, 4)`, capped at 4× the authored bias) and
  uses 5×5 PCF; true PCSS is not implemented because the atlas is bound as a
  comparison sampler only. CSM shadow passes cull drawers per cascade with the
  cascade frustum planes (WPO/SpeedTree keep full submission).
- Point: one octahedral slice. Rendering uses eight quadrant passes with
  per-octant world-space projection matrices; the fragment shader rejects
  fragments outside the current octant and writes
  `gl_FragDepth = distance / far`. Sampling encodes the pixel-to-light
  direction with the standard octahedral map and folds PCF taps back across the
  diamond/square seams.

The shader uses set 3 bindings owned by the consuming pass: deferred lighting
binding 15, forward opaque/transparent binding 6, particle binding 2. CSM stays
on its existing `shadowMap` array; a point/spot light without an assigned slice
reports full visibility. GPU particle lights never allocate a local shadow
slice.

## Volumetric Fog

`environment.fog` enables a height-fog / in-scattering pass:

```json
"fog": {
    "enabled": true,
    "density": 0.0003,
    "heightFalloff": 0.35,
    "baseHeight": 0.0,
    "albedo": [0.75, 0.82, 1.0],
    "anisotropy": 0.4,
    "intensity": 1.0
}
```

`density` and `heightFalloff` are per meter; `baseHeight` is centimeters.
The pass order is `particle → particleDistortion → volumetricFog →
fogComposite → editorGrid → autoExposure → bloomPrefilter`. The fog volume is
a quarter-resolution `RGBA16F` `texture2DArray` with 32 exponentially spaced
depth layers. Each layer integrates only the previous segment, so the cost is
O(layers) per froxel; directional light is shadowed by CSM, point/spot by the
local shadow atlas, and GPU particle lights contribute without shadows.
`fogComposite` samples the two neighboring layers by pixel view depth and
composites with premultiplied over into `sceneColor` before Bloom, so Bloom
and auto exposure see the fogged color.

Limits: no temporal denoising (there is no TAA), no froxel light list (all
in-range lights are iterated per froxel), and the offline render path does not
record the fog pass. Fog brightness follows the physical light intensity, so
bright flash lights need very low `density` values (about `0.0001`/m).

## Light Culling

A `lightCulling` compute pass runs after the local shadow passes and before
geometry. It builds a 16×16 screen-tile list for scene directional/point/spot
lights (particle lights stay on their independent SSBO):

- `lightGrid`: `uvec4 header` (overflow tile/light counters) plus one
  `uvec4(count, overflow, 0, 0)` per tile.
- `lightIndices`: fixed `tiles × 64` `uint` slots; overflow truncates in a
  deterministic order and only increments counters.
- `lightGridParams`: tile size, tile counts, render extent, light intervals and
  an `enabled` flag.

Directional lights enter every tile; point lights use a screen-projected sphere
plus view-depth interval; spot lights use the cone's minimal bounding sphere.
The lighting shaders read the tile list when `enabled != 0`, otherwise they
fall back to the existing contiguous header loops. Material/post set 0 uses
bindings 5/6/7; particle set 0 uses 10/11/12. The culling pass itself is not
registered as a shader-reload participant yet, and a tile is capped at 64
entries (no dynamic growth).

## Camera Exposure

Every camera must provide:

```json
{
    "mode": "ev100",
    "ev100": 9.0,
    "aperture": 4.0,
    "shutterSeconds": 0.008,
    "iso": 100.0,
    "compensation": 0.0
}
```

The selected mode is either `ev100` or `physical`. Physical-camera mode derives
EV100 using:

```text
EV100 = log2(aperture^2 / shutterSeconds * 100 / ISO)
```

Both real-time tone mapping and offline display use:

```text
exposureScale = 2^exposureCompensation / (1.2 * 2^EV100)
```

Positive exposure compensation makes the image brighter. Offline film data
remains linear and unexposed; the scale is applied only to display and export.

Real-time cameras may additionally use `"mode": "auto"`. Auto exposure follows
the UE-style eye-adaptation boundary used by this renderer:

- the post-process graph measures scene-linear luminance into a persistent 1x1
  exposure resource;
- the target is `autoTargetGray` (default `0.18`) with EV compensation applied;
- `autoMinEv100` / `autoMaxEv100` bound the exposure scale, with defaults
  `-10` / `20`. Formal scene cameras under `resourcePath/Maps` explicitly
  author these bounds; exposure mode remains independently authored;
- `autoSpeedUp` and `autoSpeedDown` are exponential adaptation rates in 1/s.
- `autoExposureHistogram.comp` measures every scene-color pixel, using 16x16
  workgroups, shared-memory bins, and a 64-bin histogram over log2 luminance
  `[-12, 16]`. Samples have uniform weight; there is no center-weighting mask.
  The lowest and highest 10% are trimmed before solving exposure.
- `autoExposureAdapt.comp` performs a parallel prefix scan and reduction,
  then one temporal adaptation step per rendered frame. Authored speeds are
  used directly, without the former A/B half-rate compensation.

The previous 32x18 sparse fragment metering and same-frame A/B passes are
replaced by GPU compute. Full-pixel metering can change the measured target
relative to sparse sampling; this is not a light-unit or emissive migration.
There is no CPU exposure readback or per-frame device idle. Bloom prefilter
and tone mapping read the same final exposure; manual `ev100` and `physical`
modes skip automatic histogram metering and retain their existing formulas.

`AutoExposureCompute` belongs to the prepared RenderGraph state. Its GPU
float32 history has two slots shared across all swapchain images; temporal
ordering follows submitted real-time frames on the graphics queue, not the
acquired image index. Per-image histogram, parameter buffers, and a 1x1
`R32_SFLOAT` output keep frame resource reuse separate from temporal history.
Explicit barriers cover histogram clear, histogram-to-reduction reads,
cross-frame history reuse, and compute-to-fragment exposure sampling.
New graph state, including resize or World/graph replacement, starts with
invalid history and seeds it from the measured target. Returning from Manual
to Auto also reseeds instead of reading stale or uninitialized history.
Compatible shader reload replaces pipelines and descriptors without resetting
the active history.

This is a UE-style exposure boundary, not an implementation of UE's complete
eye-adaptation algorithm. In particular, temporal adaptation remains
exponential interpolation of linear exposure with rates in 1/s, rather than
an EV-space linear/exponential transition.

The selected Camera's Details contains an Exposure section, with no separate
Post Process tab or viewport override. It exposes Manual / Auto choices, manual EV100,
exposure compensation, automatic min/max EV100, adaptation up/down rates (1/s),
and target gray (linear). Manual EV100 changes
brightness only in `ev100` mode; automatic mode uses compensation and its
EV bounds, while physical mode derives EV100 from the authored camera.
Physical-camera scene configurations are categorized as Manual in the UI,
with their derived EV100 shown read-only; selecting Manual explicitly switches
to directly authored EV100. No scene-format or exposure-formula migration is
performed by this UI simplification.
The UI publishes identity/generation-guarded `SetActorProperty` commands.
The game thread validates each field against the current authored Camera's
complete exposure state. Snapshot refresh does not submit edits or rebuild
mode-dependent controls. Accepted edits enter the scene document draft and
write only the edited exposure field after explicit Save Content confirmation;
untouched physical fields, optional automatic defaults and original numeric
precision are preserved. The UI does not read back the GPU's adapted EV.

## Migration Boundary

The loader does not reinterpret old numeric values. Existing assets must be
migrated to add `worldUnit`, `intensityUnit`, and camera `exposure`, with mesh
and world-length conversions audited together. Assets missing these fields are
intentionally rejected.

## Legacy Scene Migration

On October 9, 2026, the resource scenes under
`D:\YYBWorkSpace\GitHub\VukanLearnResources\Maps` were migrated in place:

- 30 scene assets now declare `worldUnit: "cm"` and explicit camera exposure.
- Legacy scene positions, camera clip distances, area sizes, mesh transforms,
  and directional-light CSM distances were converted from the former
  meter-like authoring scale by multiplying by 100.
- Directional-light authoring values use the fixed migration baseline
  `1 legacy unit = 1000 lux`.
- Point and spot authoring values use the fixed migration baseline
  `1 legacy unit = 1000 cd`; cone angles remain half-angles in degrees.
- Square area lights use the same 1000-fold brightness baseline, then convert
  to lumens with `Phi = pi * (1000 * legacyRadiance) * A_m2`. A former size-2,
  intensity-8 emitter is now 200 cm and approximately 100530.965 lm,
  yielding a canonical emitted value of 8000. Their `intensityUnit` is `lm`.
- Ordinary lit cameras use EV100 9. Unlit `M_brdfPlot` scenes use
  `EV100 = -log2(1.2)`, compensation 0, giving an exposure scale of 1 and
  preserving the authored curve colors. Physical-camera fields are retained,
  but are inactive in `ev100` mode.
- Six legacy HDRI scenes using EV100 9 (`SC_sifi_head`, `SC_scene02`,
  `SC_scene03`, `SC_sculpture`, `SC_sphere_array`, and
  `SC_bistro_exterior_modular`) use a fixed migration baseline of
  `1 legacy environment multiplier = 1000 new multiplier`. Leaving them at
  1 after the exposure migration suppressed both the visible background and
  IBL. The multiplier applies to both through the existing shared environment
  snapshot. Neutral-exposure paper plots keep their old environment values.
  This is a project-authored migration, not an absolute HDRI luminance
  calibration; there is no automatic inverse-exposure compensation.
- Omitted legacy CSM distances are materialized as 30000 cm (formerly the
  300-world-unit default). Bias controls are normalized depth/slope factors,
  not world lengths; they are not multiplied by 100.
- The migrated point and spot lights omit `radius`, which now means an unbounded
  artist window (`radius <= 0`). It must not be interpreted as a physical
  attenuation radius.

`SC_scene01` was migrated from the historical `sunLight` form to a loadable
`directionalLight` with `lux` intensity, centimeter-space transforms, and the
standard camera exposure block. It uses automatic exposure because its Viking
Room is an UE-style Unlit material whose authored texture values are normalized
scene-linear material values rather than calibrated luminance in cd/m^2.
The directional light is intentionally authored at `10 lux`: this scene is a
learning fixture that combines normalized Unlit texture values with a PBR
preview ball, so the historical `1000 lux` migration baseline makes the
preview ball clip while the Unlit room remains correctly exposed. The `10 lux`
value is a scene-fixture balance, not a general physical-light calibration.
Its Viking Room material remains `M_unlit`, with `USE_ALBEDO_MAP` enabled
so the authored texture is sampled. Unlit skips lighting evaluation, not camera
exposure: its current scene-color output still passes through exposure, Bloom,
and tone mapping. Exposure-independent texture display would require an
explicit material policy; it must not be obtained by replacing Unlit with PBR
or multiplying the material by an arbitrary 1000-fold factor. Fixed EV100
scenes that author physical luminance must still calibrate their emissive
values explicitly.
The runtime UI uses lux/centimeter
slider ranges and does not submit float changes during snapshot binding
updates: the old 0..10 sun slider clamped 1000 lux to 10 lux and wrote it back
to the World, making this scene nearly black at EV100 9.

Source OBJ/FBX/SpeedTree files and shared mesh descriptors are unchanged.
The factor of 100 is applied once at the scene instance transform, so imported
vertices and object translations become centimeter world lengths together.
This preserves relative framing, including mirrored instances. The world-space
measurement-grid intervals are now 20 cm and 100 cm.

### Emission And Bloom

Emissive material output is scene-linear radiance and passes through camera
exposure, just like reflected light. The two `SC_sifi_head` material instances
use the same explicit 1000-fold legacy brightness migration as their lights
and HDRI: `u_emissiveStrength` changes from 20 to 20000. This is a local
authored baseline, not measured luminance or automatic exposure compensation.
Other legacy emissive materials still need an individual brightness audit.

Bloom threshold, soft-knee width, and firefly clamp are authored in
**exposed linear space**, before tone mapping. `bloomPrefilter` receives the
same frozen camera exposure scale as `toneMapping` and applies it before
thresholding/clamping. The Bloom pyramid stores the resulting exposed linear
value; `toneMapping` exposes only the raw scene color and then combines it
with that Bloom value, so Bloom is not exposed twice. The prefilter
parameter's `w` channel is runtime-owned exposure, not an artist control;
neutral default 1 preserves neutral-exposure paper plots.
`bloomDownsample` uses a 2x2 Karis average (`1 / (1 + luma)`) instead of a fixed
3x3 kernel: fireflies from sparks/arcs no longer dominate the lowest mip, while
the normalized weights keep the overall exposure level stable.

These are explicit project-authored migration baselines, not recovered
photometric measurements or paper-scene lighting. Absolute HDRI luminance
and procedural sky multipliers are not calibrated by this asset migration.
