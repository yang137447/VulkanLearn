# Lookdev 资产审阅场景

> 状态：当前契约。2026-10-12 起只保留文章版，旧合成校准场景已移除。

## 正式入口

`Maps/SC_lookdev_article/SC_lookdev_article.json`，显示名称为 `SC_lookdev_article`。
场景与专属 Meshes、Materials、Textures、Source 都归属该目录，资产浏览器可直接发现。
`Generated/` 只保存导入清单、日志与截图，不存正式场景入口。

## 模型与照明

模型使用 UE 导出的 `Source/UE_AssetsPreview/SM_ColorCalibrator.fbx`，只导入
`SM_ColorCalibrator` 可见 section，排除碰撞代理。材质保留五个导出槽位的顺序，
由 VL 的 M_pbr 材质实例显式重建，不宣称 UE 材质图的等价迁移。
环境唯一输入是 `Common/Environments/UE_AssetsPreview/EpicQuadPanorama_Gray.hdr`，
与模型编辑器共享同一份源资产，强度为 1。
没有额外方向光；场景使用厘米单位，模型位置、旋转与比例由生成器保留 UE 基底转换结果。

相机位置 `[-660,300,-355]`，朝向 `[-105,41,35]`，水平 FOV 48 度，EV100=0、补偿 0。
启动和截图脚本设置 `tonemap 3`、`bloom strength 0`。浏览器切换场景不会自动更改
这些全局显示设置。该场景用于文章外观审阅，不是认证色卡或数值校准，不含 TAA/FSR。

## 作者工具

- `tool/lookdev/convert_assets_preview.ps1`：导出并导入 UE 资源，生成正式场景。
- `tool/lookdev/import_ue_export.py`：写入本场景的源资产、材质和网格描述。
- `tool/lookdev/build_ue_preview_scene.py`：生成正式场景 JSON。
- `tool/lookdev/build_scene.py`：兼容入口，转调文章场景生成器，不再生成合成校准场景。
- `tool/lookdev/run_lookdev.ps1`：打开文章场景并设置显示预设。
- `tool/lookdev/capture_lookdev.ps1`：采集文章场景截图，不支持已移除的 direct 校准场景。

旧合成场景的灰片像素阈值不适用于文章版，不以旧 `verify_lookdev.py` 的数值结论验收文章版。
`SC_model_preview` 保留独立的模型预览用途，其背板、材质与平板 OBJ
归属 `Maps/SC_model_preview/`。照明和反射复用上述 Epic HDRI（强度 1），
不再使用旧合成 `studio_neutral.hdr`。灰色 Unlit 背板仅决定背景，不是照明源。
