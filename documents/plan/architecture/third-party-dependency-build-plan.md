# Third-Party Dependency Build Plan (Single Machine)

## Status

- Type: build-infrastructure implementation plan (not a contract)
- Created: 2026-11-11; revised after repository verification and independent review
- Base: `codex/ue-runtime-editor-split` (`3b22f70`)
- State: D0-D5 implemented. D0/D1 are in `23a922f`; D2 plus Tracy/NVTX gates,
  MinGW runtime closure and Jolt de-duplication are in `f8ce823`; D3/D4 and the D5
  source cache are in the follow-up working-tree commit
- Review: independent read-only review returned "approve with changes"; B1-B5 folded in
- Related: `documents/plan/architecture/ue-runtime-editor-split-plan.md`,
  `documents/contracts/testing.md`, `CMakePresets.json`, `CMakeLists.txt`,
  `extern/CMakeLists.txt`, `cmake/VulkanLearnJolt.cmake`, `script/CMakeLists.txt`,
  `cmake/VulkanLearnDeps.cmake`, `ThirdParty/CMakeLists.txt`, `tool/build-deps.ps1`

## Problem

A clean project build rebuilds the whole third-party stack, and the sources are
per-worktree, per-build-directory copies.

Measured in this worktree (`build-game`, Release, MinGW Makefiles):

| Location | Object files |
|---|---|
| `build-game/_deps` (third party) | 1067 |
| `build-game/source` (project) | 282 |
| `build-game/extern` | 4 |
| `build-game/script` | 1 |

Per dependency under `_deps`: `sdl3-build` 254, `assimp-build` 221, `rmlui-build` 190,
`openexr-build` 185, `vl_jolt-build` 130, `freetype-build` 43, `vl_ozz-build` 39,
`imath-build` 5. The heavy five (SDL3, Assimp, RmlUi, OpenEXR, Jolt) are 980/1067 = 91.8%
of dependency objects; FreeType takes it to 96%.

Disk and network cost:

- `build-game/_deps`: 548 MB / 4202 files.
- Dependency sources checked out *inside the source tree* by the `extern/*` wrappers:
  `extern/Assimp/Assimp` 234 MB, `extern/SDL3/SDL3` 58 MB, `extern/RmlUi/RmlUi` 19 MB,
  `extern/FreeType/freetype` 12 MB.
- Those directories are gitignored, not vendored. A fresh clone or `git clean -xdf`
  re-downloads them.
- There are 26 worktrees on this machine; each duplicates those sources and its own
  dependency objects.
- Every dependency fetches over the network at configure time: OpenEXR/Imath, Jolt, Ozz,
  SDL3, Assimp, RmlUi, FreeType, Tracy, NVTX and googletest. `build-game` additionally
  pins `FETCHCONTENT_SOURCE_DIR_IMATH`, `..._VL_JOLT` and `..._VL_OZZ` to `build/_deps/...`,
  i.e. it silently depends on a second build directory surviving.

So `rm -rf build` is expensive, and `git clean -xdf` is worse.

## Decision

Three tiers, strictly ordered. Tier 3 is the long-term answer; Tier 1/2 must not depend
on it.

1. **Tier 1: make ccache actually hit.** The cache exists but is misconfigured and full.
2. **Tier 2: scoped project clean.** Stop deleting third-party objects during routine
   cleanup, without breaking generated makefiles.
3. **Tier 3: machine-wide dependency prefix.** Build the heavy dependencies once per
   toolchain/configuration, outside the repository, shared by all worktrees.

No CI artifact registry, no vcpkg/Conan migration, no dependency blobs committed to git.

## Outcome

- D0/D1: ccache is enabled globally with `CCACHE_BASEDIR`, `-fdebug-prefix-map`,
  `ASSIMP_BUILD_USE_CCACHE=OFF`, and a 20 GB cache. `tool/clean-project.ps1` deletes
  objects/artifacts only: a project-scoped clean rebuild recompiles 283 project objects,
  zero dependency objects, and hit 283/283 ccache calls (47.6 s wall clock here).
- D2: `ThirdParty/manifest.json` pins all dependencies and option coupling;
  `ThirdParty/deps.lock` is deterministic; `tool/deps-stamp.ps1` and `tool/deps-gc.ps1`
  verify identity and retain current Debug+Release plus one previous toolchain.
- D3/D4: `tool/build-deps.ps1` installs the six heavy dependencies into
  `D:\YYBWorkSpace\VulkanLearn-deps\<toolchain>\<config>`. Release and Debug prefixes
  both exist. `build-prebuilt`/`build-prebuilt-editor`/`build-prebuilt-debug` consume the
  prefix offline, contain zero heavy-dependency objects, and pass game/editor/debug
  smoke tests, including a `PATH` limited to `System32`.
- D5: Tracy/NVTX fetches are gated by their options, Jolt has a single fetch path, and
  `VULKANLEARN_DEPS_SOURCE_CACHE` lets source mode configure with
  `FETCHCONTENT_FULLY_DISCONNECTED=ON`; the offline configure was verified.

## Tier 1: ccache

### Verified current state

- `ccache 4.14.2` is installed; CMake is not passing a compiler launcher
  (`build-game/compile_commands.json` has 0 `ccache` entries out of 1399).
