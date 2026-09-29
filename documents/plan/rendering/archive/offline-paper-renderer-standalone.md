# Offline Paper Renderer (Historical)

This is the archived record of the retired standalone paper-image CLI and its
fixed-case studies. Its paths, commands, baselines and outputs are historical
evidence, not current build or implementation instructions. The active VL
contract is `documents/architecture/vl-offline-rendering.md`.

At the time of the archived implementation, `tool/offline-renderer/` held a
standalone C++17/CMake CLI inside the VulkanLearn repository.
`vl_offline_core` was built as a pure CPU static library in both standalone and
root builds, while `vl_offline_legacy_cases` owned the fixed OBJ/PFM inputs
and paper presets. The root executable consumed the core through
`source/render/offline/`: World candidate preparation freezes imported mesh
sections, effective MI values, model profiles and texture bytes. A CPU prepare
worker builds shared BVHs; the progressive session publishes immutable,
versioned linear films; a Vulkan display branch uploads them per acquired
swapchain image and composites the existing UI. The standalone CLI did not
include `source/`, engine materials, the RenderGraph, GBuffer, shader build
cache, or runtime scene assets. Its source and fixed-case loader were removed
when it was retired on 2026-09-29. Ignored local output directories also
contain VL integration evidence and were retained pending a selective evidence
review. Integration did not add engine runtime test flags or occupy a
`ShadingModelID`.

The active World/graph transaction preflights the selected path and publishes
its frozen CPU package with the World; a failed candidate leaves the prior
World intact. Offline-only material definitions need no fabricated GPU shader.
Realtime switching prepares GPU resources from the same frozen imports when
needed; unsupported realtime models and the offline square emitter reject
the switch. Successful material preview Apply publishes a new frozen package;
workers never read the editor document or live MI. Session generations
invalidate superseded prepare/results. Color material images use the engine's
single vertical row flip and LOD0 sampling; signed RG PFM groom uses its
separate repeat/linear direction convention. External asset digests and
effective values appear in the export manifest; replay still needs the
original asset versions.

VL perspective primary rays respect the camera near/far planes in forward
depth, while secondary paths and the standalone CLI camera remain unchanged.
Changing either plane invalidates the progressive film. The export's
`inputDigest` covers frozen mesh content digests as well as scene, camera,
material, profile, texture, and integrator inputs; task and display metadata
are recorded separately.

The selected-path preflight decodes referenced frozen color and groom bytes
before a World or material-preview commit, so malformed images reject the
candidate with its material/object identity. Triangle conversion and BVH
construction remain cancelable CPU worker tasks. This keeps committed
effective input renderable, at the cost of synchronous decode work at the
commit boundary; large-texture latency is not yet characterized.

This integration is a bounded capability, not a general CPU implementation
of arbitrary GLSL materials, environment lighting, terrain or animation.
The controlled shared Lambert scene lives in the configured resource
repository at `Maps/SC_offline_shared/`; remaining end-to-end interaction
checks are tracked in the integration plan.

## Current Scope

- `cloth-a-roughness` traces primary rays into five spheres. Every surface hit
  samples the rectangular emitter with a shadow ray. `--bounces 0` is the
  original direct-only image; `--bounces 1..8` continues sampled surface paths
  and makes color bleeding observable. There is no environment light.
- `cloth-a-fig1` uses the authored OBJ's `Plane.001` drape, fan-triangulates its
  quads and builds a median-split BVH once. Five translation-only instances
  share the BVH and take the paper's roughness sequence. The cloth triangles
  and normals are rotated 90 degrees about Y before BVH construction so the
  opening faces image left and pooled fold falls to image right, matching the
  supplied clay reference. The OBJ stays unchanged; `--fig1-mesh-yaw` records
  alternative Fig. 1-only orientations. The OBJ floor and
  `Cube` support, plus the absent MTL, are intentionally ignored; this case
  supplies its own floor, light and material. The floor ends behind the drape to expose a camera-only
  neutral backdrop; it is not an environmental light. Smooth vertex normals
  shade the thin two-sided cloth, while geometric normals offset shadow and
  continuation rays and enforce same-side opaque transport. A fixed
  directional key uses one visibility ray per hit; a low-energy rectangular
  fill is sampled with visibility to give the ground shadow a soft penumbra.
  Both lights and the figure-fit material RGB are project-owned, not published
  scene specifications. The Fig. 1 key direction `(-0.7,0.5,1.25)` was chosen
  by comparing the floor-shadow position in the figure crop. Command-line
  `--fig1-key` and `--fig1-irradiance` allow recorded, Fig. 1-only direction
  sweeps. `--light-samples` controls fill sampling for Fig. 1.
  `--softening on|off` switches only the
  light-side Eq. (4) term; the directional-albedo table stays unsoftened.
