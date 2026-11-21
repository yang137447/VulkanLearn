# VL Offline Rendering

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、测试或`tool/architecture-boundary-audit.ps1` 的门禁规则；不含对未来行为的承诺——那些在 `documents/rationale/`。

VL is the only offline-rendering application and scene authoring entry point.
The former standalone paper-image CLI and its fixed-case asset loader were
retired after the World/MI integration. Their historical study record is
archived under `documents/plan/rendering/archive/`; it is not an implementation
contract.

`vl_offline_core` is a pure C++17 CPU library owned by
`source/render/offline/core/`. It contains geometry/BVH, scattering, sampling,
integration and the progressive session, with no dependency on Vulkan, the
editor, live World or material objects. `source/render/offline/` adapts
immutable World candidates, imported mesh sections, effective material values,
profiles and texture bytes to this core. CPU workers read only frozen inputs;
task generations reject superseded results. The Vulkan display branch uploads
versioned linear films per swapchain image and composites the existing UI.
No offline model consumes a `ShadingModelID` or a fabricated realtime shader.

Paper reproduction starts here, with numerical and paper-scene evidence. A
successful offline case does not automatically become a runtime model or claim
a `ShadingModelID`. Runtime promotion is decided separately: only designated
core models support deferred rendering; other promoted paper models use
forward rendering. A forward-only model must not silently enter the geometry /
deferred path. Deferred promotion additionally requires the GBuffer allocation
and both-path contracts in `documents/contracts/gbuffer-channel-ownership.md`.

The active World/graph transaction preflights the selected path before
publishing its CPU package. Failed candidates and failed preview Apply leave
the old World and effective material version intact. Realtime switching
prepares GPU resources from the same frozen imports and explicitly rejects
offline-only materials or square emitters. Color maps use one CPU row flip and
LOD0 sampling; signed-RG PFM groom retains its distinct directional sampling
rules. The VL perspective camera clips primary hits in forward depth at its
near/far planes, without clipping secondary or shadow rays.

Formal exports are immutable full-film snapshots: Combined, Direct and
Indirect linear PFM, display PPM and a JSON manifest under
`resourcePath/Generated/OfflineRenderer/`. The BLAKE3-256 `inputDigest`
covers the frozen mesh content and the scene, camera, material, profile,
texture and integrator inputs. Task generation, completed samples and display
settings are recorded separately. Partial movement previews never enter
formal accumulation or export. Replaying an export still requires the
captured versions of external assets.

The root `vl_offline_core_tests` and `vl_offline_adapter_tests` /
`vl_offline_coordinator_tests` are GoogleTest suites. VL-only stress and export
checks live under `tool/validation/offline/`; they are external validation
scripts, not engine runtime-test flags. Supported inputs remain bounded to
static meshes, explicit no-environment scenes,
Lambert/ClothSheen/ClothFiber/AshikhminVelvet/AshikhminSatin/AshikhminSlantedVelvet
configurations, one active directional, spot or one-sided square area light, and the
documented texture/groom formats. Unsupported features fail preflight rather
than silently falling back to DefaultLit. Large-texture synchronous preflight
latency and cross-GPU/driver behavior are not yet characterized.

The offline `AshikhminVelvet` closure owns a
per-effective-material numerical table for the paper's inverse-Gaussian
normal distribution, projected-area function `g`, and directional
specular reflectance `R`; the table is frozen with the session input.
It does not share ClothSheen's Charlie distribution or its albedo LUT.
Its MI accepts scalar `sigma`, scalar `f0`, linear diffuse reflectance,
optional `baseColorMap` and linear `normalMap`. The separate realtime
Ashikhmin implementations are described in `cloth-ashikhmin-runtime.md`.

`AshikhminSatin` independently reproduces section 6.3 / Fig. 11's left arm:
the individually normalized cylindrical normal distributions are mixed by
area before computing a single `g`, normal mean and `R`. It uses Eq. (26)'s
uncorrelated visibility, not Velvet's Eq. (14) correlation or a sum of
separately compensated BRDFs. Its frozen `g`/`R` tables use 65 cosine samples
and 33 azimuth samples over a symmetry-folded quadrant. Projected-area
integration uses the cylindrical distribution's analytic transverse integral;
`R` uses 64 x 256 hemisphere quadrature of the same mixed specular kernel.

Satin authoring accepts `u_sigmaFlat` / `u_sigmaEnds` in `[0.05,1]`,
`u_flatWeight` / `u_f0` in `[0,1]`, constant linear `u_baseColor` in `[0,1]`,
and `u_fiberRotationDegrees` in `[-360,360]`. `sigma_y` is fixed at infinity,
not approximated by an arbitrary large number; narrower widths need a new
quadrature-accuracy contract first. UV0 U defines the local fiber x axis,
projected onto the shading tangent plane; y is `n x x`. Rotation is around
the shading normal. By default degenerate UVs fail World/MI
preflight. Offline Satin supports optional
`baseColorMap` and linear `normalMap`; realtime dispatch is a separate path.