- Cache: `C:\Users\...\AppData\Local\ccache`, `max_size = 5.0 GiB`, **99.86% full**,
  `3613` cleanups, hit rate **16.36%**, `25821` uncacheable calls out of `98565`.
- `hash_dir = true`; `base_dir` is unset.
- `ASSIMP_BUILD_USE_CCACHE=ON`; `SDL_CCACHE=OFF`.

### What the vendored options actually do

- `extern/Assimp/Assimp/CMakeLists.txt:87-93` sets the **GLOBAL** properties
  `RULE_LAUNCH_COMPILE` and `RULE_LAUNCH_LINK` to ccache. Global means every target defined
  after `add_subdirectory(Assimp)` — including all VulkanLearn targets — is wrapped for
  compile *and link*. Confirmed in
  `build-game/source/CMakeFiles/main_game.dir/link.txt:1`: `cmake -E rm` and `ar` are
  wrapped too. ccache does not cache links, so this is pure overhead and a large share of
  the 26% uncacheable calls.
- `extern/SDL3/SDL3/CMakeLists.txt:3248-3257` sets the launcher as **normal variables in
  that directory scope**, so `SDL_CCACHE=ON` would only affect SDL.

### Required changes

1. Resolve ccache with `find_program` and fail configure if absent; do not rely on a bare
   name in a preset (`CMakePresets.json:15` already resolves the compiler by bare name).
2. Set `CMAKE_C_COMPILER_LAUNCHER` / `CMAKE_CXX_COMPILER_LAUNCHER` once at the top level so
   every target inherits it.
3. Set `ASSIMP_BUILD_USE_CCACHE=OFF` so the global `RULE_LAUNCH_*` properties are not
   installed. With the launcher above, Assimp is still cached; nothing is double-wrapped.
4. Leave `SDL_CCACHE=OFF`.
5. Set `CCACHE_DIR=D:\YYBWorkSpace\ccache` and `max_size` 20-30 GB. 3613 cleanups at 16%
   hits is the signature of an undersized cache; one clean dependency build exceeds 5 GB.
6. Handle debug-info path hashing. `hash_dir = true` plus the `RelWithDebInfo` daily preset
   (`CMakePresets.json:32`) puts CWD into the key for `-g` builds. Either add
   `-ffile-prefix-map=<worktree>=.` or set `CCACHE_NOHASHDIR`; pick one and document it.
7. Set `CCACHE_BASEDIR`. **Corrected claim:** this shares hits between `build`,
   `build-game` and `build-editor` *of the same worktree*. It does **not** share hits
   between worktrees, because the differing topic segment
   (`.../worktrees/<topic>/VulkanLearn`) remains inside the rewritten path. Cross-worktree
   reuse is what Tier 3 provides.
8. Every measurement starts with `ccache --zero-stats`; otherwise "no cleanups" and
   ">90% hits" describe cache lifetime, not the run.

### Acceptance

- `ccache --zero-stats`, edit one project `.cpp`, rebuild twice: second build is all direct
  hits.
- Clean `vl_runtime` rebuild: hit rate above 90%, zero cleanups in that run.
- `ccache -s` no longer reports 99%+ usage.
- One `-Wa,-mbig-obj` editor TU still caches.

## Tier 2: scoped project clean

### Why the naive rules are wrong

**Coverage.** Project targets are not all under `build/source/CMakeFiles`. Verified
directories with project `.dir` targets in `build-game`: `source/CMakeFiles`,
`source/animation`, `source/debug`, `source/hair`, `source/physics` (2 targets),
`source/render/offline/core`, `source/ui/widgets`, plus `tool/hair` (4 targets) and
`tool/game-export` (2). Third-party objects live under `build/_deps/*-build`, not
`build/extern` (4 objects).

**Deleting `*.dir` breaks the build.** `CMakeFiles/<target>.dir/build.make` is included by
the generated `Makefile`, and `build-game/source/CMakeFiles/vl_runtime.dir/build.make:3754`
makes `link.txt` a prerequisite of `libvl_runtime.a` with no rule to regenerate it.
`build-game/CMakeFiles/Makefile.cmake:229` lists only `Makefile` and
`CMakeFiles/cmake.check_cache` in `CMAKE_MAKEFILE_OUTPUTS`, so `cmake_check_build_system`
cannot detect the loss. The result is `No rule to make target ...link.txt`, not a slow
rebuild.

### Rule

`tool/clean-project.ps1` deletes **outputs, not build-system metadata**:

- Default is a dry run; deletion requires `-Apply`.
- Resolve the build root; require it inside the repository; refuse paths containing
  `_deps`.
- Delete `*.obj`, `*.obj.d`, and project artifacts (`lib*.a`, `*.exe`, `*.dll` produced by
  project targets) under every project `CMakeFiles/<target>.dir`, skipping `_deps`,
  `extern`, and the dependency prefix.
- Keep `build.make`, `flags.make`, `link.txt`, `DependInfo.cmake`, `CMakeCache.txt` and
  `CMakeFiles/CMakeDirectoryInformation.cmake`.
- Provide `-Reconfigure` that additionally runs `cmake -S . -B <dir>` before returning, for
  the cases where a full metadata reset is wanted.
- Print the resolved list; fail when it is empty (guards a wrong `-BuildDir`).
- Keep `-Keep <regex>` for partial cleans.

### Acceptance