- `cloth-a-fig4` reuses that mesh and BVH for one roughness-0.40 close-up with
  a more grazing light direction. Its on/off runs hold camera, scene and per-pixel
  RNG seeds fixed; it isolates Eq. (4)'s terminator behavior, not a claim that
  the paper published exact Fig. 4 scene parameters.
- Six `cloth-b-*` cases (`linen`, `silk-crepe`, `satin-front`, `satin-back`,
  `silk-shot`, `velvet`) default to `Plane.001` and the 90-degree Fig. 1 orientation
  for one close-up drape under identical camera and directional lighting.
  Unlike A, they derive orthogonal yarn axes from the OBJ UVs and evaluate
  Sadeghi et al.'s microcylinder surface/volume response with per-family
  2013 journal Table II values and piecewise-linear tangent curves, masking and
  projected-length normalization across both yarn families (Eq. 2-15).
  Each curve defaults to 32 equal-distance midpoint samples; that
  placement is an implementation convention, not a published sample offset.
  Zero-length segments are skipped, not interpolated. Journal Table II resolves
  the old report ambiguity: Satin-back lengths are
  `{1.33,1.33,1.33,0,0.67,0.67,0.67}`, with a middle discontinuity, not an
  inferred last length. Linen offsets are `{-25,25}`. Velvet supports a user-authored
  spatial direction map; the original paper texture remains unavailable.
  Eq. 3's dye multiplies the whole volume term. Eq. 15 normalizes by
  `Q=a1*mean(P1)+a2*mean(P2)+(1-a1-a2)*NoV`. Uncovered area contributes
  projected area to Q, not Lambert reflection; explicit pore transmission is not modeled.
  Missing/degenerate UVs fail fast.
  The camera, default directional key, floor and thin two-sided geometry are
  controlled scene choices, not published scene metadata. `--bounces 0..8`
  continues sampled surface paths with the same Eq. 14-15 kernel divided by
  its full-support mixture PDF (half uniform sphere, half longitudinal
  Gaussian scattering-cone proposals over both yarn families, tangent
  samples and surface/volume widths). The cone angular Gaussian is truncated
  to the longitudinal range and converted to solid angle with its Jacobian.
  There is no extra macro cosine on the yarn. Floor continuation uses Lambert
  with cosine sampling; ray origins follow the outgoing geometric side.
  Its indirect AOV records light reached after at least one surface bounce.
- B-arm `--b-scene cylinder-drape` uses the user's cloth-only
  `assets/fabric_cylinder.obj` at yaw 0; `--b-scene cylinder` uses a closed
  analytic cylinder with axial U and circumferential V. Both keep material
  evaluation, groom handling and transport shared with `drape`.
  Only the analytic body gives exact axial/circumferential/diagonal frames.
  CLI 0/90/45 follows these names; paper Fig. 16 labels them 90/0/45.
  `--curve-samples 8|16|32|64` changes evaluation and proposal PDF together.
- Estévez & Kulla 2017 Eq. (2)-(4) supply Charlie D, fitted Lambda, and
  light-side terminator softening. The base uses the unsoftened directional
  albedo at both light/view directions. The paper's roughness values are used
  directly, without squaring.
- The 16x16 unsoftened albedo table is computed at startup with deterministic
  uniform-solid-angle midpoint quadrature. Its axes cover cos(80 degrees)..1
  and sqrt(roughness). This renderer is not an independent numerical oracle for
  its own table.
