# Rod isolated shader validation entries

These compute entry points are experimental probes or retired numerical
candidates. They are outside runtime shader enumeration. Production includes
remain under shader/glsl/hair; runtime candidate entry is rodV3ImplicitCr.comp.

Compile from the topic code root with the explicit shared include path:

```powershell
glslc -O -I shader/glsl/hair tool/hair/validation/shader/rodV3ImplicitSection.comp -o artifacts/hair-landing-20261119/section-new-identity.spv
spirv-val --target-env vulkan1.0 artifacts/hair-landing-20261119/section-new-identity.spv
```

Use a fresh output identity for each source change. Frozen old source entries,
hashes and all original SPVs/results remain in
artifacts/hair-landing-20261119/experimental-shader-entries-freeze-001 and
the corresponding earlier result directories. Moving an unchanged source
does not change its physics or authorize replacing old result metadata.

rodKkt10AssemblyProbe tests actual CPU contact/edge assembly increments.
rodKkt10LinearProbe tests local rank detection and pivot solves.
rodKktGlobal20Probe tests complete global row/column permutation on a locally
singular, globally invertible two-node constrained system.
None of those probes is a production constrained timestep.
