# Physical Lighting And Exposure

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

The current `radius` field remains a legacy scene field because it is not yet
consumed by the real-time evaluator. It is not a physical attenuation control.

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
- `autoMinEv100` / `autoMaxEv100` bound the exposure scale;
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

The runtime Post Process page exposes Manual / Auto choices, manual EV100,
exposure compensation, and automatic min/max EV100. Manual EV100 changes
brightness only in `ev100` mode; automatic mode uses compensation and its
EV bounds, while physical mode derives EV100 from the authored camera.
Physical-camera scene configurations are categorized as Manual in the UI,
with their derived EV100 shown read-only; selecting Manual explicitly switches
to directly authored EV100. No scene-format or exposure-formula migration is
performed by this UI simplification.
The UI edits the active Camera through validated game-thread commands and
does not write scene assets or read back the GPU's adapted EV.

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
- The unused legacy `radius` field was removed from migrated point and spot
  lights. It must not be interpreted as a physical attenuation radius.

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

These are explicit project-authored migration baselines, not recovered
photometric measurements or paper-scene lighting. Absolute HDRI luminance
and procedural sky multipliers are not calibrated by this asset migration.