- A-arm cloth continuation samples a 50/50 cosine/uniform hemisphere mixture and
  weights the full BRDF by cosine over the mixture PDF; the floor uses cosine
  sampling. B-arm yarn continuation instead uses its full-sphere mixture
  with the Eq. 14-15 radiance kernel (including its longitudinal cosine)
  divided by that mixture PDF. Russian roulette starts after the third surface hit. The sphere
  case explicitly samples a non-intersectable area emitter; directional cloth
  cases use one delta-direction shadow test per hit. Those baselines have no
  overlapping light estimator. B-arm area mode separately implements MIS as
  described below. No mode adds environment radiance.
- Each render writes linear RGB PFM (measurements), tone-mapped sRGB PPM
  (display only), direct/indirect AOV pairs, and a settings manifest. Only
  the indirect PPM uses a 4x display exposure; all PFMs remain linear and
  unscaled. Outputs belong under ignored `artifacts/offline-renderer/`.
- `--groom-map` loads signed UV direction vectors from a linear RGB PFM
  (both byte orders, repeat addressing and bilinear vector filtering).
  Triangle barycentrics carry UVs to each hit; the same rotated yarn frame
  is used by direct lighting, continuation and proposal PDF evaluation.
  Exactly cancelling antipodal vectors choose the nearest texel.
  `--yarn-rotation` adds a B-arm-only constant rotation; no scene/light
  rotation is implied. Groom maps are authored inputs, not reconstructed
  paper data. Invalid/missing/non-finite maps fail before image writes.
- `--threads 1..64` distributes rows among standard C++ workers. The default
  is at most eight hardware threads. Each pixel retains its own fixed RNG
  and serial sample accumulation, preserving output across worker counts.
  The standalone target links only the standard Threads dependency.

## Numerical And Image Validation

- `reference_test.cpp`: normal-incidence closed-form checks for black and
  colored dye at three `kd` values, plus 72 independent scalar Eq. 2-15
  probes over the six published presets. The scalar reference uses projection
  ratios rather than production angular helpers. It shares input parameters,
  not the kernel implementation; it is still not measured-BRDF acceptance.
  Unequal family sample counts additionally guard the per-family mean in Q.
- `sampling_test.cpp`: all six materials at normal-ish and grazing views,
  96x192 versus 192x384 sphere quadrature, PDF normalization, and 150,000
  samples per combination. MC error is checked against estimated standard
  error and grid-refinement error; lower-hemisphere support is checked.
- CTest also covers groom interpolation/UVs/rotation covariance and rejects
  invalid CLI values. `verify_outputs.py` runs black-box tests for both
  PFM byte orders, invalid maps, constant and spatial maps, thread-count
  identity, direct/indirect separation, and both fixed A-arm hashes.
- `render_study.py` generates six-material and authored-groom figures, yarn
  orientation views and paired spp/depth studies with unscaled PFM metrics.
  The renderer itself has no Python dependencies; verification uses Python 3
  stdlib, and the contact-sheet generator additionally requires Pillow.
- `cylinder_test.cpp` validates intersections, tangent frames, finite profiles
  at 8/16/32/64 samples, Linen symmetry and the horizontal 2/3/4-peak structures
  of Crepe / Satin front / Satin back. `render_cylinder_study.py` emits CSV
  profiles, six-material three-direction images on both geometries, numerical
  versus image-body comparisons and relative discretization RMSE. Profile
  predictions share the production kernel; this is a render-chain regression,
  not independent measured-data acceptance.
- `lighting_test.cpp` checks oriented square-light intersections, emitting side,
  PDF Jacobian, occlusion and emitter double-counting. Light-only and MIS
  integrators are checked against two area-quadrature resolutions for Lambert,
  all six fabrics and a below-macro-normal Velvet probe. The quadrature shares
  the separately tested yarn kernel but not the light sampling estimator.