- `clean-project.ps1 -Apply` then `cmake --build build-game -j 4` recompiles **283** project
  objects (282 `source` + 1 `script`) and **zero** dependency objects, and relinks without a
  "No rule to make target" error.
- Same check on `build` (editor + tests) recompiles every project target, including
  `vl_editor`, `vl_editor_asset_core` and the test executables.
- `git status --short` unchanged; running twice is idempotent.

## Tier 3: machine-wide dependency prefix

### Location

```text
D:\YYBWorkSpace\VulkanLearn-deps\<toolchain-id>\<config>\
  bin\  lib\  include\  share\vl_deps\
  src\<dep>\      # shared fetch checkouts
  build\<dep>\    # per-dependency build trees
```

Machine-wide, **not** `artifacts/deps`: `/artifacts` is gitignored and per-worktree, so 26
worktrees would build 26 prefixes.

`toolchain-id` = `win64-mingw-<gcc-version>-<crt>`. Do **not** put the manifest hash in the
path; the stamp already covers it, and a path hash orphans the whole prefix on every edit.
Debug and Release are separate prefixes.

`ExternalProject_Add` defaults its source/build/prefix trees under the *current* binary
dir; pin `SOURCE_DIR`, `BINARY_DIR` and `INSTALL_DIR` to the machine-wide root or the 26×
duplication comes back. Add prefix retention (keep the last N toolchain/configs).

### Scope: per dependency, not all-or-nothing

`add_subdirectory(extern)` provides more than the heavy set:

- already prebuilt/imported: `Vulkan::Vulkan`, `Vulkan::shaderc_combined`,
  `Spirv_Reflect` (committed under `extern/`);
- header-only: `Json`, `Eigen3`, `stb_image`, `NVTX::nvtx`;
- small static: `BLAKE3::blake3`, `MikkTSpace`, `SpeedTreeParserCore`;
- heavy: `SDL3`, `Assimp`, `RmlUi`, `OpenEXR`, `FreeType`, `Jolt`, `ozz`;
- optional: `Tracy::TracyClient`, `NVTX::nvtx`.

Make the mode a per-dependency set with a **defined default**:
`VULKANLEARN_DEPS_PREBUILT="SDL3;Assimp;OpenEXR;RmlUi;FreeType;Jolt"`. Fail on unknown or
empty entries. `extern/CMakeLists.txt` skips only those and keeps the rest from source.

### Package contract: exact names and link type

`source/CMakeLists.txt:99-124` links `SDL3::SDL3`, `assimp`, `RmlUi::Core`, `Jolt`,
`OpenEXR::OpenEXR`, `Freetype::Freetype`, plus the non-prefix targets. Stock installs
export different names, so `vl_depsConfig.cmake` must add the aliases:

| Consumer name | Upstream export |
|---|---|
| `RmlUi::Core` | `RmlUi::rmlui_core` (alias at `Source/Core/CMakeLists.txt:365` is not exported) |
| `assimp` | `assimp::assimp` |
| `Jolt` | `Jolt::Jolt` |
| `SDL3::SDL3` | `SDL3::SDL3` |
| `OpenEXR::OpenEXR` | `OpenEXR::OpenEXR` |
| `Freetype::Freetype` | `Freetype::Freetype` |

Link types are mixed, not "only Jolt static":

| Shared (DLL) | Static |
|---|---|
| SDL3, Assimp, RmlUi, FreeType, OpenEXR/Imath/Iex/IlmThread | Jolt, ozz, BLAKE3, MikkTSpace, Spirv_Reflect, openjph, zlibstatic |

FreeType is shared today only because Assimp's `option(BUILD_SHARED_LIBS ... ON)`
(`extern/Assimp/Assimp/CMakeLists.txt:114`) runs first; a root FreeType build produces
`libfreetype.a`. Pin the link type per dependency in the superbuild instead of inheriting
global state.

### Runtime closure (blocking detail)

`build-game/bin` has 10 DLLs, but `main_game.exe` also imports `libstdc++-6.dll`,
`libgcc_s_seh-1.dll` and `libwinpthread-1.dll`; none are present. They resolve today only
because MinGW is on `PATH`, and `tool/game-export/nativeDependencies.cpp:116` also searches
`PATH`. Copying `prefix/bin/*.dll` does not fix this.

Decision required in D4: either copy the MinGW runtime trio into `bin/` and the game
package, or link all binaries with `-static-libgcc -static-libstdc++`. Pick one, test the
package on a machine without MinGW on `PATH`, and make the exporter's dependency closure
assert it.

### Superbuild mechanics

`add_subdirectory` does not reproduce standalone semantics:

- `extern/SDL3/SDL3/CMakeLists.txt:10-14,278`: `SDL_INSTALL` defaults to
  `SDL3_MAINPROJECT`, which is `OFF` as a subproject.
- `extern/RmlUi/RmlUi/CMakeLists.txt:13-15,156-164`: the export set is only installed when
  `CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME`. `RMLUI_IS_ROOT_PROJECT` is a normal variable,
  so a parent `set(RMLUI_IS_ROOT_PROJECT TRUE)` does work, but the supported path is a real
  root build.
- `cmake/VulkanLearnJolt.cmake:13` hardcodes `ENABLE_INSTALL OFF`.

Recommended: one `ExternalProject_Add` per heavy dependency, configured as root, with a
checked-in option table. Root defaults differ from the wrappers and several are wrong:

