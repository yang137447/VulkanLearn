# NeoX Skin 效果对齐合同 V1

## 状态

- **状态**：对齐规则草案；作为 `b_f_3725` 皮肤专项的实现门禁，不能据此宣称效果已完成。
- **目标角色**：NeoX `b_f_3725` 当前 Pose 静态角色。
- **目标槽位**：身体 `b_f_3725_high_0` 与脸部 `nf2022_f_01` Skin 槽。
- **目标 Shading Model**：VulkanLearn 已有的 `PreintegratedSkin` ID 3。
- **源 Shader**：`pbr/pbr_skin.nsf`、`pbr/nodes/pbr_skin_nodes.hlsl`、`pbr/nodes/pbr_skin_parameters.hlsl`、`pbr/nodes/skin_functions.hlsl`。
- **关联公共合同**：`documents/rendering/subsurface-shading-models.md`。

本文定义 NeoX 皮肤从源材质到 VulkanLearn 的可审计规则。它不新增 Shading Model，也不把 NeoX 的整套光照函数直接复制进公共 Skin evaluator。

## 1. 目标与硬规则

目标路径固定为：

```text
NeoX Tex0 / NormalMap / ParamMap / DetailMap / MTG 参数
    -> NeoX Skin Material Functions
    -> 标准 MaterialInputs + PreintegratedSkinInputs
    -> VulkanLearn PreintegratedSkin Shading Model
    -> Forward / Deferred / ShadowDepth / IBL
```

### 1.1 Shading Model 冻结

1. 不新增 NeoX 专用 Shading Model ID。
2. `shader/glsl/engine/preintegratedSkinLighting.glsl` 只维护公共 `PreintegratedSkin` 合同；NeoX 特殊采样、换色和辅助图语义必须留在 `mf_neox*`。
3. NeoX `pbr_skin` 的光照代码只用于确认参数意义、能量分配和视觉目标，不能逐行覆盖 VulkanLearn 的 LUT、GGX 或 GBuffer 合同。
4. 只有发现公共 `PreintegratedSkin` 偏离 `documents/rendering/subsurface-shading-models.md` 时，才允许修改公共 evaluator；修改必须同时通过普通 Skin 测试与 NeoX Skin 测试。

### 1.2 事实源优先级

发生冲突时按以下顺序处理：

1. NeoX 源采样函数与节点执行顺序；
2. NeoX 材质参数和实际槽位有效值；
3. 离线生成的标准纹理资产 JSON 与通道审计；
4. VulkanLearn `M_` / `mf_*` / Shading Model 合同；
5. 固定机位截图。

截图只能证明结果，不能反推出纹理通道、颜色空间、UV 或 RenderState 语义。

### 1.3 结构边界

- `M_preintegratedSkin` 保持通用测试材质和旧资产兼容，不继续堆叠 NeoX 分支。
- NeoX 皮肤应使用独立的 `M_neoxSkin` 与 `mf_neoxSkinTextures`、`mf_neoxSkinInputs`，通过标准 `PreintegratedSkin` 输入接入公共模型。
- `M_` 只负责组合 MF 和 MaterialInputs wiring；MF 不直接读取灯光、不写 GBuffer、不执行 `discard`、不改变 Blend/Cull/Depth 状态。
- RenderState、Pass 路由和排序由材质/Pass 合同决定，不能通过改变 `opacity` 绕过。
- Base、Forward、ShadowDepth 必须共享同一 coverage 语义；Opaque Skin 不得消费 `Tex0.A` 作为透明度。

## 2. NeoX 输入合同

### 2.1 纹理通道

