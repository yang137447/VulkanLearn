# C# Actor Runtime Plan

## Status

- Type: executable architecture and implementation plan
- Created: 2026-09-03
- State: planned, not implemented
- Naming decision: gameplay source files use `*.actor.cs`
- Scope: an in-process C# gameplay layer with code-authored Actors; no node-based Blueprint editor or graph runtime

This plan defines a usable C# Actor runtime for VulkanLearn. Gameplay authors
write normal C# classes, compile them into a managed gameplay assembly, and
attach those classes to scene-authored Actor instances. C++ keeps ownership of
the engine, World, resources, Vulkan, and frame boundaries. C# owns gameplay
behavior through a narrow, explicit API.

The current architecture intentionally excluded reflection, GC, editor, and
Blueprint infrastructure from V1. This plan is a post-V1 extension and must
preserve the existing Game Thread / Render Thread boundary, World transaction
rules, resource lifetime rules, and renderer snapshot contract.

## Target Outcome

```text
Player.actor.cs
    -> Gameplay.csproj
    -> Gameplay.dll + PDB
    -> native ScriptRuntime loads the assembly
    -> scene creates Gameplay.PlayerActor
    -> OnCreate / BeginPlay / Tick / EndPlay
    -> C# reads Input and changes Actor Transform
    -> C++ World is updated on the Game Thread
    -> WorldSnapshot is built
    -> Vulkan renderer displays the result
```

The first usable release must support Actor identity, lifecycle, components,
transforms, input actions, timers, events, spawning, destruction, scene
properties, diagnostics, test coverage, and a controlled reload story. It is
not merely a P/Invoke proof of concept.

## Current Baseline

The repository already provides useful boundaries but not the runtime itself:

- `EngineLoop` owns startup, per-frame dispatch, World transitions, and render
  submission in `source/engine/engineLoop.*`.
- `World` is the Game Thread-owned mutable gameplay state in
  `source/world/world.*`.
- `WorldSnapshot` is an immutable render-facing DTO; the renderer must not read
  mutable World or managed objects directly.
- `InputSubsystem` converts SDL input into engine-facing intent, but currently
  exposes only hard-coded movement keys and mouse deltas.
- `CommandBus` and `RuntimeCommandExecutor` demonstrate deferred owner-side
  commands, but are not a C# ABI or a general gameplay command system.
- `WorldLoader` and `WorldBuilder` support fixed scene object types. The scene
  schema has no Actor class, script property, or component declaration.
- `RuntimeConfig` has no script assembly, script root, reload, or execution
  policy configuration.
- The CMake project builds C/C++ only. There are no managed projects, runtime
  host, binding layer, or script tests.

## Design Principles

1. **C# is gameplay, not renderer control.** Managed code cannot access Vulkan,
   `RenderSystem`, `RenderGraph`, renderer caches, descriptor sets, or native
   GPU objects.
2. **C++ owns identity and lifetime.** C# receives opaque handles and cannot
   delete native objects or keep ownership through raw pointers.
3. **Game Thread is the first execution model.** Actor callbacks and World API
   calls run on the Game Thread. Render Thread code only consumes snapshots.
4. **World transitions are transactional.** Candidate World and candidate
   script context are prepared together and published together.
5. **Frame mutation is deterministic.** Spawn, destroy, attach, and component
   changes are queued and flushed at defined frame boundaries.
6. **The public managed API is small and versioned.** Eigen, STL containers,
   Vulkan-Hpp types, and C++ exceptions never become the ABI.
7. **Normal C# tooling remains usable.** `*.actor.cs` is an engine convention,
   not a requirement for a custom IDE.
8. **Failures are contained.** A script exception is diagnosed with Actor and
   callback context and cannot silently corrupt World state.

## Scope And Non-Goals

### In scope

- In-process .NET/CoreCLR-hosted C# runtime for the current Windows-first target.
- One or more managed gameplay assemblies, with `Gameplay.dll` as the default.
- `Actor` and `Component` base types.
- Scene binding by managed type name and serialized properties.
- Transform, input, time, timer, event, logging, asset-reference, and basic
  World APIs.