| Dependency | Required options |
|---|---|
| Assimp | `ASSIMP_BUILD_TESTS=OFF`, `ASSIMP_WARNINGS_AS_ERRORS=OFF` (root default ON), `ASSIMP_NO_EXPORT=ON` (+ FBX exporter when `VULKANLEARN_BUILD_ANIMATION_TOOLS=ON`) |
| SDL3 | `SDL_INSTALL=ON`, `SDL_TESTS=OFF`, `SDL_TEST_LIBRARY=OFF`, `SDL_SHARED=ON`, `SDL_STATIC=OFF` |
| RmlUi | `BUILD_TESTING=OFF` (root default ON hits `FATAL_ERROR` at `CMakeLists.txt:45-53`), `RMLUI_SAMPLES=OFF`, `RMLUI_PRECOMPILED_HEADERS=OFF`, `RMLUI_FONT_ENGINE=freetype` |
| OpenEXR | `OPENEXR_FORCE_INTERNAL_IMATH=ON`, `OPENEXR_USE_INTERNAL_DEFLATE=ON`, `OPENEXR_FORCE_INTERNAL_OPENJPH=ON`, `OPENEXR_USE_TBB=OFF`, `BUILD_TESTING=OFF` |
| FreeType | `BUILD_SHARED_LIBS=ON` explicitly |
| Jolt | `ENABLE_INSTALL=ON` (parameterize the existing function), `JPH_BUILD_SHARED_LIBS=OFF` |

Pass `Freetype_DIR` and `Imath_DIR` explicitly rather than relying on discovery order.
`script/CMakeLists.txt` must lose its duplicate `vl_add_jolt_cpu` copy; today it re-declares
the fetch without the `if(TARGET Jolt) return()` guard and calls it again.

### Stamp

`deps-stamp.json` is the only authority for "prefix is current". Hash:

- CMake version and generator;
- compiler path, `gcc -v` "Configured with" / `-dumpmachine` (CRT is not derivable from
  `--version`), and target triple;
- `CMAKE_BUILD_TYPE` plus **every** `CMAKE_C_FLAGS_<CONFIG>`, `CMAKE_CXX_FLAGS_<CONFIG>`,
  `CMAKE_EXE_LINKER_FLAGS[_<CONFIG>]`, `CMAKE_SHARED_LINKER_FLAGS[_<CONFIG>]` and
  `CMAKE_MODULE_LINKER_FLAGS[_<CONFIG>]` — the base flags are empty in practice
  (`build-game/CMakeCache.txt:183`, with `-O3 -DNDEBUG` only at `:195`);
- the VulkanLearn feature set that changes dependency options
  (`VULKANLEARN_BUILD_ANIMATION_TOOLS` toggles `ASSIMP_NO_EXPORT` / FBX exporter);
- manifest hash, patch hashes and the resolved option map per dependency;
- dependency version/commit/URL hash, and the installed artifact hashes.

One prefix per feature set: an animation-tools worktree cannot share the daily prefix.

D2 implements the identity/input half of this stamp (toolchain, compiler, CRT, config,
flags, feature options, `deps.lock` hashes). Installed artifact hashes are added in D3,
once a prefix actually contains built libraries.

### Mode selection

`VULKANLEARN_DEPS_MODE`:

- `source` — default; current behaviour.
- `prebuilt` — requires `VULKANLEARN_DEPS_ROOT`; `find_package(vl_deps CONFIG REQUIRED
  NO_DEFAULT_PATH)`; fail loudly on missing or mismatched stamp; never fall back silently;
  re-verify the stamp at build time (a prefix replaced after configure is otherwise
  invisible).

Presets opt into `prebuilt` explicitly.

### Offline

- `prebuilt` mode must not touch the network. That requires gating the unconditional
  `FetchContent_Populate` in `extern/tracy/CMakeLists.txt:20` and
  `extern/nvtx/CMakeLists.txt:20`, and moving Jolt out of root `CMakeLists.txt:45-46`
  behind the mode, because the per-dependency skip list in `extern/CMakeLists.txt` cannot
  skip it.
- `source` mode pins a machine-wide source cache for **all** fetched dependencies, not only
  OpenEXR/Imath/Jolt/Ozz: SDL3, Assimp, RmlUi, FreeType, Tracy, NVTX and googletest
  included.
- Test with `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` on a network-disabled run.

### Workflows

```powershell
# one-time / on dependency change
tool\build-deps.ps1 -Config RelWithDebInfo   # matches the development preset

# daily
cmake --preset development
cmake --build --preset development

# project-only reset
tool\clean-project.ps1 -BuildDir build-vscode\development -Apply
```

`CMakePresets.json` build presets target only `main`. Add a `main_game` preset for
game-only builds; use the `debug` preset (`build`, tests ON) for editor/game tests, since
`build-game` is game-only with tests OFF.

## D2 Implementation: manifest, lock, stamp and GC

State: implemented 2026-11-11 as documentation, JSON and PowerShell only. No `CMakeLists.txt`,
`source/**` or `tool/game-export/**` change is part of D2.

| Path | Role |
|---|---|
| `ThirdParty/manifest.json` | authoritative dependency inventory and option table |
| `ThirdParty/deps.lock` | deterministic lock regenerated from the manifest and patch set |
| `tool/deps-stamp.ps1` | computes, writes or checks `<prefix>/deps-stamp.json`; `-UpdateLock` regenerates the lock |
| `tool/deps-gc.ps1` | dry-run-first prefix garbage collector |