`AshikhminSlantedVelvet` is a separate section 6.4 / Fig. 12-13 closure.
The bundle axis, not the side-normal mean, is slanted by 40 degrees relative
to the cloth normal. With UV0 U defining the bundle's horizontal direction,
the cylindrical Gaussian is rotated about local y by `slant - 90` degrees.
Its full rotated support is retained, including normals below the macroscopic
hemisphere; specular evaluation still requires view/light above the cloth.
Only the sides enter density and `g`; the signed mean `<h dot n>` is doubled
to approximate the tops exactly where the paper specifies it. In particular,
that mean is not `2*g(n)`, which uses a positive projection.

The frozen `g`/`R` tables use 65 cosine samples and 65 azimuth samples from
0 to pi. Only y-sign symmetry is folded; x-sign asymmetry is essential.
Projected-area integration uses a 256-point slope quadrature and the analytic
transverse integral of the whole rotated distribution. `R` integrates the same
15-degree Eq. (14) correlated specular kernel with 64 x 256 hemisphere samples,
and Eq. (30) gives the reciprocal diffuse completion.

Slanted Velvet authoring supports `u_sigma` in `[0.35,1]`,
`u_slantDegrees` in `[30,80]`, `u_f0` in `[0,1]`, constant linear
`u_baseColor` in `[0,1]`, and `u_fiberRotationDegrees` in `[-360,360]`.
These width/slant limits keep the weak-direction projection represented by
the finite slope grid, rather than patching zero denominators in the kernel.
Default-paper energy validation applies to sigma=0.5 / slant=40 / f0=0.05;
it is not a claim over all supported parameters. The non-height-field model
can exceed unit reflectance for f0=1, as the paper warns; do not clamp `R` or
silently replace its distribution to make that case pass a white furnace.
Degenerate UVs fail preflight and sessions own immutable material tables.
Offline Slanted Velvet supports optional `baseColorMap` and linear `normalMap`.

## Cloth showcase adapter coverage

`tool/cloth-intake/build_cloth_showcase_stage.py` also generates
`SC_cloth_showcase_<satin|velvet|slantedVelvet>_offline.json`.
The corresponding `<variant>_direct.json` controls retain realtime materials
and the same lamp while disabling environment illumination; they use the
centered browsing controller and are captured with `--realtime-only`.
These use the existing VL `--offline` entry point, frozen World imports and
progressive session. No separate renderer or model loader is involved.
The model TRS, source FBX, wall/floor/flags and SunPatch are shared with
the realtime stage. The camera matches the centered browsing composition.
Only the selected cloth instance is present, without showcase interaction.

These are explicit diagnostic derivatives: environment lighting is disabled,
and wall/floor and noncloth slots use named offline Lambert MIs instead of
DefaultLit. They do not reproduce PBR specular or floor reflections. They are
not whole-image realtime equivalence tests or reproductions of paper figures.
Spot lighting uses candela, centimeter distances, the realtime inverse-square
softening/window and smooth cone attenuation; visibility uses exact finite
shadow rays, without realtime shadow-map bias or filtering. Disabled lights
are ignored; multiple active lights still fail preflight.

Ashikhmin color maps multiply only the diffuse completion; the specular kernel
and numerical tables retain the paper parameters. JPG/JPEG, PNG, TGA, HDR and
EXR color inputs are frozen and decoded with the same single row flip.
Satin and Slanted Velvet can explicitly set `u_useImportedTangents=1`.
That path requires finite, nonzero imported vertex tangents and handedness
instead of deriving the fiber axis from UV differences; absent or invalid
tangents fail preflight. Showcase derivatives opt into this path because their
FBX contains degenerate UV triangles, while preserving the original geometry.
Normal maps use imported tangents when available, otherwise the UV-derived
frame, with handedness; final shading is normalized. This is not a claim of
bit-identical interpolation with the realtime shader. Geometry normals determine
ray offsets and geometric visibility. No fiber geometry or denoiser is added.

`tool/cloth-intake/render_offline_showcase.py` drives VL console commands,
waits for completed samples and exports Combined/Direct/Indirect AOVs plus
the immutable input manifest. `--samples` and `--bounces` control convergence
and transport depth. The film is linear; the offline display/export preview
uses the same ACES fitted coefficients as realtime tone-mapping mode 3,
followed by one sRGB encoding. Exposure is the frozen camera's
`ExposureScale` multiplied by the offline manual multiplier (default 1).
Linear PFM data is unaffected. Historical Reinhard exports keep their
recorded mapping and must not be compared as if their previews used ACES.

`tool/validation/offline/compare_ashikhmin_runtime.py` generates CPU/GPU
diagnostic pairs under `Generated/ClothParity/` from the existing Fig.11/13
scenes. Both sides use EV100 9, compensation 0, 5000 lux, no environment,
zero bounces, constant reflectance and identical geometry/camera transforms.
Offline uses manual multiplier 1; runtime uses ACES mode 3 with Bloom disabled.
Satin/Slanted CPU inputs explicitly use imported tangents to match the GPU's
fiber frame. Four pairs include black/red slanted Velvet. The original four
offline paper assets also use EV100 9; the runtime Velvet preview uses the same
0.002 diffuse reflectance and single light, with its old extra fill removed.
These controlled comparisons do not claim the independently authored preview
assets are identical or that CPU ray shadows equal CSM filtering.
