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