The sphere sweep and draped-cloth case use controlled project-owned geometry,
light, camera and colors. The cloth scene checks the paper's Fig. 1 roughness
progression with a figure-like crop and color fit. The September 27 mesh
revision extends the folds and pools the hem toward the camera, but its
silhouette and pile-up still differ from the paper's geometry.
The close-up supports Fig. 4 softening on/off.
Neither is a pixel-exact reproduction or completed acceptance of the paper's
unpublished scene. The fixed OBJ belongs under `tool/offline-renderer/assets/`;
its SHA-256 is
`CDD1D66BAE1D58230F2C8E7290565AC042C8C8BCB282E9F9DD5122BA1578AEC3`.
Render outputs and visual comparisons belong under ignored
`artifacts/offline-renderer/`. `tool/offline-renderer/README.md` gives the
reproducible commands and input limitations.

## Fixed Preview Evidence

The previous `cloth-a-fig1-v2` / `cloth-a-fig4-v2` previews used a
project-owned area emitter. Their 80x amplified Fig. 4 difference PNG and
`0.01126` maximum linear-channel difference are historical *area-light*
evidence, not directional-light evidence. The current `cloth-a-fig1-sun`
preview at 800x260, 16 spp and two bounces produced finite PFM pixels. Its
maximum linear-channel residual of `combined - direct - indirect` was
`5.96e-8` and mean indirect channel value was `0.01537`.
`cloth-a-fig4-sun-on/off` at 700x250, 16 spp and zero bounces shared the
same seed and geometry; disabling Eq. (4) changed 27,025 pixels above a
`1e-6` linear-channel threshold, with maximum absolute difference `0.10144`.
The 20x amplified difference PNG is only a diagnostic, not actual radiance.
Neither comparison claims the paper's unreported light settings.

The 96x32, 1 spp, one-light-sample, zero-bounce sphere baseline retained
the exact PFM SHA-256
`9EA81056F06EBCED3D8057607EB865425272F1C9E39976F23A1456C51067A5BB`
after mesh integration. The independent A-arm numerical reference
`verify_cloth_sheen_albedo.py --quick` passed 24/24 checks. These are
fixed-preview and formula checks, not a convergence study or pixel-exact
paper figure acceptance.

The earlier-mesh `cloth-a-fig1-paper-fit` comparison uses the paper's Fig. 1
crop at 816x193 alongside the same-size 64 spp render with two surface
bounces. Five sampled top pixels in the paper progress approximately
`(104,2,0)` to `(143,53,53)` in display RGB; the fitted render goes from
`(110,12,12)` to `(149,60,60)`. These are illustrative crop samples, not an
image metric or a claim about published linear RGB. The 64 spp PFM contains
only finite values; the maximum channel residual of combined minus direct
minus indirect is `7.45e-8`. The sphere baseline hash above was unchanged
after the figure-fit light, material, camera and same-side transport edits;
the independent 24/24 A-arm check also passed. That mesh's hem silhouette, fold
length, front pooling and inter-instance overlap differed noticeably from
the published image. The paper does not disclose
the exact scene or lighting, so RGB fitting cannot establish physical identity.

That fixed preview was produced before the later shadow-direction correction.
At image x=800, its direct-light shadow starts near y=125, whereas the paper
crop starts near y=95. With the current default key, an otherwise identical
direct-only sweep starts near y=100 and ends near y=150; the paper's visible
range is approximately y=95..155. The more front-facing light required a
separate red-cloth RGB refit. These pixel positions are crop-specific,
geometry-dependent evidence, not a recovered world-space light vector.

The earlier-mesh `cloth-a-fig1-paper-key-fit` render is 816x193, 64 spp and two
bounces with the then-current camera and OBJ. Its manifest records normalized
`to_light=(-0.461316,0.329511,0.823778)` and key irradiance `6.85`.
At x=800 its direct shadow runs approximately y=100..150. Five sampled
top display-RGB pixels progress `(106,16,16)` to `(154,51,51)` versus the
paper crop's `(104,2,0)` to `(143,53,53)`. All combined and AOV PFM pixels
are finite, and `max(abs(combined-direct-indirect))` is `8.94e-8`.
The unchanged sphere PFM SHA-256 remains
`9EA81056F06EBCED3D8057607EB865425272F1C9E39976F23A1456C51067A5BB`.
This is a closer projected shadow direction, not proof of the actual light;
front-face illumination and the old short hem still differ visibly.

