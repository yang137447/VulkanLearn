# Houdini / Pegasus 地形离线导入

这套工具按 Project Pegasus 的离线烘焙思路接入 Houdini 地形，不把
Houdini Engine/HAPI 作为运行时依赖：

```text
Houdini heightfield + masks
  -> 16-bit PNG + 8-bit VLP1 权重
  -> TR / SC / MI + texture arrays
  -> VulkanLearn 地形编辑器
```

Pegasus 本身是 SideFX 的 Houdini + UE5 教程工程，不是可安装的运行时 SDK。
官方 `.hip`/`.hda` 需要 Houdini 商业或 Indie 授权；本机 Houdini 19.5
当前没有可用 license（`localhost:1715` 无授权服务），所以本工具只处理
**已经导出的真实数据**，不负责 cook `.hip`。

## 数据来源

当前 fixture 来自已有的 G66 工程，不是 Pegasus 官方资产：

```text
D:\YYBWorkSpace\G66\res\scene_bw\bigworld\export\
  Heightmap.png              4065x4065, 16-bit grayscale
  Layer_g_layer0..3.png      4065x4065, 16-bit grayscale, 归一化地表权重
```

世界尺寸和高度范围来自同目录的
`terrain_swap\ue_to_houdini\bg43.bgeo.sc`。`gstat` 可读到：

```text
height volume: [4065, 4065, 1]
Size:   2048.5, 512.707, 2048.5   (m)
Bounds: -2464.25, -462.525, 6815.75
        -415.748, 50.182, 8864.25
```

这些值写在 `houdini_pegasus_source.json`。脚本会先核对 PNG 分辨率与
`bgeoResolution` 一致，再生成导入清单，避免把不同 tile 的高度和尺寸拼错。

**许可提醒：** G66 数据只用于本地管线验证，不要随发行包分发。官方
Pegasus 教程资产有独立授权条款，不能因为“能下载”就当作可再分发资产。

## 生成资产

```powershell
py -3 tool\houdini-terrain-import\prepare_pegasus_import.py `
  --check-only

py -3 tool\houdini-terrain-import\prepare_pegasus_import.py

py -3 tool\terrain-ue-import\build_vulkanlearn_assets.py `
  D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources\Generated\HoudiniPegasus\houdini_pegasus_import.json `
  --resource-root D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources `
  --verbose
```

输出位于资源 worktree：

```text
Generated\HoudiniPegasus\
  TR_terrain_houdini_pegasus_bg43.json
  SC_terrain_houdini_pegasus_bg43.json
  MI_terrain_houdini_pegasus_bg43.json
  Source\heightmap.png
  Source\terrain_houdini_pegasus_bg43_paint.vlpaint
  Textures\...
```

`Source\heightmap.png` 是 16-bit PNG；`paint.vlpaint` 是 VLP1，把 Houdini
的 `g_layer0..3` 直接烘成 2048² 的权重覆盖层。运行时不需要新增外部 mask
格式，现有 Paint 覆盖层路径就能消费这套权重。

## 当前限制

- 地面材质使用 `Generated\KiteDemo\Textures\Source\layer_XX_*` 的真实
  PBR 纹理作为占位。G66 XML 引用的原始 `.tga` 不在本机资源中；在找到可
  授权纹理前，不把占位纹理说成 Houdini/Pegasus 原始材质。
- fixture 只用了 4 张归一化 `g_layer` mask。`Layer_water`、`Layer_sediment`、
  `Layer_curvature` 等仍是导出备用数据，尚未映射成运行时层。
- 2026-11-04 的 Houdini 安装没有 license，因此没有从 `.hip` 重新 cook；
  世界尺寸与高度范围来自 bgeo 缓存元数据，而不是脚本内猜值。
- 这是离线烘焙管线，不是运行时 HAPI 接入。

## 验证

```powershell
.\build\bin\terrain_tests.exe
.\build\bin\editor_ui_tests.exe --gtest_filter=*Terrain*
py -3 tool\terrain-ue-import\build_vulkanlearn_assets.py --self-test
py -3 tool\validate-docs.py
```