### Manifest schema (`vulkanlearn-deps-manifest/v1`)

```json
{
  "schema": "vulkanlearn-deps-manifest/v1",
  "manifestVersion": 1,
  "updated": "YYYY-MM-DD",
  "purpose": "one line",
  "policy": {
    "sourceKinds": ["vendored", "fetchcontent", "system"],
    "prefixLayout": "D:\\YYBWorkSpace\\VulkanLearn-deps\\<toolchain-id>\\<config>",
    "toolchainIdFormat": "win64-mingw-<gcc-version>-<crt>",
    "lockFile": "ThirdParty/deps.lock",
    "stampFile": "<prefix>/deps-stamp.json",
    "manifestHashInPrefixPath": false
  },
  "superbuildScope": ["SDL3", "Assimp", "OpenEXR", "RmlUi", "FreeType", "Jolt"],
  "featureOptions": [
    { "name": "VULKANLEARN_BUILD_ANIMATION_TOOLS", "default": "OFF",
      "affects": [{ "dependency": "Assimp", "options": { } }] }
  ],
  "dependencies": []
}
```

Per dependency:

```json
{
  "name": "SDL3",
  "role": "heavy | header-only | small-static | prebuilt | optional-object | test-only",
  "sourceKind": "vendored | fetchcontent | system",
  "version": "upstream tag, commit or 1.3.250",
  "revision": "tag or commit when sourceKind is fetchcontent",
  "origin": { "type": "git | url", "url": "...", "tag": "...", "sha256": "...", "declaredBy": "path:line" },
  "sourceDir": "extern/SDL3/SDL3 or <build>/_deps/<name>-src",
  "path": "extern/... for vendored entries",
  "wrapper": "the CMake file that declares it",
  "targets": ["SDL3::SDL3"],
  "linkType": "shared | static | interface | object | prebuilt-static | prebuilt-shared",
  "superbuild": true,
  "transitive": true,
  "parent": "OpenEXR",
  "contentTree": "git tree object id for vendored directories",
  "artifacts": [{ "path": "...", "sha256": "...", "bytes": 0 }],
  "patches": [],
  "cmake": {
    "declaredOptions": {},
    "featureOptions": { "<VULKANLEARN_OPTION>": { "OFF": {}, "ON": {} } },
    "resolvedOptions": {},
    "resolvedFrom": "build-game/CMakeCache.txt (Release, 2026-11-11)",
    "superbuildRequired": {}
  },
  "notes": "why this entry exists or what is missing"
}
```

Rules:

- `declaredOptions` is what the repository's wrapper sets today. `resolvedOptions` is
  captured evidence from `build-game/CMakeCache.txt`, not a second source of truth.
  `superbuildRequired` is the D3 option table that must be forced when the dependency is
  built as a root `ExternalProject`.
- `featureOptions` records options that change the dependency's build, keyed by the
  `VULKANLEARN_*` value. `VULKANLEARN_BUILD_ANIMATION_TOOLS` toggles Assimp's exporter set;
  `VULKANLEARN_BUILD_TESTS` decides whether googletest exists at all.
- Prebuilt committed artifacts (Shaderc, VulkanSDK, SPIRV-Reflect) have no usable upstream
  version string. Their `artifacts[].sha256` is the pin, and `contentTree` pins the whole
  vendored directory. MikkTSpace is pinned the same way.
- `system` is a legal `sourceKind` for future non-Windows packages, but no entry uses it:
  the current build is Windows-first and self-bootstrapping.
- `ThirdParty/patches/` is empty today. Adding `patches/<dependency>/*.patch` changes the
  lock automatically.

### Option table location

The authoritative option table is `ThirdParty/manifest.json ->
dependencies[].cmake.{declaredOptions,featureOptions,superbuildRequired}`. The tables in
the Tier 3 sections above are the review record and must not be edited independently.
`tool/deps-stamp.ps1 -UpdateLock` hashes a projection of exactly those three maps, so an
option edit without a lock update is a hard failure.

### Lock schema (`vulkanlearn-deps-lock/v1`)

`ThirdParty/deps.lock` is deterministic: no timestamps, keys sorted ordinal, arrays in
declared order, UTF-8 without BOM, LF.

```json
{
  "schema": "vulkanlearn-deps-lock/v1",
  "lockVersion": 1,
  "generatedBy": "tool/deps-stamp.ps1 -UpdateLock",
  "normalization": "UTF-8 without BOM; object keys sorted ordinal; arrays in declared order; 2-space indent; LF",
  "manifest": { "path": "ThirdParty/manifest.json", "schema": "...", "sha256": "..." },
  "optionSet": { "project": "...", "sha256": "..." },
  "patches": { "root": "ThirdParty/patches", "files": [], "sha256": "..." },
  "lockSha256": "..."
}
```

`lockSha256` covers every field except itself. The stamp consumes
`manifest.sha256`, `optionSet.sha256`, `patches.sha256` and `lockSha256`; it does not
consume the manifest path or file mtime. Reformatting the manifest does not change the
hash; editing any value, option or patch does.

### Stamp fields (`<prefix>/deps-stamp.json`, `vulkanlearn-deps-stamp/v1`)

