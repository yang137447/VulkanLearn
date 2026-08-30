# Material Instance Asset Editor

## Status

**Current status (2026-08-29): renderer-independent editor baseline, the
`renderMode`/`shadingModel`/`cullMode` MI editing path, renderer-owned numeric
live preview, and renderer-owned render-state live replacement are connected.
Texture live preview is not connected yet.**

This document is the formal contract for the Material Instance Asset Editor
in VulkanLearn's Dear ImGui developer layer. It migrates the stable P0
boundaries from
`documents/plan/rendering/material-instance-imgui-editor-plan.md`; the plan
remains the execution record and should not be used as a second runtime
contract.

The editor is an asset-document tool, not a second live material system. A
scene object is only a navigation origin. The `MI_*.json` document owns the
draft, validation, sparse candidate, conflict decision, and save lifecycle.
The active World and renderer provide an optional preview target.

The following implementation pieces are now present:

- `source/editor/command/` contains the typed command specification, bounded
  `EditorCommandBus`, lower-case JSON codec, payload validation, command-id
  de-duplication, and document-revision gating;
- `source/editor/service/materialInstanceDocumentService.*` provides the
  renderer-independent document session, navigation-context storage, typed
  parameter/texture/render-state editing, validation, sparse candidate
  construction, BLAKE3 conflict detection, and atomic save;
- `source/editor/runtime/materialInstanceEditorRuntime.*` and
  `source/editor/ui/materialInstanceAssetEditorPanel.*` provide the current
  game-thread facade and ImGui panel, including parameter, texture, and
  supported render-state controls, wired into `UiSubsystem` initialization,
  ticking, snapshot publication, and `Ctrl+S`;
- `source/editor/preview/` contains the value-semantic preview types,
  generation-aware controller, explicit `Unavailable` adapter, and the
  renderer-owned numeric adapter;
- `tool/material-instance-editor-tests/` contains fixture contract tests and
  direct landed-interface tests, and is registered by the root CMake test graph.

The current runtime facade routes the document/list/select/read/edit/reset/
revert/reload/validate/save commands and the numeric
`material.preview.connect`/`apply`/`restore_baseline` path. `EngineLoop` injects
the renderer-owned preview adapter, which captures the active World-local MI
and applies complete numeric or render-state drafts synchronously at the
stable frame boundary. Numeric-only drafts update the captured MI in place;
render-state changes prepare and atomically swap the affected variant,
pass/material routing, descriptors, graphics pipeline, and RenderScene
material groups. Texture live preview still has no descriptor replacement
transaction; texture-inspector opening, event listing, and some navigation host
routes remain integration gaps.

The renderer-independent document service and ImGui command producer already
define typed `renderMode`, `shadingModel`, and `cullMode` edits. Direct
runtime-facade routing for those Set/Clear commands must remain an explicit
integration check; a test must not treat an unsupported facade command as a
successful owner/session preview. The current focused production test uses
the supported batch service route to create the draft, then exercises the
public preview adapter boundary.

The focused production runtime test drives the same public adapter seam with
each of `renderMode`, `shadingModel`, and `cullMode`. It verifies that the
complete serialized draft reaches preview preparation, an injected CPU fake can
surface a prepare failure without discarding the dirty document, and baseline
restore does not mutate the asset draft. The renderer-owned adapter separately
implements the successful live resource replacement path; this focused test is
not proof of a Vulkan live resource swap.

## Scope

P0 supports:

- locating an MI from a scene mesh section or terrain material slot;
- opening an MI independently of whether its World is loaded;
- editing `float`, `vec2`, `vec3`, and `vec4` values;
- selecting an existing `T_*.json` texture asset for a material texture slot;
- editing the three supported MI render-state overrides: `renderMode`,
  `shadingModel`, and `cullMode`;
- Override, Clear/Reset, Reset All, Revert, Validate, Save, Reload, and
  `Ctrl+S` semantics;
- sparse JSON generation, source-digest conflict detection, and atomic replace;
- an optional typed preview bridge to a same-path live `MaterialInstance`;
- one versioned command and result protocol shared by ImGui, Console, AI, and
  runtime tests.

P0 does not edit texture import settings, raw image paths, material macros,
render-state fields outside `renderMode`/`shadingModel`/`cullMode`, pipeline
variants directly, object references, RenderGraph pass materials, or
preview-scene/picking/undo systems. Editing those three render-state fields is
an Asset Document capability; live render-state preview is a separate
renderer integration gate.