On September 27, the authored OBJ was updated without changing its
`Plane.001` topology (65,536 quads, 131,072 triangles). Its local bounds
changed from approximately `x[-0.477,0.523], y[0.008,0.957],
z[-0.482,0.480]` to `x[-0.695,0.707], y[0.018,1.316],
z[-0.654,0.567]`. The revised export also includes an ignored `Cube`
support object. Fig. 1 instance spacing increased from 1.05 to 1.40;
the camera target height increased from 0.44 to 0.60 and orthographic
width from 5.7 to 7.6. Fig. 4 retains its material and light, but its
close-up camera moved from `(0,1.35,4) -> (0,0.6,0), width 1.6`
to `(0,1.8,4) -> (0,1.06,0), width 1.9` so the taller top is visible.
`cloth-a-updated-fabric` is the new 816x193, 32 spp, two-bounce render
under ignored `artifacts/offline-renderer/`, with a three-row paper /
previous mesh / updated mesh comparison PNG. All combined and AOV PFM
pixels are finite, their maximum additive residual is `7.45e-8`,
and the zero-bounce sphere baseline retains the hash above. The new
pooled front hem and longer vertical folds improve the silhouette;
the shoulder width and floor pile-up still differ from the paper, and
the fixed lighting has not been re-fitted to this new geometry.
At 350x125, 4 spp and zero bounces, Fig. 4 on/off PFMs are finite and
their maximum linear-channel difference is `0.28847`; this is a smoke
check for the updated crop, not a converged paper comparison.

The first updated-mesh preview above was at yaw 0 and remains historical.
A subsequent orientation sweep at 0, 45, 90, 135 and 180 degrees against
the supplied clay reference selected yaw 90 based on the image-left opening
and image-right pooled fold. This is an authored view/orientation choice,
not a recovered paper camera or light direction. Fig. 4 retains yaw 0.
The current `cloth-a-reference-yaw` 816x193 render uses yaw 90, 32 spp
and two bounces. Its manifest records `mesh_yaw_degrees=90` and the
unchanged directional key `(-0.461316,0.329511,0.823778)`. The
combined/direct/indirect PFM pixels are finite and their maximum
additive residual is `5.96e-8`; the sphere baseline remains
`9EA81056F06EBCED3D8057607EB865425272F1C9E39976F23A1456C51067A5BB`.
The right-hand fold and left-hand opening now agree with the clay
reference in silhouette. The reference's visible gray support is
still omitted, and neither the paper lighting nor material was re-fit
after the mesh orientation change.

The following B-arm previews are historical transport evidence only.
They predate the correction that applies Eq. 3 dye to the whole volume term
and removes the undocumented Lambert pore fill. Their numeric integrals and
images must not be used as current material references.

The pre-preset `cloth-b-linen-bounce2-96spp` preview uses the same fixed mesh and yaw,
one instance, 700x600, 96 spp and two bounces. Combined, direct and indirect
PFMs are finite. The indirect linear-channel mean is `0.0079381`, close to
`0.0079436` at 32 spp; the maximum combined/direct/indirect additive
residual is `5.78e-8`. The 280x240, 8 spp paired zero- and two-bounce runs
have bit-identical direct PFMs, while the two-bounce indirect mean is
`0.0079612` and nonzero in the cloth-center crop. An 8-bounce 96x96 smoke
render is finite. The 96x32 sphere baseline retained
`9EA81056F06EBCED3D8057607EB865425272F1C9E39976F23A1456C51067A5BB`,
and the A-arm Fig. 1 smoke PFM hash stayed identical before and after the
B-arm path change. The earlier uniform-sphere `offline_sampling` run compared
96x192 full-sphere quadrature `(0.523673,0.632029,0.668148)` with 200,000
mixture samples `(0.522248,0.630439,0.666502)` at a fixed view. The current
cone-proposal test retains that quadrature and checks its PDF/measure,
lower-hemisphere support, all six material curves and finite scattering
values. This checks sampling and visible transport, not the paper's
unpublished illumination, cloth geometry or RGB values.