| Field | Content |
|---|---|
| `schema` | `vulkanlearn-deps-stamp/v1` |
| `prefix` | resolved absolute prefix; informational, excluded from `identitySha256` so a moved prefix is still comparable |
| `createdAt`, `lastUsed` | UTC ISO-8601; `lastUsed` is refreshed by a successful `-Check` unless `-NoTouch` |
| `identity.platform` | `win64` or `win-arm64`, derived from the compiler target triple |
| `identity.toolchainId` | `win64-mingw-<gcc-version>-<crt>` |
| `identity.compiler` | `{id, version, path, target, crt}`; CRT is detected from `libstdc++-6.dll` imports (`api-ms-win-crt-*` or `msvcrt.dll`) with a `gcc -v` fallback |
| `identity.generator`, `identity.cmakeVersion` | from `CMakeCache.txt` |
| `identity.config` | `Debug`, `Release`, `RelWithDebInfo` or `MinSizeRel` |
| `inputs.flags` | base and per-config `CMAKE_{C,CXX}_FLAGS`, `CMAKE_{EXE,SHARED,MODULE}_LINKER_FLAGS` |
| `inputs.featureOptions` | every `manifest.featureOptions[].name` resolved from the cache, `<unset>` when absent |
| `inputs.manifestSha256`, `optionSetSha256`, `patchSetSha256`, `lockSha256` | copied from `deps.lock` |
| `identitySha256` | SHA-256 of the deterministic `{identity, inputs}` payload |

`-Check` fails with exit 1 when the stamp is missing, internally inconsistent (a hand-edited
field whose `identitySha256` no longer covers it), or differs from the current build tree.
It prints field-by-field `stamp=`/`current=` differences. `-Write` creates the prefix
directory and the stamp; `-UpdateLock` needs no build tree.

The manifest hash must never appear in the prefix path: a manifest edit would orphan the
entire prefix. The stamp is the only authority for "this prefix is current".

### GC policy (`tool/deps-gc.ps1`)

- Dry run by default; deletion requires `-Apply`.
- `-DepsRoot` defaults to `D:\YYBWorkSpace\VulkanLearn-deps`. The root must exist, be an
  absolute path, not a drive root, not shorter than 12 characters, and not inside the
  repository. An empty root (no stamped prefixes) is a hard failure, not a no-op.
- Only `<deps-root>/<toolchain-id>/<config>` directories are considered, and only when they
  contain `deps-stamp.json`. Reparse points are skipped and never deleted. Every deletion
  re-resolves the path and asserts it stays under the root.
- Build trees are scanned for references: each worktree's `build*/CMakeCache.txt` and any
  build-side `deps-stamp.json`. A prefix is never deleted while referenced. The check
  accepts the full path or the `\<toolchain-id>\<config>` suffix, so a cache written through
  an 8.3 path (`%TEMP%`) still matches a long-path stamp.
- Hard keeps: the current toolchain's `Debug` and `Release` (`-KeepConfigs` overrides the
  set), the most recently used config of that toolchain (normally the daily
  `RelWithDebInfo` preset), plus every prefix of one previous toolchain. A stamp that is
  missing, unreadable or does not match its own prefix is kept and reported as
  `keep-invalid`.
- Budget: 20 GB by default (`-BudgetGB`). Non-hard-kept prefixes are deleted least-recently
  used first until the total fits. If the hard-kept set alone exceeds the budget, GC warns
  and deletes nothing from that set.
- After deleting the last config of a toolchain, the now-empty toolchain directory is
  removed under the same safety checks.

### Acceptance

- Delete a build directory, configure `prebuilt`, build: zero third-party compiles and no
  network access (`FETCHCONTENT_FULLY_DISCONNECTED=ON`).
- `source` mode still bootstraps from a fresh clone.
- Stamp mismatch produces a clear error, not a link failure.
- `main_game.exe` passes `--exit-after-frames 120` **on a machine with MinGW removed from
  `PATH`**, and the exported package imports nothing outside its own directory.
- `vl_asset_edit`, `material_instance_editor_production_tests`, `editor_ui_tests` and
  `game_export_tests` link from the `debug` tree.
- Editing one manifest entry rebuilds exactly the affected dependency.

## D3/D4 Implementation: superbuild and prebuilt mode

State: implemented 2026-11-11 in the working tree. The six heavy dependencies are
installed once per toolchain/configuration and the root project consumes them
without compiling their sources.

| Path | Role |
|---|---|
| `ThirdParty/CMakeLists.txt` | ExternalProject superbuild for SDL3, Assimp, OpenEXR (+Imath/OpenJPH), RmlUi, FreeType and Jolt |
| `ThirdParty/vl_depsConfig.cmake.in` | package config copied to `<prefix>/share/vl_deps`; imports upstream configs and adds the consumer aliases |
| `cmake/VulkanLearnDeps.cmake` | shared toolchain-id/prefix resolution, stamp validation, mode wiring and DLL deployment |
| `tool/build-deps.ps1` | source preparation, configure/build, lock validation and stamp write/check |

### Actual prefix layout

```text
D:\YYBWorkSpace\VulkanLearn-deps\win64-mingw-16.2.0-ucrt\Release\
  bin\        SDL3.dll, libassimp-6.dll, libOpenEXR*.dll, libImath-3_2.dll,
              libfreetype.dll, librmlui*.dll
  lib\        import libraries plus libJolt.a
  lib\cmake\  SDL3, assimp-6.0, freetype, Imath, OpenEXR, Jolt, RmlUi
  include\    installed headers
  share\vl_deps\vl_depsConfig.cmake + vl_depsConfigVersion.cmake
  deps-stamp.json
```