- Deferred structural commands and stable opaque handles.
- World-load integration and deterministic shutdown.
- Build, deploy, diagnostics, tests, and an initial manual reload path.

### Explicitly out of scope for the first release

- Blueprint graph editor or node graph format.
- General-purpose C++ reflection/GC/UObject system.
- Direct C# authoring of Vulkan pipelines, shaders, descriptors, or render passes.
- Untrusted Mod sandboxing inside the engine process.
- Multi-threaded Actor Tick or arbitrary managed access from the Render Thread.
- Full physics, animation, audio, networking, replication, or save-game systems.
- Hot replacement of arbitrary managed code while preserving every object field.

## Proposed Runtime Architecture

```text
EngineLoop (GT)
  ├─ InputSubsystem
  ├─ ScriptRuntimeSubsystem
  │    ├─ ScriptHost / .NET boundary
  │    ├─ AssemblyCatalog
  │    ├─ ScriptTypeRegistry
  │    ├─ ScriptWorldContext
  │    ├─ ActorInstanceRegistry
  │    ├─ ScriptScheduler
  │    ├─ GameplayCommandBuffer
  │    └─ ScriptDiagnostics
  ├─ WorldManager / WorldTransitionCoordinator
  └─ WorldSnapshotBuilder -> RenderSystem

Managed side
  ├─ VulkanLearn.Runtime.dll
  │    ├─ Actor / Component
  │    ├─ Handles and math value types
  │    ├─ Transform / World / Input / Time APIs
  │    ├─ Timer / Event APIs
  │    └─ NativeApi P/Invoke declarations
  └─ Gameplay.dll
       └─ user-authored `*.actor.cs` classes
```

### Ownership matrix

| Object | Owner | Managed view |
|---|---|---|
| Actor identity and enabled state | C++ World / ActorRegistry | `ActorHandle` |
| Actor Transform | C++ World | `Transform` value/proxy API |
| Native component data | C++ ComponentRegistry | typed component handle |
| C# object instance | ScriptRuntime | managed reference, never native owner |
| GPU mesh/material/texture | renderer resource owners | logical `AssetId` |
| Frame render input | `WorldSnapshotBuilder` | not exposed as mutable object |
| callback scheduling | ScriptScheduler | callback token/wrapper |

## Managed API Shape

The following is the intended shape, not a frozen implementation signature:

```csharp
public abstract class Actor
{
    public ActorHandle Handle { get; }
    public Transform Transform { get; }
    public bool Enabled { get; set; }

    protected virtual void OnCreate() { }
    protected virtual void BeginPlay() { }
    protected virtual void Tick(float deltaTime) { }
    protected virtual void LateTick(float deltaTime) { }
    protected virtual void EndPlay(EndPlayReason reason) { }
}

public abstract class Component
{
    public ComponentHandle Handle { get; }
    public Actor Owner { get; }
    public bool Enabled { get; set; }

    protected virtual void OnCreate() { }
    protected virtual void BeginPlay() { }
    protected virtual void Tick(float deltaTime) { }
    protected virtual void EndPlay(EndPlayReason reason) { }
}
```

Example authoring style:

```csharp
public sealed class PlayerActor : Actor
{
    [Expose]
    public float MoveSpeed { get; set; } = 5.0f;

    protected override void BeginPlay()
    {
        Log.Info($"Player started: {Name}");
    }

    protected override void Tick(float deltaTime)
    {
        var movement = Input.GetVector2("Move");
        Transform.Translate(new Vector3(movement.X, 0.0f, movement.Y) * MoveSpeed * deltaTime);
    }
}
```

The final API must document whether property access is a cached managed value,
a native proxy, or a command submission. Prefer batched native calls and avoid
one P/Invoke transition per scalar field per frame.

## Native/Managed ABI

Use a dedicated C-compatible layer, for example:

```text
source/script/native/scriptNativeApi.h
source/script/native/scriptNativeApi.cpp
source/script/native/scriptAbiTypes.h
```