The six `cloth-b-cone32-*` previews use the same 90-degree OBJ, camera,
directional key and floor at 320x270, 32 spp and one surface bounce. All
combined, direct and indirect PFMs contain finite values, with maximum
`abs(combined-direct-indirect)` no greater than `1.2e-7`; their direct AOVs
are bit-identical to the matched 32-spp `cloth-b-compare32-*` runs made
before the cone sampler change. The narrow-lobe
indirect images, especially Velvet, are visibly noisy at this sample count.
No radiance clamp or artificial fill was applied. The current 50,000-sample
Linen cone-proposal test estimates `(0.519010,0.627410,0.663544)` against
full-sphere midpoint quadrature `(0.523673,0.632029,0.668148)`, with
28,873 sampled directions below the macro normal. The test checks the
other five presets for valid curves and finite sampled responses, but does
not integrate each of them against separate quadrature references. After
the six-preset change, the 96x32 zero-bounce sphere and Fig. 1 PFM hashes
remain `9EA81056F06EBCED3D8057607EB865425272F1C9E39976F23A1456C51067A5BB`
and `42D27640EC5622914629FA758485D46FFEBC625C422126C733F44B5C95E26BD0`,
respectively.

## Historical Report-Version Evidence

The following `b-study/` results predate the 2013 journal Q, Linen ramp,
Satin-back discontinuity and 32-sample corrections. They remain historical
transport evidence, not current material baselines.

The corrected `b-study/` comparison uses 384x324, 512 spp and four surface
bounces for all six materials under the same scene. Combined/direct/indirect
PFMs are finite; the largest additive residual is below `6.0e-8`.
Whole-frame indirect RGB means range from `0.0014864` (Satin front) to
`0.0029493` (Velvet). The supplementary 160x135 study compares 64 and 256 spp
to a paired 1024-spp reference: indirect-image RMSE at 256 spp is
`0.422..0.660` times the 64-spp RMSE across the six cases. This is decreasing
sampling error, not proof of per-pixel convergence; Velvet and narrow
specular regions still show residual bright samples at 512 spp.

At 160x135 and 256 spp, increasing depth 4 to 8 changes whole-frame
indirect means by `2.33e-7..3.35e-6`, while direct AOVs stay bit-identical.
This supports depth 4 for the current preview, not arbitrary geometry or
lighting. The 0/45/90-degree Silk Shot study holds the scene fixed and only
rotates the yarn frame. The Velvet groom comparison uses a project-authored
periodic angle field `65deg*sin(2*pi*u)*sin(2*pi*v)`, not a paper asset.
Commands, scene manifests, linear data, contact sheets and numerical
`study.json` are reproducible through `render_study.py`.

The corrected all-six/two-view MC test has maximum per-channel relative
error `0.00635` against the fine quadrature grid; numerical PDF integrals
range from `0.999557` to `1.00003`. All ten CTest cases and ten black-box
check groups pass. Both historical A-arm PFM hashes remain unchanged.

## Journal Source And Cylinder Evidence

The current source is the author's April 2013 ACM TOG 32(2), Article 14 PDF:
`https://datarepo.eng.ucsd.edu/~henrik/papers/practical_microcylinder_appearance_model_for_cloth_rendering.pdf`.
Its SHA-256 is
`06683F6DFABD64390220EAA1FB04650641B7CD8952AC9449EBE87BD5E52260D0`.
Eq. 14-15 is on PDF page 8, Table II and Fig. 16-19 on page 9,
and Fig. 20-24 continue on pages 10-11. Do not confuse it with the
June 2011 report still stored in the knowledge book's older library entry.
The book's asset/citation migration is a separate task.

The second fixed asset is `tool/offline-renderer/assets/fabric_cylinder.obj`,
cloth only (`Plane.001`, 65,536 quads, 66,049 vertices and UVs).
SHA-256: `E5F1E31AFAABF3D351D77CA1D862A549277A5DBB783D29F28E16F8837489A30F`.
The accidentally exported ground and support are excluded; retained cloth
positions, UVs and normals are unchanged. The renderer owns a separate floor.
The first `fabric.obj` asset is unchanged.

`render_cylinder_study.py` provides a reproducible controlled experiment:
all six materials x axial/circumferential/45 degrees on the ideal cylinder,
and all six x UV 0/90/45 on the new drape. Ideal images are direct-only,
160x264 at 16 spp; drapes are 288x192, 128 spp and four bounces. Both use
32 tangent samples per family. `study.json` records exact commands,
linear AOV means/residuals, mesh hash, image/profile RMSE and 8/16/32 versus
64-sample differences; `profiles.csv` retains linear RGB values.

