# Coding Guidelines

This document records general coding conventions that should apply across engine modules.
Feature-specific documents may add narrower rules, but should not redefine these base rules.

## Header Comments

New C++ headers that introduce a public class or shared data structure should include a short responsibility comment near the `class` or `struct` declaration.

The comment should explain:

- what the type is responsible for
- where its input data comes from
- who consumes its output or result
- what the type intentionally does not do

Keep these comments practical. They should describe module boundaries and ownership decisions, not repeat member names or obvious syntax.

Example shape:

```cpp
// Resolves raw asset JSON into the effective asset data consumed by validation.
// It does not read files, import GPU resources, or create runtime scene objects.
class ExampleAssetResolver
{
public:
    ...
};
```

Use this rule especially for loader, resolver, validator, factory, cache, and runtime resource classes, because their boundaries are easy to blur as the renderer grows.

## Chinese Comments And Migration Fidelity

New or modified non-obvious engine, rendering, and shader code must include concise Chinese comments. Keep API names, type names, shader symbols, and standard graphics terms in English when that is clearer, but explain the design intent in Chinese.

Comments are especially required for:

- ownership and lifetime boundaries
- Vulkan synchronization and GPU/CPU publication timing
- coordinate-space, handedness, normal, tangent, UV, and texture-channel conventions
- shader formulas, approximations, masks, energy splits, and static branch rationale
- source-data assumptions and intentionally omitted runtime validation
- deliberate compatibility behavior or performance tradeoffs

Migration work has a stronger requirement: read the source comments together with the surrounding implementation before rewriting the code. Recover the reason behind the source logic, then preserve that useful intent as Chinese comments near the migrated implementation.

- Do not remove meaningful source comments without restoring their verified design intent.
- Do not mechanically translate comments that no longer match the migrated behavior.
- Rewrite source-specific descriptions around VulkanLearn's actual MF, Shading Model, MeshPass, resource, and ownership boundaries.
- When the migrated implementation intentionally differs from the source, add a Chinese comment explaining what changed and why.
- Preserve non-obvious source conventions such as UV transforms, coordinate directions, channel meanings, formula inputs, special-case branches, and known limitations.
- Do not add comments that merely restate syntax or obvious assignments.

For example, a NeoX shader migration should not only reproduce a UV transform formula. It should also retain the verified intent that UV0 controls coverage and fake-sphere normals while a real UV1 controls 2U MatCap sampling, and it should explain when Billboard geometry has been baked offline instead of reproduced at runtime.

## Input Validation Boundaries

Validate external or authored input once at the boundary that owns its interpretation. Examples include console command parsers, JSON loaders, asset importers, and future network-message decoders.

- Reject invalid syntax, non-finite numeric values, and out-of-contract ranges before publishing runtime commands or runtime data.
- After a value crosses that boundary, downstream engine and rendering code should trust the established contract instead of repeating the same validation in every layer, frame update, or shader.
- Document important downstream preconditions near the public API when they are not obvious from the type system.
- Every new producer of the same runtime data must perform the required validation before handing the data to shared downstream consumers.
- Add a downstream check only when the code crosses a new independent trust boundary or when it protects memory safety from corrupted state; do not add redundant defensive checks by default.

For example, the SpeedTree `windstrength` console command validates that its value is finite and inside `[0, 1]` before creating a runtime command. `SpeedTreeWindProfileSet` then consumes the normalized value under that contract without checking the same range again.

## Test Governance

Treat test code as owned production-support code: it must have a clear module
owner, a narrow purpose, and a stable execution path. Do not add tests merely
because a local experiment needs a one-off executable or a broad regression
bucket.

### Module Tests

- Implement pure-logic, asset-contract, and module-integration tests with
  GoogleTest. Register each target through the root
  `vulkanlearn_add_gtest(...)` CMake helper so CTest discovers every `TEST`
  independently.
- Do not add a test-only `main()` function or a direct `add_test(...)` entry.
  A real command-line tool may keep its production `main()`, but its automated
  coverage must call focused GTest cases rather than invoke a second custom
  test runner.
- Put a new assertion in the nearest existing module test suite whenever that
  module already has one. Create a new test target only for a distinct module
  boundary with its own dependencies and ownership.
- Keep one `TEST` focused on one observable behavior or contract. Split
  unrelated scenarios so failures are diagnosable and GTest filtering remains
  useful.
- Fixtures must create and clean up only the resources they own. Do not rely
  on test ordering, shared mutable files, or stale generated outputs.

### Runtime Tests

Runtime validation is intentionally exceptional. The supported engine runtime
commands are limited to:

- `--shader-reload-test`
- `--shader-compute-reload-test`
- `--world-graph-transaction-test`

Do not add, restore, or expand runtime test commands without explicit user
agreement and a written explanation of why a GoogleTest module test cannot
validate the behavior. These runtime tests share `shader/spv/`, require serial
execution, and exist only for end-to-end Vulkan publication and transaction
paths that cannot be represented safely as module tests.