| 源资源 | 通道 | 目标语义 | 采样规则 |
|---|---|---|---|
| `Tex0` | RGB | Base Color，源标记为 sRGB | Clamp；目标 BaseColor 只在此处解码 |
| `Tex0` | A | 源皮肤自发光控制量，源代码以 `1 - A` 生成 emissive mask | 不得进入 Opaque coverage；是否迁移到 Emission MF 需逐槽位确认 |
| `NormalMap` | RG | Tangent-space normal XY | Clamp；`RG * 2 - 1` |
| `NormalMap` | B | Curvature | 线性控制量，进入 `SkinAux.R` 或等价标准化字段 |
| `NormalMap` | A | Detail normal mask | 线性控制量，进入 `SkinAux.G` |
| `ParamMap` | R | Roughness | 线性；不得做 smoothness 反转 |
| `ParamMap` | G | Metallic | 线性；进入 Skin 公共输入 |
| `ParamMap` | B | Skin color / tattoo mask | 线性；不能未经验证直接等同于 SSS weight |
| `ParamMap` | A | Ambient Occlusion | 线性；进入标准 AO |
| `DetailMap` | RG | Detail normal XY | Repeat；由 `u_detail_tilling` 控制 |
| `DetailMap` | B | Pore/detail modulation | 线性；是否影响 curvature 必须保留源质量分支条件 |
| `DetailMap` | A | 预留/源特定辅助通道 | 未确认前不得加入最终光照 |

离线拆分为 `SkinParam`、`SkinAux` 和 `DetailNormal` 时，必须保留源路径、通道描述、颜色空间、地址模式、转换版本和源槽位清单。缺图不能静默绑定默认纹理。

### 2.2 参数与范围

以下参数只能来自 MI 或公共合同默认值，不能在 shader 中按角色名称硬编码：

| 参数 | 源起点 | 目标职责 |
|---|---:|---|
| `u_roughness` | `0.4` | Skin 平均粗糙度起点 |
| `u_specular` | `0.5` | Skin 高光颜色/强度起点 |
| `u_metallic` | `0.05` | 仅在源槽位确有模型覆盖时启用 |
| `u_detail_tilling` | `60` | DetailMap UV 频率 |
| `u_skin_detail` | `1` | Detail normal 强度 |
| `u_curvature_curve` | `1` | 曲率曲线 |
| `u_curvature_intensity` | `1` | 曲率强度 |
| `u_skin_color` | `[1,1,1,1]` | RGB 为换色目标，A 为换色强度；只在 skin mask 内应用 |
| `u_skin_bright` | `1` | 换色目标亮度；不能作为整张脸的固定曝光倍率 |
| `u_skin_roughness` | `0` | 在 skin mask 内叠加到源 roughness 后饱和 |
| `u_skin_specular` | `0.5` | 在 skin mask 内替换介电高光输入 |
| `u_skin_opacity` | `1` | 仅服务源明确的 coverage 路径；Opaque 槽不使用 |
| `u_subsurface_color` | 按源槽位 | 透射颜色；不能用 LUT 默认值替代源资产语义 |

参数有效值、默认值和宏必须进入逐槽位 manifest。共享 MI 只允许完整输入合同一致的槽位合并。

## 3. MaterialInputs 映射规则

### 3.1 基础输入

`mf_neoxSkinInputs` 必须按源执行顺序完成：

1. 采样 Tex0 RGB 并应用 tint；
2. 采样 ParamMap，写入 roughness、metallic、AO 与 skin mask；
3. 采样 NormalMap，恢复顶层 tangent normal，并独立保存 curvature/detail mask；
4. 按质量配置采样 DetailMap；
5. 仅在源语义确认后执行肤色换色、粗糙度/高光调整和 emissive；
6. 将稳定的 `skinLutId`、厚度、厚度比例、响应权重、曲率和透射权重写入 `PreintegratedSkinMaterialInputs`。

`skin mask` 的职责必须拆开记录：

- 源换色 mask：控制 BaseColor、roughness、specular 的皮肤区域混合；
- SSS response weight：只有源或目标公共合同明确时才能使用；
- 纹身/妆容 mask：不能隐式复用为 SSS 权重。

如果多个职责需要同一通道，必须在 MF 中显式命名和记录转换，不得靠变量复用掩盖语义。

### 3.2 双层法线与曲率

NeoX 高质量路径存在顶层法线和底层模糊法线：