At 64 samples, the controlled horizontal profiles locate Crepe peaks at
approximately -34.75/34.75 degrees, Satin-front peaks at -31.75/0/31.75,
and Satin-back peaks at -30/-4.25/4.25/30. These reproduce the expected
peak structures, not digitized paper measurements or a recovered light.
Our view and light coincide; exact paper camera/light metadata is unavailable.
The cylinder study's drape comparisons use a directional key, whereas journal
Fig. 24 uses a square area light. The area study below adds that emitter type;
exact scene illumination is still not recovered.

The completed 36-image study has maximum image/profile relative RMSE
`0.004792` over the central +/-60-degree body interval. Across all 18
material/orientation curves, maximum relative RGB RMSE against 64 samples is
`0.29236 / 0.10789 / 0.03840` at 8 / 16 / 32 samples respectively.
All PFM/AOV values are finite and nonnegative; maximum
`abs(combined-direct-indirect)` is `1.01e-6`. The 128-spp drapes still show
bright indirect samples, especially Velvet; this is not a converged beauty
render. No radiance clamp or denoiser was used. Sixteen CTest cases and
thirteen black-box check groups pass, including both unchanged A-arm hashes.

## Square-Area-Light Study

`--b-light directional|area` keeps all existing directional and A-arm behavior
as the default and introduces an explicit B-arm square emitter. Defaults are
center `(-2.8,3.8,3.5)`, target `(0,0.5,0)`, side `3`, emitted radiance `8`;
`--b-area-center`, `--b-area-target`, `--b-area-size` and
`--b-area-radiance` record authored alternatives. These are not unpublished
paper-scene measurements. Area options are invalid outside B area mode.

The square has an orthonormal frame and emits on one side. Uniform area samples
use the solid-angle density `r^2/(area*cosLight)`; perfect-square sample counts
use a jittered square stratification. Four light samples are the default.
Both the full-sphere yarn response and Lambert floor are evaluated in their
own measures, with the same visibility geometry and signed yarn ray offsets.
No extra macro cosine is applied to the yarn kernel.

Default `--b-area-estimator mis` combines area samples and one material proposal
at each surface vertex. Power-heuristic weights include `nLight*pLight` and
the material PDF. Because this pair fully estimates emission at that vertex,
continuation rays hitting the emitter terminate without contributing emission
again. Camera-visible light radiance belongs to the direct AOV; its back is
opaque black. `--b-area-estimator light` is the unbiased light-only comparison,
not a different material. No mode clamps radiance or adds ambient fill.

`--seed` is a uint32 stream selector; zero retains previous pixel/sample seeds.
The two A-arm PFM hashes remain unchanged. The new pinned 96x32, 1-spp,
zero-bounce Velvet directional baseline is
`BB411301650DB919DB82B063EDB2FB119CB75A82CDE68107188F539D4FAF7ADC`.
Black-box checks cover zero light, linear radiance scaling, worker/seed
reproducibility and direct-AOV invariance with depth, in addition to the
previous groom/curve/cylinder checks.

`render_area_study.py` owns `artifacts/offline-renderer/b-area-study/`:
six-material 288x192, 512-spp, four-bounce images and AOVs; identical-camera
directional/area and low/high-spp contact sheets; 96x64 convergence measurements
over three streams at 32/128 spp against an independent 1024-spp stream; and
an equal-spp light-only/MIS comparison with timings. RMSE is measured on
unscaled whole-frame PFMs, including the floor. The independent reference is
still noisy; changing spp, lighting, or estimator does not establish paper
appearance agreement or equal-compute efficiency by itself.

Study phases can run independently with `--phase images|convergence`; their
reports are `images.json` and `convergence.json`. Default `all` writes
`study.json`. A completed report carries `status=complete`; a checkpoint is
not a completed study. AOV addition is checked against a magnitude-aware
float32 bound (`4*2^-24` after normalization by the largest term or 1).
This handles HDR output quantization without changing any radiance values;
synthetic tests still reject genuinely mismatched sums and negative values.

