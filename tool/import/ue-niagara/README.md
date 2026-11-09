# UE Niagara Asset Import

These scripts export the reference UE 5.8 Niagara assets and build VulkanLearn
resource descriptors. They are the reproducible source for the real skeletal
meshes and effect textures under the sibling `VukanLearnResources` worktree.

## Inputs

- UE 5.8: `C:\Program Files\Epic Games\UE_5.8`
- Reference project: `D:\YYBWorkSpace\UnrealProject\effect\effect.uproject`
- VL resources: `..\VukanLearnResources`

## Order

1. `export_niagara_assets.py` exports the character, Dino, ToyCow, rifle and
   effect source assets from UE.
2. `export_dino_lod1.py`, `export_manny_lod1.py`, `export_quinn_lod1.py`
   export the low-LOD skeletal particle sources.
3. `build_vl_skeletons.py` and `build_vl_assets.py` generate VL
   `SK_*.json` / `SM_*.json` / texture / material descriptors.
4. `apply_real_assets.py` switches galleries from fallback proxies to the
   imported assets.
5. `export_fx_textures.py` + `build_fx_texture_descriptors.py` +
   `apply_fx_textures.py` export and bind the UE effect textures.

`manifest.json` and `fx_texture_manifest.json` record the UE source paths and
generated VL targets. The scripts are intentionally outside the resource
worktree so the generated export cache remains gitignored.