Required rules:

- Use fixed-width integers, POD structs, UTF-8 strings, caller-provided buffers,
  and explicit release functions.
- Use opaque `uint64_t` IDs plus generation/context tokens to reject stale
  handles.
- Never export C++ name-mangled functions, STL containers, Eigen objects,
  `shared_ptr`, exceptions, or Vulkan handles.
- Every call returns a status or writes a structured error into an engine-owned
  error channel; exceptions never cross the ABI.
- ABI and managed runtime versions are checked before loading `Gameplay.dll`.
- Group functions by ownership: runtime, actor, component, transform, input,
  time, timer, event, asset, diagnostics, and command submission.

The first ABI version must be narrow. Expanding it requires an explicit version
decision and managed wrapper update, not ad hoc exported helper functions.

## Actor And Component Model

Every Actor and Component needs:

- stable runtime ID within the active World;
- World generation or context ID;
- debug name and managed type name;
- enabled/started/pending-destroy state;
- optional tags and parent Actor ID.

Handles become invalid after destruction or World replacement. Native code must
validate generation before every mutating operation.

### Lifecycle

```text
Construct managed object
    -> bind native handle and serialized properties
    -> OnCreate
    -> BeginPlay
    -> Tick / Component Tick
    -> LateTick
    -> EndPlay
    -> release managed instance and native registration
```

Required decisions:

- Actor creation follows scene declaration order.
- Component creation follows declaration order.
- `BeginPlay` occurs only after the candidate World is valid.
- `Destroy` marks for end-of-frame removal and does not invalidate iteration.
- `Spawn` becomes visible at the next documented phase.
- A callback exception disables the failing Actor according to configured policy
  and records a structured diagnostic.

### Components

Start with adapters for current runtime needs:

- `TransformComponent`;
- `MeshRendererComponent`;
- `CameraComponent`;
- `DirectionalLightComponent`;
- `PointLightComponent`;
- `SpotLightComponent`;
- `EnvironmentComponent` only if gameplay requires it.

Do not force an immediate rewrite of current camera/light/mesh loading. Add the
component layer first, then migrate individual scene types when their ownership
and snapshot representation are ready.

## Frame And Thread Contract

The target frame order is:

```text
1. Process platform events
2. Update input action state
3. Advance RuntimeClock
4. Publish input/time to ScriptRuntime
5. Run Actor / Component Tick on the Game Thread
6. Flush gameplay structural commands
7. Run LateTick and final transform propagation
8. Build WorldSnapshot
9. Submit/render snapshot
10. Retire timers, callbacks, and destroyed instances
```

The exact placement must be reconciled with the current `EngineLoop::Tick()`
ordering before implementation. Scripts run after input is available and before
`WorldSnapshotBuilder` reads World.

First-version restrictions:

- C# callbacks run only on the Game Thread.
- C# never reads mutable World from the Render Thread.
- Structural changes use `GameplayCommandBuffer`.
- Background managed work may calculate plain data only; applying it requires a
  Game Thread continuation or explicit command.
- Shutdown stops script callbacks before renderer and World owners are torn down.

## Scene And Configuration Contract

Add an Actor form without breaking existing fixed object forms:

```json
{
    "name": "Player",
    "type": "actor",
    "class": "Gameplay.PlayerActor",
    "position": [0, 0, 0],
    "rotation": [0, 0, 0],
    "scale": [1, 1, 1],
    "components": [
        { "type": "mesh", "modelPath": "models/SM_player.json" }
    ],
    "properties": {
        "MoveSpeed": 5.0
    }
}
```

The schema must define managed type and assembly identity, transform defaults,
component properties, property conversion/validation, tags, parent reference,
enabled state, and spawn policy. Exact field names are deferred until the native
Actor/component ownership model is implemented.

Runtime configuration should add a `scripts` block:

```json
{
    "scripts": {
        "enabled": true,
        "projectRoot": "script",
        "runtimeAssembly": "Gameplay/bin/Debug/net8.0/Gameplay.dll",
        "runtimeConfig": "Gameplay.runtime.json",
        "reloadMode": "manual",
        "exceptionPolicy": "disable_actor"
    }
}
```