The completed area study's six 288x192, 512-spp images use four area samples,
one material proposal and four bounces. The 18 image-stage configurations
(six each for directional 64-spp, area 64-spp, area 512-spp) have finite,
nonnegative linear AOVs; their maximum relative additive residual is
`1.05e-7`. The figure contact sheets include separate direct and indirect
AOVs. Indirect-only display PPM uses 4x exposure; the composite and PFMs do
not. Bright isolated samples remain on Crepe, Satin and Velvet at 512 spp.
The authored emitter and camera differ visibly from journal Fig. 24 and are
not a pixel-exact reproduction.

At 96x64, three independent seed streams per budget give 128/32-spp
whole-frame indirect RMSE ratios of `0.514` (Linen), `0.264` (Crepe), `0.463`
(Satin front), `0.340` (Satin back), `0.484` (Silk Shot) and `0.437` (Velvet),
each versus a distinct 1024-spp seed. These ratios demonstrate noise
reduction at this budget, not convergence to an exact transport solution.
At equal 128 spp, comparing one same-seed light-only run to the MIS reference
gives indirect RMSE pairs `light / MIS` of `0.00324 / 0.00327`, `0.02311 /
0.02392`, `0.04594 / 0.04469`, `0.04002 / 0.02098`, `0.04457 / 0.01835` and
`0.06438 / 0.07712` in the same material order. MIS helps notably for Satin
back and Silk Shot, not uniformly for Linen, Crepe or Velvet. It costs more
per sample; these comparisons do not establish equal-time superiority.

Twenty-seven CTest cases and nineteen black-box groups pass, including the
unchanged A-arm hashes and B directional Velvet hash. The original cloth
asset SHA-256 remains `CDD1D66BAE1D58230F2C8E7290565AC042C8C8BCB282E9F9DD5122BA1578AEC3`;
the clean cylinder drape remains
`E5F1E31AFAABF3D351D77CA1D862A549277A5DBB783D29F28E16F8837489A30F`.

## Reproduction Acceptance

The current implementation covers the six parameterized materials,
curve evaluation, macro-surface indirect paths, spatial direction input and
reproducible numerical/image checks. It is not yet a complete reproduction
of the paper's experiments. Remaining acceptance gaps are:

- The original Velvet groom map, measured BRDF samples and exact scene
  metadata are unavailable.
- The cylinder peak counts and model/image consistency are checked; measured
  RGB curves and pixel-exact paper images have not been quantitatively matched.
- Midpoint curves remain a discretization choice. The 8/16/32/64 sweep measures
  sensitivity but does not prove that 64 is exact or globally converged.
- The two additional imaginary fabrics in journal Fig. 24 do not have complete
  presets in Table II; this implementation does not invent paper parameters.
- Macro-surface bounces are not a simulation of pore transmission or
  microscopic multiple scattering between yarns, which the paper itself
  leaves outside the model.

## Evidence Ownership

`tool/validation/verify_cloth_sheen_albedo.py` remains the separately
implemented numerical oracle for `S-03`, `S-04`, `S-10`, `S-27`, `S-32`, and
`S-33`. Do not move or remove that script, `M_brdfPlot`, or historical evidence
until each old case has an equivalent result and provenance recorded here.
The offline renderer is a research instrument; any later VL forward/deferred
integration must be tested separately inside the engine.

Next milestones are quantitative A-arm image measurements and B-arm
curve/BRDF validation against measured paper plots and refinement of the authored
Fig. 24 area-light proxy. The cylinder and UV-drape presets compare material response on controlled
scenes, but cannot establish a measured-material image match; Velvet's original
spatial groom texture and scene metadata still need primary evidence.
The B-arm continuation samples the same direct-light kernel on the whole
sphere with an explicit PDF. Independent formula checks now supplement the
sampler/quadrature comparisons; neither proves that the paper's unpublished
scene and indirect lighting have been reproduced. Manifests now identify the
verified 2013 journal source. The existing B-arm `E_fabric` hemisphere LUT is
a runtime approximation, not the offline renderer's full-domain curve kernel.
