# VL Offline Rendering

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
static meshes, explicit no-environment scenes, Lambert/ClothSheen/ClothFiber
configurations, directional or one-sided square area lighting, and the
documented texture/groom formats. Unsupported features fail preflight rather
than silently falling back to DefaultLit. Large-texture synchronous preflight
latency and cross-GPU/driver behavior are not yet characterized.