- 顶层法线用于高光和主要视角响应；
- 底层法线用于 PreintegratedSkin 漫反射/散射方向；
- DetailMap 同时影响两层，但底层使用源 mip/LOD 规则；
- 金属区域不得注入皮肤细节法线；
- 曲率来自 NormalMap.B，必要时应用源的 `pow(curvature, curve) * intensity`。

Skin 当前通过 GBuffer F.zw 独立保存 bottom normal 的八面体坐标；它不复用 Clear Coat 字段，也不把普通 tangent 当作 Skin bottom normal。bottom normal 的视觉响应和 mip/LOD 参数仍需固定机位 Debug View 验收。

### 3.3 Emissive 与 coverage

- Opaque 身体和脸部 Skin 的 coverage 固定为 `1`；Tex0.A 不得进入 alpha clip/blend。
- 源 `1 - Tex0.A` emissive mask 需要单独迁移到 Emission MF；在迁移前不得把它当作 AO、opacity 或 skin mask。
- ShadowDepth 使用与 Base 完全相同的 coverage；不得因为缺少 Emission 而改变阴影。

## 4. 光照与能量规则

### 4.1 公共 PreintegratedSkin 规则

1. LUT 负责 `(N·L, thickness)` 的 diffuse response；CPU 不生成运行时 response。
2. `finalDiffuseResponse` 与 `scatteringMultiplier` 的 Lambert 乘法只能由 LUT metadata 决定，不能在 NeoX MF 中重复补偿。
3. 透射颜色、厚度单位和 transmission weight 必须来自资产合同；不得用固定橙色常量代替不同槽位的源参数。
4. Diffuse、transmission、direct specular、indirect diffuse 和 indirect specular 必须保持独立能量预算；调亮 Skin 不得通过重复叠加 DefaultLit diffuse 实现。

### 4.2 NeoX 高光对齐

NeoX 源的目标意图是 PreintegratedSkin 的 dual-lobe specular，并使用 curvature 作为皮肤高光调节输入。对齐顺序固定为：

1. 先确认 roughness/specular/curvature 是否正确进入 GBuffer；
2. 再确认公共 evaluator 的 lobe 参数和能量归一化；
3. 最后才做 MI 的 roughness/specular 校准。

不得用 `directSpecular *= 常数` 作为最终对齐方案。当前 NeoX dual-lobe 参数固定为 `roughness0=0.61601`、`roughness1=1.06777`、`lobeMix=0.85`；平均粗糙度用于 Smith G，两个 lobe 只混合 GGX D。源 curvature 对 lobe roughness 的 UE 分支目前是注释状态，因此在源证据确认前只保存 curvature，不额外乘倍率。

### 4.3 Shadow 与 IBL

- NeoX 皮肤阴影的颜色调制必须作为独立可验证阶段，不得把 shadow color 隐式乘进 LUT。
- Skin IBL 的 diffuse average、AO 和 specular 必须分别验证；不能只看最终 scene color。
- 角色专用环境/方向/局部光倍率只能放在标准输入或场景配置中，不能写入公共 Skin evaluator。

## 5. 资产与 RenderState 规则

- 身体 `b_f_3725_high_0` 当前为 mode 1 Opaque，必须保持 Back Cull、Depth Test 和 Depth Write 的源等价状态。
- 脸部 Skin 的实际 mode、Cull、AlphaRef、宏和槽位绑定必须从 MTG 有效合同读取；不能因为“看起来是皮肤”自动套用身体 MI。
- Skin、Makeup、Glitter、Tattoo、Rebirth 等辅助效果必须按源宏/槽位拆开；缺少目标资源时记录为 unsupported difference，不得静默用默认图或假噪声。
- 真实 UV0/UV1 只由 glTF 和网格导入合同决定；shader 不得复制 UV0 伪造 UV1。

## 6. 验证合同

### 6.1 静态合同测试

至少覆盖：