The scope above is the P0 contract. The currently wired subset and the
remaining integration gates are recorded in **Status** and **Integration
Gates** below.

## UI Boundary

The editor is part of the optional Dear ImGui developer layer described by
`documents/architecture/game-ui-stack.md`. It must follow the same ownership
rules:

```text
ImGui / Console / AI / Runtime Test
    -> EditorCommandEnvelope
    -> EditorCommandBus
    -> MaterialInstanceEditorRuntime (game-thread drain)
    -> MaterialInstanceDocumentService
    -> EditorCommandResult / EditorSnapshot
    -> optional Preview adapter
```

ImGui callbacks are producers only. They may keep presentation state such as
search text, folded sections, hover state, and window layout, but they may not
modify an asset document, call a live `MaterialInstance`, write a file, or
touch Vulkan objects. The panel currently submits through the runtime facade;
the command protocol and codec are shared by the fixture/landed tests, while
Console and AI adapters still need host integration. No producer may bypass
the command boundary by translating through `UiAction` or `RuntimeCommand`.

The panel should expose three conceptual areas:

- **Asset Navigation**: scene/mesh/terrain breadcrumb, MI browser, and known
  navigation origins;
- **Asset Details**: General, render states, scalar/vector parameters, and
  texture bindings;
- **Operation/Preview Status**: dirty state, validation, save/conflict state,
  preview connection, and the last structured result.

## Command Contract

### Envelope

The external JSON form is versioned and contains only serializable values:

```cpp
struct EditorCommandEnvelope
{
    uint32_t protocolVersion;
    uint64_t commandId;
    std::optional<uint64_t> correlationId;
    EditorCommandSource source;
    EditorCommandType type;
    std::optional<uint64_t> expectedDocumentRevision;
    EditorCommandPayload payload;
};
```

The wire form uses stable lower-case names. `commandId` is non-zero and is used
for result lookup and de-duplication. `correlationId` groups a multi-command
operation but does not alter behavior. `source` is audit metadata only.
`expectedDocumentRevision` is required on draft-mutating commands and rejects
stale producers before execution. Payloads contain no pointers, callbacks,
lambda captures, Vulkan handles, or live object identities.

The P0 command names are:

```text
material.resolve_scene_reference
material.list_assets
material.open
texture.open
material.select
material.close
material.get_document
material.get_reference_context
material.set_parameter
material.clear_parameter
material.set_texture
material.clear_texture
material.set_render_state
material.clear_render_state
material.reset_overrides
material.validate
material.save
material.revert
material.reload
material.preview.connect
material.preview.apply
material.preview.restore_baseline
material.preview.disconnect
editor.get_command_result
editor.list_events
editor.execute_batch
```

`material.set_parameter` carries the explicit type and a complete value array.
`float` uses one component; `vec2`, `vec3`, and `vec4` use exactly two, three,
and four components. Every component must be finite. A vector edit is one
typed command, not a sequence of scalar actions.

Asset paths are normalized to resource-root-relative generic paths before the
executor consumes them. Traversal, absolute paths, and raw image paths are
rejected. Texture bindings accept only a normalized `T_*.json` path.

Render-state commands use typed field/value payloads and serialize stable names
rather than enum ordinals:

```text
field: renderMode | shadingModel | cullMode
value: an allowed value for the selected field
```

`ResetMaterialInstanceOverrides` accepts `render_states` in addition to
`parameters`, `textures`, and `all`. A value equal to the M_ default clears the
override and has no sparse representation.

### Results

Every command produces one structured result:

```cpp
struct EditorCommandResult
{
    uint32_t protocolVersion;
    uint64_t commandId;
    EditorCommandStatus status;
    EditorErrorCode errorCode;
    std::string message;
    std::optional<uint64_t> documentRevision;
    EditorCommandResultPayload payload;
};
```

The stable status tokens are `Accepted`, `Running`, `Succeeded`, `Rejected`, and
`Failed`. Only the last three are terminal. `Rejected` means that execution
did not start because a precondition such as revision, dirty policy, or target
identity was invalid. `Failed` means execution started but asset loading,
validation, preparation, or writing failed. Consumers branch on `errorCode`,
never on diagnostic prose.

The initial error-code vocabulary is:

```text
None
InvalidProtocolVersion
InvalidCommandId
InvalidCommandType
InvalidPayload
MissingExpectedDocumentRevision
StaleDocumentRevision
DuplicateCommandId
ResultStoreCapacityExceeded
AssetNotFound
InvalidAssetType
ReferenceResolutionFailed
DocumentNotOpen
DocumentDirty
UnknownParameter
ParameterTypeMismatch
UnknownTextureSlot
InvalidTextureAssetReference
SourceChanged
ValidationFailed
AtomicWriteFailed
PreviewUnavailable
PreviewGenerationChanged
PreviewPrepareFailed
PreviewCommitFailed
```

The protocol and bus support `accepted` and `running` intermediate states for
future asynchronous operations. The current runtime facade executes its
supported document commands synchronously while draining the queue in `Tick`,
so those commands normally publish a terminal result in the same tick. Numeric
and render-state preview have connected renderer-owned paths. Numeric drafts
update the captured MI in place, while render-state drafts use an isolated
candidate resource package and commit the complete replacement only after
variant, pass, material, descriptor, graphics-pipeline, object-binding, and
RenderScene validation succeeds. Texture preview still requires its own
descriptor replacement transaction.
`editor.list_events` is not connected to an event publisher.
`editor.get_command_result` remains the authoritative query path; once a command
reaches a terminal state, repeated queries return the same terminal result and
cannot be overwritten by a late update.

## Navigation

The supported reference chain is:

```text
scene*.json object
  -> modelPath / terrainPath
  -> SM_*.json / TR_*.json
  -> mesh section / terrain material slot
  -> materialInstancePath
  -> MI_*.json
  -> M_*.json + T_*.json references
```

The document service can produce a normalized breadcrumb for
`material.resolve_scene_reference`, containing the scene, object identity/type,
mesh or terrain asset, section/slot selector, and MI path. The current runtime
facade has not routed that command yet, so Scene Outliner reveal remains an
integration gate. `material.open` accepts an optional navigation origin; origins
are diagnostic context and do not participate in document identity. An MI
opened from the browser remains usable when no current World references it.

P0 reports only the origins known to the current scene/navigation context. It
must not claim a complete project-wide reverse-dependency index.

## Asset Schema And Authoring

The M_ asset remains the schema authority described by
`documents/rendering/material-param-authoring-and-reflection.md`:

- parameter names, types, defaults, and optional channel metadata come from
  `M_*.json`;
- supported authoring types are `float`, `vec2`, `vec3`, and `vec4`;
- channel metadata is ordered `x/y/z/w` and may provide name, description, and
  finite min/max ranges;
- texture slot names and default `T_*.json` bindings come from the M_ schema;
- `MaterialDescriptorSchema` supplies deterministic Set 1 layout and active
  reflection determines which texture bindings a selected pass actually uses.

Render-state defaults come from the M_ material definition: `shadingModel` is
the material-root field, while `renderMode` and `cullMode` are render-state
fields when declared by the definition. The MI document stores explicit
changes under `renderStateOverrides`. The editor exposes the effective value,
default value, and optional override for the three supported fields. Basic
field/value membership, known-key, and redundant-default checks are performed
at the document boundary. The renderer's `MaterialInstanceValidator` remains
the authority for combinations that affect pass routing, shader variants, or
pipeline contracts.

The editor displays the effective value as `MI override > M_ default`. An
inactive reflection texture is still a valid authoring slot and can be saved;
inactive is a display/diagnostic state, not a reason to discard a binding.

Numeric controls remain editable while their value is inherited from M_. The
first non-default edit creates the MI override, while Reset or editing exactly
back to the M_ default removes it through the sparse-save rule. The UI must not
require an explicit default-valued override as an editing gate because such an
override has no persistent representation. The parameter table therefore has
no override-enable checkbox; an emphasized Reset action indicates that the MI
currently owns an override for that parameter.

Parameter names are not inferred to be colors. Color controls require explicit
future metadata.

All candidate values go through the shared material validator plus the
renderer-free document-level type, finite-value, channel-range, and texture
reference checks before Save. The production preview/material path must still
apply any pass-specific `MaterialDescriptorSchema` and reflected binding checks
when it supplies a live target. The editor must not implement a second range,
type, texture-reference, or render-state-combination validator for another
producer. The shared validator rejects incompatible effective
`renderMode`/`shadingModel` pairs before Save; a later renderer prepare step can
still reject pass-specific routing or descriptor/reflection constraints.

