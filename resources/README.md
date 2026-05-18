# External Resources

This repository no longer stores runtime assets under `resources/` in git.

Runtime asset lookup now expects the full resources tree at:

```text
../VulkanLearnAssets/resources
```

That path is resolved relative to the repository root's parent directory.

Expected external layout:

```text
VulkanLearnAssets/
  resources/
    scenes/
    models/
    materials/
    textures/
    hdri/
```

Notes:

- Keep this `README.md` in the repository as a placeholder only.
- Generated local outputs may still appear under `resources/generated/`.
- Existing tracked resource files must be removed from git history/index separately if you are migrating them out of the repository.
