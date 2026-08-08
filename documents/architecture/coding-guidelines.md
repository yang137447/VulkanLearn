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

## Input Validation Boundaries

Validate external or authored input once at the boundary that owns its interpretation. Examples include console command parsers, JSON loaders, asset importers, and future network-message decoders.

- Reject invalid syntax, non-finite numeric values, and out-of-contract ranges before publishing runtime commands or runtime data.
- After a value crosses that boundary, downstream engine and rendering code should trust the established contract instead of repeating the same validation in every layer, frame update, or shader.
- Document important downstream preconditions near the public API when they are not obvious from the type system.
- Every new producer of the same runtime data must perform the required validation before handing the data to shared downstream consumers.
- Add a downstream check only when the code crosses a new independent trust boundary or when it protects memory safety from corrupted state; do not add redundant defensive checks by default.

For example, the SpeedTree `windstrength` console command validates that its value is finite and inside `[0, 1]` before creating a runtime command. `SpeedTreeWindProfileSet` then consumes the normalized value under that contract without checking the same range again.