Shared source checkouts live in `<deps-root>/_sources` and per-dependency build
trees in `<deps-root>/_build/<compiler-id>-<version>/<config>`, so cleaning
`build-deps` does not re-download or re-clone sources.

### Imported target table (verified against the installed configs)

| Consumer target | Upstream export |
|---|---|
| `SDL3::SDL3` | `SDL3::SDL3` |
| `assimp` | `assimp::assimp` (config installed under `lib/cmake/assimp-6.0`) |
| `RmlUi::Core` | `RmlUi::RmlUi` (RmlUi 6.2 sets `EXPORT_NAME RmlUi`; the `rmlui_core` alias in the plan was stale) |
| `OpenEXR::OpenEXR` | `OpenEXR::OpenEXR` (+ `Imath::Imath`) |
| `Freetype::Freetype` | `Freetype::Freetype` |
| `Jolt` | `Jolt::Jolt` |

Vendored OpenJPH is built with `EXCLUDE_FROM_ALL` inside OpenEXR, so it is not
installed and `OpenEXRConfig.cmake` generates an empty `if ()` for
`find_dependency(openjph)`. `vl_depsConfig.cmake` therefore does not request it.

### Option table and export deviations found while building

- RmlUi installs runtime dependencies through
  `file(GET_RUNTIME_DEPENDENCIES)`, which needs MinGW on `PATH` and fails on
  `libfreetype.dll`/`libstdc++-6.dll`. The superbuild passes
  `RMLUI_INSTALL_RUNTIME_DEPENDENCIES=OFF`; deployment is the consumer's job.
- `OPENEXR_USE_INTERNAL_DEFLATE` is a no-op in 3.4.10; the superbuild passes
  `OPENEXR_FORCE_INTERNAL_DEFLATE=ON` as the manifest requires.
- `OPENEXR_FORCE_INTERNAL_IMATH=ON` plus
  `FETCHCONTENT_SOURCE_DIR_IMATH=<deps-root>/_sources/imath` keeps the Imath
  checkout pinned and avoids a second download.
- FreeType is pinned to `BUILD_SHARED_LIBS=ON`; Assimp and RmlUi explicitly set
  their root-project defaults (`tests`, `samples`, `warnings-as-errors`,
  `BUILD_TESTING`).

### Mode wiring

- `VULKANLEARN_DEPS_MODE=source|prebuilt`; `source` is the default and keeps the
  existing in-tree behavior.
- `VULKANLEARN_DEPS_PREBUILT` defaults to the six heavy names. `extern/CMakeLists.txt`
  and the root Jolt call skip only entries in that list; Json/Eigen/STB/MikkTSpace/
  SpirvReflect/Ozz/SpeedTree/Shaderc/VulkanSDK/BLAKE3 stay in-tree.
- `prebuilt` requires `VULKANLEARN_DEPS_ROOT`, resolves
  `<root>/<toolchain-id>/<config>`, fails on a missing/invalid stamp, and calls
  `find_package(vl_deps CONFIG REQUIRED NO_DEFAULT_PATH)`.
- `vl_deps_stamp_check` runs `tool/deps-stamp.ps1 -Check` before `vl_runtime`
  builds; a flag/feature/lock change fails the build instead of silently using
  stale libraries.
- Offline configure is supported with
  `FETCHCONTENT_FULLY_DISCONNECTED=ON`: Ozz uses
  `<deps-root>/_sources/ozz` and googletest uses
  `<deps-root>/_sources/googletest`; Tracy/NVTX remain gated by their option.
- `bin/*.dll` from the prefix plus `libstdc++-6.dll`, `libgcc_s_seh-1.dll` and
  `libwinpthread-1.dll` are copied to the executable directory by
  `vl_prebuilt_runtime_dlls`.

### Verification evidence

- `tool/build-deps.ps1 -Config Release` built all six dependencies and wrote
  `deps-stamp.json` (11 DLLs, 13 import/static libraries).
- `build-prebuilt` configured with
  `-DFETCHCONTENT_FULLY_DISCONNECTED=ON -DVULKANLEARN_ENABLE_EDITOR=OFF
  -DVULKANLEARN_BUILD_TESTS=OFF`; `_deps` contains only `vl_ozz-build`, and
  `extern/` contains only the light dependencies.
- `build-prebuilt` object distribution: 326 objects total, 217 in
  `source/CMakeFiles`, 39 in `_deps/vl_ozz-build`, 0 heavy-dependency objects.
- `main_game` smoke (`SC_fracture_playground`, 60 frames) exits 0 with
  `EngineLoop teardown complete`; a second 20-frame run with `PATH` reduced to
  `C:\Windows\System32;C:\Windows` also exits 0.
- Source mode `build-game/main_game` still builds green.

### Remaining limitations

- The stamp identity includes `VULKANLEARN_ENABLE_EDITOR` and
  `VULKANLEARN_BUILD_TESTS`, so a prefix is feature-set scoped even though those
  flags do not change dependency options. A separate prefix is required for a
  different feature set (for example tests ON).