1. 源 `pbr_skin` 参数、宏、贴图和 RenderState 解析；
2. SkinParam/SkinAux/DetailNormal 通道与颜色空间 manifest；
3. `M_neoxSkin` 只组合 NeoX MF，不把公共 Skin evaluator 私有化；
4. Opaque coverage 在 Base/Forward/ShadowDepth 中一致；
5. 普通 `M_preintegratedSkin` 与 NeoX `M_neoxSkin` 同时可编译、可反射。

### 6.2 运行时 Debug View

皮肤专项至少需要以下可单独观察的视图或 readback：

- BaseColor；
- Roughness / Metallic / AO；
- Skin mask；
- Curvature；
- Top Normal；
- Bottom/Blurred Normal；
- Detail Normal mask；
- Skin LUT ID / thickness / response weight；
- Direct diffuse before/after Skin response；
- Direct specular；
- Transmission；
- Shadow color；
- IBL diffuse / IBL specular。

最终截图不能替代通道和绑定验证。

### 6.3 固定机位验收

固定角色、相机、灯光、环境、曝光和资源版本，按以下顺序比较：

1. 几何、法线和 UV；
2. BaseColor 与 ParamMap 通道；
3. Detail normal 与 curvature；
4. Skin diffuse response；
5. 双 lobe 高光；
6. 透射与阴影；
7. IBL 与 Tone Mapping。

每轮只修改一类输入。不得同时修改纹理、MI、公共 evaluator、场景光照和曝光。

## 7. 完成定义与有意差异

皮肤效果只有同时满足以下条件，才能标记为“效果对齐”：

- 身体与脸部的源槽位合同可反查，且没有静默 fallback；
- 纹理通道、颜色空间、UV、采样器和 mip/LOD 规则可审计；
- NeoX 差异全部由 `M_neoxSkin` / `mf_neox*` / MI 表达；
- 公共 `PreintegratedSkin` 保持普通测试材质兼容；
- Base/Forward/ShadowDepth coverage 一致；
- top/bottom normal、curvature、LUT response、双 lobe 高光、transmission、shadow 和 IBL 均有独立验证证据；
- 所有尚未支持的脸部妆容、glitter、Rebirth、动态动画或质量分支都列入差异清单。

当前明确不等同于完成的内容：

- 仅通过 `mf_preintegratedSkinInputs` 复用 NeoX 参数；
- 仅把 skin mask 乘到 SSS weight/transmission；
- 仅通过固定 specular 缩放获得“更像皮肤”的截图；
- 已有 bottom normal 编解码但没有 Debug View/固定机位证据；
- 未经源资产确认的 `Tex0.A` emissive、脸部 glitter、makeup 和 Rebirth；
- 仅通过最终 Beauty 截图，没有逐通道和 RenderDoc 证据。

### 7.1 角色光照倍率

抓帧源 `u_char_skin_sep = [1.8, 1.37, 1.0, 0.75]` 的通道规则为：

```text
x = 环境光 / IBL
y = 方向光
z = GI
w = 虚拟光
```

VulkanLearn 已将该合同加入 `PreintegratedSkin` 输入，并通过 Skin 专属 `GBufferE`
跨过 deferred pass。V1 只消费 `x/y`；`z/w` 只保留快照，不伪造当前引擎不存在的
GI 分类或虚拟光源。Skin 的曲率仍留在 `GBufferF.x`，底层法线仍留在 `GBufferF.zw`，
以免把 blur 半径语义与角色光照倍率混在一起。

其中 `w` 的语义统一称为 `Virtual Light` multiplier。实现位于
`shader/glsl/engine/virtualLight.glsl`：公共模块只构造摄像机方向和补光强度，
Hair/Skin evaluator 分别消费自己的 HairBxDF 或 Skin response。它不是 `pointLight`
数组中的真实点光，也不执行距离衰减；Skin 在 Deferred 中启用它是 VulkanLearn 为当前
渲染路径增加的兼容扩展，源 shader 的原始 Virtual Light 仍只存在于 Mesh Pass。
