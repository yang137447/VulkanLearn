# Hair semantic authoring interface

Status: implemented configuration checkpoint; runtime solver acceptance remains owned by the main thread.

JSON selects `solver: "tressfx" | "cosseratXpbdV1" | "cosseratImplicitV3"` and optional `solverProfile: "effectiveBundleV1"`. Missing solver preserves the TressFX baseline. No aliases or speculative profile choices are accepted. The Implicit V3 tag is separate from the historical XPBD V1 candidate; neither configuration acceptance nor a tag switch releases physical quality.

C++ uses global `TressfxSimulationSolver { Tressfx, CosseratXpbdV1, CosseratImplicitV3 }`, `TressfxSolverProfile { EffectiveBundleV1 }`, and config members `solver / solverProfile`. Runtime Rod configs require GPU; selecting CPU fails rather than running the TressFX reference. GPU layouts belong to the GPU worker. Behavior/group keys distinguish internal algorithm/profile-math versions `1/1` and `3/3`, without exposing those as JSON authoring fields.

CPU owns the shared Effective Bundle v1 definition/export: δ=.01, ζ=.5, radius .0005 m, density 1300 kg/m³, E=1e6 Pa, G=.4e6 Pa. These are engineering effective bundle assumptions, not measured human hair. Configuration exposes no raw SI/stiffness adjustment. Formula provenance lives in `artifacts/hair-landing-20261119/production-formulas.md` and `cpu-gpu-spec.md`.

Rod rejects explicitly authored TressFX damping, windGain, iterations, rootFixedPoints, maxDt, bend/global/local shape and VSP fields in asset/group/placement/live inputs. Common gravity, enabled, backend, binding and collision infrastructure remains available. A placement cannot silently switch raw-authored legacy asset defaults to Rod; use a semantic TF descriptor.

Groups can override solverProfile but cannot choose solver/backend. Resolution remains asset, group, placement. Writer and behavior/group identity include versioned solver/profile. Rod roundtrip omits inactive raw fields; legacy serialization keeps its original fields.

`hair set` retains its numeric single-object protocol. Raw Rod edits fail atomically; solver/profile changes require reload. Blueprint solver/profile controls retain sparse inheritance and existing asset edit permissions. TF standard modify capabilities expose the two text fields and validate candidates before saving. Rod TF numeric controls display only gravity; group raw controls are hidden.

Behavior implementation lives in `source/mesh/validation/meshAssetValidator.cpp`; only TressFX sections are changed. No CMake, resource, GPU simulation, CPU rod or knowledge-book edits belong to this worker.

Tests: `tool/hair-tests/tressfxSemanticAuthoringTests.cpp` and `tressfxSemanticSchemaTests.py`. CPU worker adds the GTest source to CMake and owns the shared build.
