# Cloth Ashikhmin Runtime LUT

Generates the runtime constants and the 256x256 LUT atlas used by
`Cloth` / Ashikhmin 2000 forward materials.

From the code-worktree root:

```powershell
py -3 tool\cloth-runtime-lut\generate_cloth_ashikhmin_lut.py
py -3 tool\cloth-runtime-lut\write_cloth_lut_exr.py --output ..\VukanLearnResources\Common\Source\Textures\T_clothAshikhminLut.exr
```

The first command writes `shader/glsl/common/clothAshikhminConstants.glsl`; the
second writes the EXR atlas referenced by
`Common/Textures/T_clothAshikhminLut.json`. Both use the same paper-default
parameters and quadrature as the offline `ashikhmin*.cpp` evaluators.
