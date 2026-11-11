# UE/KiteDemo 地形资产导入

这套工具把 UE 的非交互导出结果转换成 VulkanLearn 的 `TR` / `SC` / `MI`
和 texture-array 资产。

## 先确认数据来源

`Open World Demo Collection` 的两张 KiteDemo 地图（`Overview`、`Showcase`）
没有 `ALandscape` actor。地图里的 `StaticMeshActor` 是资产展示场景，不是地形。
因此当前 `Generated/KiteDemo` 使用：

- 从 UE 实际导出的 9 组 GroundTiles albedo / normal / height 贴图；
- 程序生成的 2048² PNG16 高度场；
- 基于同一高度/坡度场生成的 512² × 9 层 VLP1 权重。

生成的 `TR` / `SC` description 已明确记录这一点。不要把程序化高度场说成
UE Landscape 导出数据。

## 1. 从 UE 导出

如果输入地图真的包含 Landscape：

```powershell
C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe `
  D:\YYBWorkSpace\UnrealProject\landscape\landscape.uproject `
  -ExecutePythonScript=tool\terrain-ue-import\export_landscape.py `
  -unattended -nullrhi -nosplash -NoSound
```

脚本会输出高度图、层权重、manifest，并可直接导出指定纹理资产。
它只接受资源 worktree 下的输出目录，并显式拒绝 `effect` 工程。

导出纹理时优先使用 `AssetTools.export_assets`，保持源文件位深；当前 KiteDemo
的 `_H` 贴图本身是 8-bit，转换器会用 `value * 257` 升到 PNG16，并保留真实
16-bit 高度图的全部样本。

## 2. 生成 VulkanLearn 资产

```powershell
py -3 tool\terrain-ue-import\build_vulkanlearn_assets.py `
  D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources\Generated\KiteDemo\kitedemo_manifest.json `
  --resource-root D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources `
  --texture-resolution 1024 --verbose
```

转换器会：

- 检查高度图是否为 16-bit PNG（或受支持的 8-bit PNG/scanline EXR）；
- 统一 texture-array 尺寸，并检查所有源图格式/引用；
- 权重图接受 8-bit 或 16-bit 灰度 PNG；16-bit 按 `value / 257` 归一化，
  避免 `Image.convert("L")` 截断低 8 位；
- 把 DirectX normal 的绿色通道翻成 VulkanLearn 的 OpenGL 约定；
- 写入同序 `displacementSources`，只接受 PNG16 灰度位移源；
- 为每个层生成 albedo / normal / PBR / height16；
- 写出 `TR_terrain_kitedemo.json`、`SC_terrain_kitedemo.json`、
  `MI_terrain_kitedemo.json`、三张 texture-array JSON 与 VLP1；
- 使用原子写入，重复运行应报告 `filesChanged: 0`。

清单可选 `assetName` / `outputRelative` 生成 KiteDemo 之外的资产。
Houdini heightfield 的离线导入见
`tool/houdini-terrain-import/README.md`。

当前演示规模为 `size=[20000,20000]cm`、`gridResolution=2048`、
`chunkQuads=256`、2 级 LOD，共 64 个地形 chunk；几何高度图 1:1 采样，
不再用低分辨率网格表现 16-bit 高度。

## 3. 验证

```powershell
.\build\bin\main.exe --dev-ui --focus-window `
  --initial-scene Generated/KiteDemo/SC_terrain_kitedemo.json
.\build\bin\terrain_tests.exe
.\build\bin\editor_ui_tests.exe --gtest_filter=*Terrain*
py -3 tool\validate-docs.py
```

`assets` 输出位于 sibling resources worktree；不要在主 checkout 的 `resources/`
下生成运行资产。