Paths are resolved through `RuntimeConfig`; the script subsystem must not
rediscover the project root independently.

## Build And Deployment Plan

Recommended managed layout:

```text
script/
├── VulkanLearn.Runtime/
│   ├── VulkanLearn.Runtime.csproj
│   └── ...
└── Gameplay/
    ├── Gameplay.csproj
    ├── Gameplay.runtime.json
    └── Actors/
        ├── Player.actor.cs
        └── CameraController.actor.cs
```

`*.actor.cs` is a source naming convention. The project file must explicitly
include the files if SDK globs do not include them, while preserving standard
`dotnet`, Rider, and Visual Studio language-service behavior.

CMake should:

- expose `VULKANLEARN_ENABLE_CSHARP_ACTORS`;
- locate or validate the required .NET SDK/runtime;
- invoke the managed project as an explicit target;
- stage the assembly, dependencies, runtime config, and PDB predictably;
- make `main` depend on managed output only when enabled;
- report a clear configuration error rather than silently disabling scripts.

The C++ build remains valid without the .NET SDK when the option is disabled.
The initial native target is Windows/MinGW-compatible hosting; C++/CLI is not a
dependency or integration route.

## Runtime Hosting And Assembly Loading

`ScriptHost` owns managed runtime lifetime:

1. Validate host/runtime ABI versions.
2. Initialize the managed runtime before loading the initial World.
3. Load `VulkanLearn.Runtime.dll` and the configured gameplay assembly.
4. Discover Actor types through a controlled registration/reflection boundary.
5. Cache type metadata and lifecycle entry points.
6. Create a `ScriptWorldContext` for each active/candidate World.
7. Stop callbacks before unloading or replacing a context.
8. Shutdown only after all instances and callbacks are gone.

Use a generated registry or managed registration method where practical.
Reflection may support managed authoring properties, but must not become a broad
native reflection system.

## Properties And Serialization

Use explicit authoring attributes:

```csharp
[Expose]
public float MoveSpeed { get; set; } = 5.0f;
```

Initial property types:

```text
bool, int, uint, float, double, string,
Vector2, Vector3, Vector4, Quaternion,
AssetId, ActorReference, enum
```

The property pipeline needs one source of truth for defaults, JSON conversion,
validation, unknown fields, editor/runtime visibility, reset behavior, and
serialization. Do not serialize arbitrary managed object graphs initially.

## Input, Time, Timer, And Events

### Input

Keep SDL ownership in `InputSubsystem`, but replace fixed movement fields with
an action/axis map:

```csharp
Input.IsPressed("Jump")
Input.WasPressed("Jump")
Input.WasReleased("Jump")
Input.GetAxis("MoveForward")
Input.GetVector2("Move")
```

Input contexts must let UI capture disable gameplay input without exposing SDL.

### Time

Expose explicit semantics:

```text
Time.DeltaTime
Time.UnscaledDeltaTime
Time.FixedDeltaTime (only when fixed update exists)
Time.TimeSinceStartup
Time.TimeScale
Time.FrameIndex
```

The first implementation may omit physics-driven fixed update, but must not
pretend variable frame delta is fixed time.

### Timers and events

Engine-owned timers/events must provide one-shot and repeating timers,
cancellation by token and owner, subscription tokens, deterministic callback
phase/order, and automatic cleanup on Actor destruction or World replacement.
No callback may remain reachable through a timer or event after its owner ends.

## Asset And Rendering Integration

C# refers to resources logically:

```csharp
var mesh = Assets.Get<MeshAsset>("Meshes/Player");
```

The first release may expose references without complete asynchronous loading,
but must define identity, normalized paths, status/failure, World ownership,
renderer cache lookup, and retirement behavior. GPU resource creation remains in
the existing renderer resource loaders and transactional World/Graph path.

## World Transition Integration

Extend the existing prepare/commit path rather than adding a parallel script
loader:

```text
WorldTransitionCoordinator
  -> load and validate scene
  -> prepare renderer resource package
  -> build candidate World
  -> prepare candidate ScriptWorldContext
  -> create and validate candidate Actors
  -> prepare runtime binding
  -> commit World + script context + renderer owners
  -> begin play candidate scripts
  -> retire old script context after callbacks stop
```

The safe default is to construct and validate before commit, publish ownership,
then call `BeginPlay` at the next stable Game Thread phase. The post-commit
`BeginPlay` failure policy must be explicit and must not partially roll back
Vulkan ownership.

## Error Handling And Diagnostics

Every script failure should include assembly/type, Actor name and handle, World
scene path and generation, callback, exception type/message/stack, frame index,
and the resulting disable/destroy/load-failure action.

Use `DiagnosticsSubsystem` as the initial report sink. Start with this policy:

```text
construction / property validation failure -> reject candidate World
BeginPlay failure -> reject before activation when possible
Tick failure -> disable the Actor and continue
fatal host/runtime failure -> request controlled shutdown
```

## Reload Strategy

### Level 0: restart-only

Rebuild `Gameplay.dll` and restart the process. This is mandatory for the first
vertical slice and provides a reliable baseline.

### Level 1: manual context reload

At a safe Game Thread point, stop callbacks, create a new context/assembly,
recreate Actors from the active World and serialized properties, then resume.
Timers, events, delegates, native callbacks, and static state must not retain the
old context.

### Level 2: state-preserving reload

Only after Level 1 is stable, add explicit migration for fields marked with a
migration attribute or serialized property name. Do not promise arbitrary object
graph preservation.

Do not claim AssemblyLoadContext unload correctness until a test proves that old
contexts and delegates become collectible after reload.

## Developer Tooling

Eventually expose through the existing developer UI/console:

- runtime enabled state and loaded assembly/version;
- Actor count and type/name/ID;
- lifecycle state and last exception;
- per-Actor Tick time;
- pending timer/event/command counts;
- manual script reload;
- stale-handle and ABI diagnostics.

The first milestone can use diagnostics and console commands only; a full editor
is not required.

## Test Plan

### Managed unit tests

Use a normal .NET test project for lifecycle ordering, property conversion,
input edge semantics, timer cancellation/owner cleanup, event cleanup, and handle
wrappers.

### Native/module tests

Register through the repository GoogleTest helper for ABI layout/version checks,
stale-handle rejection, command ordering, Actor registry behavior, exception
conversion, scene Actor validation, and ScriptWorldContext isolation.

### Runtime test

Add a serial runtime test only when module tests cannot cover the behavior. It
must load `Gameplay.dll`, instantiate a scene Actor, observe `BeginPlay`, run
several frames, verify script-driven Transform and WorldSnapshot changes, load a
second World, verify old `EndPlay` and handle invalidation, and verify no old
timer or callback fires.

## Implementation Phases

### Phase 0: contracts and host spike

- Confirm supported .NET runtime/SDK and Windows deployment model.
- Add an isolated managed hello-world host spike outside gameplay integration.
- Freeze ABI versioning, handle generation, UTF-8, error, and shutdown rules.
- Decide one default assembly versus multiple assemblies.

Exit criteria: native code loads and calls managed code, reports a managed
exception, and shuts down without Vulkan or World involvement.

### Phase 1: vertical slice

- Add `ScriptRuntimeSubsystem` to `SubsystemCollection`.
- Add managed runtime library and `Gameplay.csproj`.
- Load `Gameplay.dll` during startup.
- Add Actor registration, handles, `OnCreate`, `BeginPlay`, `Tick`, and `EndPlay`.
- Add minimal Transform, Time, Log, and Input APIs.
- Bind one scene Actor and migrate the camera/player movement proof from
  `Controller` to `Player.actor.cs`.

Exit criteria: a `.actor.cs` Actor moves an existing World object in the Vulkan
application, and a failing script does not crash the process.

### Phase 2: World and component foundation