## Document Session

An open document is keyed by one normalized MI path. Opening the same path from
another origin focuses the existing document and adds origin context; it never
creates a second baseline or working copy. A snapshot contains:

- normalized MI and referenced M_ paths;
- the original MI JSON and schema digest;
- baseline and working parameter, texture, and render-state overrides/effective
  values;
- dirty state, revision, validation diagnostics, and save/conflict state;
- known navigation origins.

Preview connection state is separate and contains active World identity, live
resource generation, connection state, and the latest preview diagnostic. It
does not change document baseline/dirty state and does not decide whether the
asset can be saved.

The minimum document state transitions are:

```text
Closed -> Clean -> Dirty -> Saving -> Clean
                     |          -> SaveFailed
                     -> SourceChanged
```

`Saving` is reserved for an asynchronous document implementation. The current
service exposes `Clean`, `Dirty`, `SaveFailed`, and `SourceChanged` and finishes
save work during the owning runtime tick. Each successful state-changing
command increments `documentRevision`; read commands do not. A stale write is
rejected without changing the document. A dirty close requires `Save`,
`Discard`, or `Cancel`. World replacement keeps the document open; the current
facade updates the preview diagnostic, while disconnect/reconnect is pending
the live adapter.

When the M_ schema digest changes, the document enters `SourceChanged` and
cannot silently apply old parameter types or texture slots to the new schema.
The user must explicitly Reload/Merge according to the production document
implementation.

## Sparse Save And Conflict

Save builds a candidate from the original MI JSON captured when the document
opened:

1. copy the original JSON;
2. patch only editor-managed `parameters`, `textures`, and
   `renderStateOverrides` keys;
3. remove a parameter override when its working value exactly equals the M_
   default;
4. remove a texture override when its working `T_*.json` path equals the M_
   default;
5. remove a render-state override when its working value equals the M_ default;
6. remove an empty `parameters`, `textures`, or `renderStateOverrides` object;
7. preserve `type`, `name`, `material`, macros, accepted extension fields, and
   unmanaged JSON data.

Equality is exact after parsing the float components. No hidden epsilon may
silently erase a user value. The generated candidate is validated before any
write.

The write path is:

```text
validate candidate
  -> re-read MI and compare BLAKE3-256 source digest
  -> SourceChanged: reject and leave file untouched
  -> write sibling temporary file
  -> flush and close
  -> atomic replace original
  -> update baseline and digest only after success
```

The default conflict policy is `Reload / Cancel`; P0 does not provide Force
Save or Save As. Any validation, conflict, or atomic replacement failure must
leave the original file unchanged. Save does not require an active World.

## Preview Bridge

Preview is an optional runtime bridge and never owns the asset document:

```text
ApplyMaterialInstancePreview(path, revision)
  -> EditorCommandBus
  -> EngineLoop stable frame boundary
  -> validate World / MI path / generation / schema
  -> numeric: set complete MaterialInstance value
  -> render state: prepare variant/pass/pipeline resource rebuild
  -> texture: prepare Texture and affected descriptor package
  -> no-throw live swap
  -> retire old resources by GPU epoch
```

`material.preview.apply` identifies the exact document revision. A preview
failure reports `PreviewUnavailable`, `PreviewGenerationChanged`,
`PreviewPrepareFailed`, or `PreviewCommitFailed` as appropriate; it does
not roll back a valid working draft or block Save. Preview must not call a
global `device.waitIdle()`. Numeric changes do not rebuild pipelines or
descriptors. Render-state changes are handled by the renderer-owned adapter in
an isolated candidate transaction: `renderMode` may change pass routing,
blend/depth state, and pipeline identity; `shadingModel` may change the shader
variant and descriptor contract; and `cullMode` changes graphics pipeline
fixed state. The complete affected package is swapped atomically and replaced
resources retire after the completed GPU epoch. Texture changes still require
the not-yet-connected descriptor replacement path.

The renderer-owned numeric adapter is now connected through the public
`MaterialInstanceEditorRuntime::Config::previewAdapter` seam. It captures a
World-local MI session, applies complete numeric drafts, and restores the
captured numeric baseline without changing the Asset Document baseline. The
numeric path is synchronous; the focused tests inject a CPU fake through the
same seam and do not instantiate `RenderSystem`, Vulkan handles, descriptor
packages, or GPU epochs.

