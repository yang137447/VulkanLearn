# VL Offline Rendering

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、测试或`tool/ue-lite-boundary-audit.ps1` 的门禁规则；不含对未来行为的承诺——那些在 `documents/rationale/`。

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
configurations, directional or one-sided square area lighting, and the
documented texture/groom formats. Unsupported features fail preflight rather
than silently falling back to DefaultLit. Large-texture synchronous preflight
latency and cross-GPU/driver behavior are not yet characterized.

The paper-specific `AshikhminVelvet` closure is offline-only. It owns a
per-effective-material numerical table for the paper's inverse-Gaussian
normal distribution, projected-area function `g`, and directional
specular reflectance `R`; the table is frozen with the session input.
It does not share ClothSheen's Charlie distribution or its albedo LUT.
Its current MI accepts scalar `sigma`, scalar `f0`, and constant linear
diffuse reflectance only; no velvet texture or runtime dispatch exists.

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
the shading normal. Degenerate UVs fail World/MI preflight. No Satin texture,
runtime shader, GBuffer allocation or `ShadingModelID` dispatch is added.

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
No textured slanted Velvet, runtime ID, shader or GBuffer allocation is added.