- Only the Release prefix is currently built. A Debug prefix needs a separate
  `tool/build-deps.ps1 -Config Debug` run.
- Tracy/NVTX and the heavier optional tools do not have a machine-wide source
  cache yet; enabling them in `prebuilt` mode may still require network access.
- `prebuilt` requires a single-configuration generator because the prefix is
  resolved from `CMAKE_BUILD_TYPE`.

## Phases

| Phase | Deliverable | Exit criteria | Rollback |
|---|---|---|---|
| D0 | ccache launcher, `ASSIMP_BUILD_USE_CCACHE=OFF`, cache dir/size, hash-dir decision | `--zero-stats` then project rebuild: >90% hits, zero cleanups | remove preset keys |
| D1 | `tool/clean-project.ps1` (object-level) | 283 project objects recompiled, zero dependency objects, no "No rule to make target" | delete script |
| D2 | `ThirdParty/manifest.json`, `ThirdParty/deps.lock`, `tool/deps-stamp.ps1`, `tool/deps-gc.ps1` | stamp mismatch on flags/compiler/feature/manifest change; GC refuses an empty root and a referenced prefix | docs/script only |
| D3 | per-dependency superbuild (ExternalProject) for the six | `find_package(vl_deps)` imports all six with the alias table | stay on `source` mode |
| D4 | `VULKANLEARN_DEPS_MODE` + per-dependency skip + runtime closure | `prebuilt` clean build offline; `main_game` runs without MinGW on `PATH` | `source` mode |
| D5 | source cache for every fetched dependency; remove Jolt duplication; gate Tracy/NVTX | `source` mode configures offline from the cache | revert entries |

## Risks

- Shared DLL deployment and the MinGW runtime trio are runtime-only failures; smoke-test
  both executables and the exported package on a clean `PATH`.
- Export gaps: RmlUi (`RMLUI_IS_ROOT_PROJECT`), Jolt (`ENABLE_INSTALL OFF`), and renamed
  exports (`RmlUi::rmlui_core`, `assimp::assimp`, `Jolt::Jolt`). Main schedule risk.
- Root-default drift: Assimp tests/warnings, RmlUi `BUILD_TESTING`, FreeType link type.
  The checked-in option table is the guard.
- Feature-set coupling: `VULKANLEARN_BUILD_ANIMATION_TOOLS` changes Assimp options, so the
  prefix must be feature-scoped.
- Path length: current max build path is 209 chars; keep the prefix short but this is not a
  current blocker.
- Licensing: add Imath, OpenJPH, libdeflate, Assimp's bundled zlib/draco, and the committed
  Shaderc/VulkanSDK binaries to the notice set. Today only ozz writes a license file
  (`extern/OzzAnimation/CMakeLists.txt:22-24`); `tool/game-export` has no copy step and
  `tool/validate-game-package.ps1` does not check notices.

## Non-Goals

- No dependency binaries committed to git. `extern/VulkanSDK/lib/Win32/vulkan-1.lib` and
  `extern/Shaderc/lib/*` are already committed prebuilt binaries; this plan does not extend
  that pattern.
- No CI artifact registry, remote cache, vcpkg/Conan, Ninja migration, or change to
  runtime/editor ownership and packaging contracts.

## Review Record

Independent review verdict: **approve with changes**. The following blocking issues were
found against the repository and are folded into the tiers above:

- **B1 — naive scoped clean breaks Makefiles.** Deleting `CMakeFiles/<target>.dir` removes
  `build.make`/`link.txt` metadata that the generated Makefiles include; keep metadata and
  delete only objects/artifacts.
- **B2 — `prebuilt` cannot be zero-network yet.** Tracy/NVTX populate unconditionally and
  Jolt is fetched from two places; the phase must add explicit offline gates.
- **B3 — MinGW runtime is not in the package closure.** `libstdc++-6`, `libgcc_s_seh-1` and
  `libwinpthread-1` currently come from `PATH`.
- **B4 — stamp hashing is incomplete.** Effective `CMAKE_CXX_FLAGS_<CONFIG>` and feature
  options such as `VULKANLEARN_BUILD_ANIMATION_TOOLS` must participate; the manifest hash
  must not be embedded in the prefix path.
- **B5 — root-project defaults drift.** Assimp, RmlUi and FreeType change defaults when
  built from a superbuild; the checked-in option table and export aliases are required.

Corrected measurements: the heavy five dependencies are 91.8% of dependency objects
(FreeType raises it to 96%), and the longest current build path is 209 characters, not a
blocker.

## Decisions (Resolved)

All three decisions were made on 2026-11-11:

1. **MinGW runtime: copy the three DLLs.** `libstdc++-6.dll`, `libgcc_s_seh-1.dll` and
   `libwinpthread-1.dll` are copied explicitly from the compiler bin directory and
   validated as part of the package. Do not static-link the runtime in this cycle.
2. **Daily `development` stays `RelWithDebInfo`.** Add `CCACHE_BASEDIR` and
   `-fdebug-prefix-map` so `-g` builds still share cache across worktrees. Add a
   `development-fast` Release preset for speed-sensitive work; do not change the default.
3. **Prefix GC:** keep the current toolchain's Debug + Release and one previous toolchain.
   Use a 20 GB budget and `lastUsed` stamps; `tool/deps-gc.ps1` is dry-run by default and
   only deletes prefixes not referenced by a build tree.