The numeric adapter may receive the document's unchanged `textures` object. It compares every retained entry with the connected World-local MI's
captured normalized texture asset identity; malformed or changed texture entries fail at preview
preparation instead of being silently ignored. Actual texture replacement still requires the
descriptor transaction described above.

The renderer-owned session accepts a changed `renderStateOverrides` value and
rebuilds the affected live package transactionally. A failed prepare or commit
leaves the active MI, Material, descriptors, pipeline, and RenderScene groups
unchanged. Saving the draft remains independent and succeeds when document
validation passes, regardless of whether a live preview target is connected.

### Real Owner/Session Test Design

The focused targets do not instantiate `RenderSystem`, Vulkan handles, or the
renderer resource cache. Real owner/session coverage belongs to an
application-level integration target that supplies the active World and
constructs `RendererOwnedMaterialInstancePreviewAdapter` around the actual
`IRendererMaterialInstancePreviewOwner`. The test fixture should use an MI
with a known `M_` baseline and stable material-slot references.

The required matrix is:

| Case | Draft | Owner/session assertions |
| --- | --- | --- |
| Capture baseline | unchanged MI | Capture succeeds only for the active World and normalized MI path; the session freezes document revision, numeric values, texture identities, and effective `renderMode`/`shadingModel`/`cullMode`. |
| `renderMode` replacement | change only `renderMode` | Apply prepares a complete candidate, then atomically replaces pass classification, material/pipeline state, descriptors, and render grouping; a prepare/commit failure leaves the active package unchanged. |
| `shadingModel` replacement | change only `shadingModel` | Apply replaces the shader variant, descriptor schema, shadow contract, and affected live resources atomically; a failure leaves the active package unchanged. |
| `cullMode` replacement | change only `cullMode` | Apply replaces the graphics fixed state and active pipeline atomically; a failure leaves the active package unchanged. |
| Mixed draft | numeric edit plus one state edit | The state edit is not ignored and the numeric value is not partially committed. |
| Restore | captured baseline after a rejected draft | Restore validates the baseline, preserves the Asset Document revision/dirty state, and does not report a live replacement generation. |
| World replacement | change World generation before commit | The old session rejects the operation as stale; no resource from the old World is written into the new World. |

Every failed prepare or commit case must also assert that the old `MaterialInstance`,
`Material`, pipeline, descriptor package, and render grouping remain usable.
Successful state changes atomically swap the complete affected package,
increment the runtime resource generation only after commit, and retire old
resources by GPU epoch. The CPU fake-adapter regression in
`tool/material-instance-editor-tests/` covers the serialized command boundary;
it is not a substitute for the renderer-owned owner/session integration path.

World replacement disconnects the old generation and invalidates pending
operations. The current runtime keeps the document open and requires a new
connect command for the new World generation; it does not silently reconnect a
different live MI. Texture apply/restore remains a gap until the renderer
provides its descriptor replacement transaction. A saved render-state edit
becomes effective on the next World resource load/rebuild; Save does not
silently reload the active World. UI/renderer shutdown must clear pending
preview work before live resources are destroyed.

## Threading And Ownership

The game thread owns the ImGui context, command execution boundary, asset
documents, validation, and preview request preparation. The current document
service runs synchronously at that boundary; no editor worker owns live state.
Future compile/IO workers may parse and prepare immutable data but may not touch
Vulkan, live `MaterialInstance`, `Material`, `PipelineFactory`, or active
document ownership. Only the stable EngineLoop frame boundary may commit a
prepared preview swap.

The render thread consumes immutable UI/render snapshots and never traverses a
live ImGui context or asset document. This extends the existing two-layer UI
contract rather than creating a third UI-to-renderer path.

## Verification Contract

The focused suite is `tool/material-instance-editor-tests/`. The root CMake
registers it when `VULKANLEARN_BUILD_TESTS=ON`, and the directory can also be
configured independently. Its three targets exercise:

- `material_instance_editor_tests`: fixture contract harness for wire/schema,
  navigation, document-session, sparse-save, conflict, and preview-result
  behavior;
- `material_instance_editor_landed_tests`: direct compile-time/runtime checks
  for the landed command types, JSON codec, command bus, persistence, and
  preview value/controller and protocol-boundary APIs;
- `material_instance_editor_production_tests`: production document-service and
  runtime-facade checks, including serialized baseline/working-draft
  regression, public fake-adapter numeric preview connect/apply/restore,
  `renderMode`/`shadingModel`/`cullMode` draft coverage, and
  World-generation invalidation.