- Add native Actor/Component registries and stable IDs.
- Add scene schema and loader/build-plan support for Actor entries.
- Add component creation/lookup and parent/child Transform hierarchy.
- Integrate candidate ScriptWorldContext into World transactions.
- Define spawn/destroy/add/remove command flush phases.

Exit criteria: World replacement is atomic from the script and renderer
perspective; old handles cannot modify the new World.

### Phase 3: usable gameplay services

- Replace fixed input fields with action/axis maps.
- Add timers and owner-scoped events.
- Add exposed property metadata and scene overrides.
- Add logical asset references and first renderer-facing components.
- Add console/developer diagnostics and per-Actor timing.

Exit criteria: a small sample implements a controller, trigger, spawner, light
controller, and UI-facing state without C++ changes.

### Phase 4: build, deploy, and manual reload

- Integrate managed builds into CMake and runtime output staging.
- Add startup validation for missing SDK/runtime/assembly.
- Add manual script context reload at a Game Thread safe point.
- Prove cleanup of timers, events, callbacks, handles, and old assemblies.

Exit criteria: reload failures preserve the last valid running context or follow
an explicitly documented fallback policy.

### Phase 5: hardening and extensions

- Add explicit state migration.
- Add async asset completion routed to Game Thread.
- Add fixed-step update if physics or deterministic gameplay needs it.
- Add profiling and broader managed API coverage.

Every extension needs module tests, lifecycle ownership rules, and no weakening
of the WorldSnapshot or Vulkan resource boundaries.

## Acceptance Matrix

The first usable release is not complete until:

- C++ builds with the C# feature disabled.
- C# build produces deterministic assembly, dependencies, and PDB output.
- Missing SDK/runtime/assembly gives a structured startup error.
- A scene instantiates an Actor by assembly-qualified type name.
- Lifecycle ordering is tested.
- Input, Transform, Time, Log, Spawn, and Destroy work without raw pointers.
- Actor and Component handles reject stale World generations.
- Public C# API cannot mutate renderer/Vulkan objects.
- Script exceptions are isolated and diagnosable.
- Failed candidate World/script preparation leaves active World unchanged.
- World replacement stops old callbacks and owner-scoped timers.
- WorldSnapshot contains script-driven World changes.
- Existing synchronous and optional render-thread modes retain behavior.
- Manual reload commits a valid replacement or preserves the last valid context.
- GoogleTest, managed tests, and the approved serial runtime test pass.

## Main Risks And Mitigations

| Risk | Mitigation |
|---|---|
| ABI exposes C++ implementation types | Dedicated C ABI and layout tests |
| Old World remains reachable | Explicit context ownership and generation checks |
| Tick mutates containers | Deferred structural command buffer |
| World reload mixes old/new script state | Candidate ScriptWorldContext and one commit boundary |
| C# exception crashes native process | Catch at every managed callback boundary |
| P/Invoke overhead grows per frame | Cached proxies, value snapshots, batching, profiling |
| `.actor.cs` breaks tooling | Explicit project inclusion and standard SDK project support |
| Runtime deployment differs | Runtime validation and staged output policy |
| Reload leaves old assembly loaded | Collectibility tests and callback/timer cleanup audit |
| C# gains renderer authority | API review rule forbidding render-owner/Vulkan references |

## Deferred Decisions

Phase 0 must decide .NET version and deployment mode, hostfxr/CoreCLR details,
assembly topology, registration versus reflection, managed-only versus native
Actor representation, post-commit `BeginPlay` failure policy, property storage
location, fixed-step requirements, and the manual reload command.

## Documentation Follow-Ups

When Phase 2 becomes current behavior, update:

- `documents/architecture/vulkanlearn-architecture.html` with Script/Gameplay
  ownership and thread rules;
- `documents/architecture/coding-guidelines.md` with ABI and managed API rules;
- `schema/scene.schema.json` and schema documentation;
- `README.md` with C# SDK setup, build, run, and deployment instructions.

Until then, this plan is the future-facing proposal; the existing V1
architecture document remains the current implementation contract.