Together they exercise:

- command codec/schema, source parity, complete vector arity, path policy, and
  stable result statuses/error codes;
- mesh and terrain navigation breadcrumbs;
- real `MaterialAssetValidator` and `MaterialDescriptorSchema` behavior,
  including channel metadata and active/inactive texture validation;
- sparse candidate generation, exact numeric comparison, empty-object removal,
  and unmanaged-field retention;
- `T_*.json` and source-file checks, missing-source rejection, raw-image
  rejection, and traversal rejection;
- BLAKE3 source conflicts, untouched rejected files, atomic candidate commit,
  and missing-asset errors;
- same-path document singleton, multiple origins, revision rejection, dirty
  close policy, schema change, World change, and preview/result terminal-state
  stability;
- baseline draft equality, dirty-draft preservation, revert, save advancement,
  preview command protocol rejection, public runtime adapter injection, and
  render-state preview draft/restore behavior for all three supported fields.

The focused production target does instantiate the production document service
and runtime facade, but it injects only the CPU fake adapter through the public
configuration seam. It does not link or instantiate the renderer-owned
`RenderSystem` adapter, Vulkan handles, descriptor replacement, or GPU
synchronization; those require application-level smoke coverage. The managed-
document save path validates the editor-owned projection while preserving
opaque source fields; the opaque-field fixture remains a regression case for
that policy.

The owner/session matrix above remains the CPU-test contract because the
focused suite does not instantiate the renderer-owned adapter. Application-level
Vulkan smoke and manual acceptance are still required for live pass
reclassification, pipeline replacement, descriptor replacement, and GPU-epoch
retirement.

The production and landed focused tests were executed with the repository's
MinGW toolchain on 2026-08-29 and passed, including the three render-state
preview draft cases. Single-thread and double-thread application
frame smoke also passed on 2026-08-27 with workerThreadCount=1 and workerThreadCount=2,
both with developer UI disabled and enabled. Manual interactive ImGui acceptance was not performed.

From the repository root, the registered targets can be built and run with:

```powershell
cmake --build build -j2 --target material_instance_editor_tests material_instance_editor_landed_tests material_instance_editor_production_tests
ctest --test-dir build -R "^material_instance_editor_(contract|landed|production)$" --output-on-failure
```

The directory can also be configured independently with:

```powershell
cmake -S tool/material-instance-editor-tests -B build/material-instance-editor-tests -G "MinGW Makefiles" -DCMAKE_C_COMPILER=C:/Software/mingw64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Software/mingw64/bin/g++.exe
cmake --build build/material-instance-editor-tests -j
ctest --test-dir build/material-instance-editor-tests --output-on-failure
```

## Integration Gates

The following are the remaining handoff items for the production integration:

1. Wire the existing scene-reference resolver, batch service, and direct
   render-state Set/Clear methods through `MaterialInstanceEditorRuntime`, and
   decide the host behavior for `texture.open` and `editor.list_events`.
2. Renderer-owned render-state live preview is implemented. The stable-frame
   owner path transactionally rebuilds the affected shader variant,
   pass/material selection, descriptor layout, graphics pipeline, object
   descriptors, and RenderScene material groups in an isolated World-local
   candidate. `renderMode`, `shadingModel`, and `cullMode` commit atomically;
   the adapter reports the replacement generation only after the live swap, and
   the old World-local package retires by GPU epoch.
3. Complete the renderer-owned texture live-preview path. It must resolve the
   active World and same-path live MI at the stable frame boundary, prepare
   affected descriptor packages transactionally, perform a no-throw swap, and
   retire replaced resources by GPU epoch. It must not use a global
   `device.waitIdle()`. The numeric live-preview path is already connected.
4. Complete manual ImGui verification. The focused production and landed
   black-box tests, baseline regression, public fake-adapter runtime checks,
   protocol-boundary checks, and application frame smoke are already present;
   focused tests must remain serial because they share fixtures and generated state.
5. Keep one canonical command route. The current active route is the runtime
   facade; any future executor split must not introduce a second behavior
   implementation or let producers call the service directly.

Until these gates are closed, the landed feature includes asset editing,
`renderMode`/`shadingModel`/`cullMode` live preview, and numeric live preview
through the connected adapter. Texture live preview, `texture.open`, and
`editor.list_events` remain unavailable at runtime.
