# 运行时资产组织标准

本文定义 `VulkanLearn` 独立运行时资源仓库的目录组织标准。目标是让资产
所有权先于资产类型表达，同时保持场景、模型、材质和纹理之间的相对路径
引用简单、可读、可迁移。

## 根目录

运行时资源仓库根目录只保留三个主要内容域：

```text
VukanLearnResources/
  Common/
    Templates/
  Maps/
    SC_<scene-name>/
  Generated/
```

- `Common/`：跨场景复用或由引擎/RenderGraph直接使用的正式资产。
- `Common/Templates/`：供离线工具或资产作者复用的模板，不是运行时 manifest，
  也不承载某个具体场景的正式实例。
- `Maps/`：具体场景及其专属资产。每个场景使用一个 `SC_<scene-name>/`
  目录，场景文件和只服务该场景的资产放在同一内容域内。
- `Generated/`：导入、运行时、验证和截图等生成物，不属于正式内容资产。

不再把 `models/`、`materials/`、`textures/`、`scenes/` 作为资源仓库的
长期顶层内容域。它们只在 `Common/` 或具体场景目录内作为二级分类使用。

## 内容域布局

正式资产使用下面的目录形状：

```text
Common/
  Meshes/
  Materials/
    Pass/
  Textures/
  Environments/
  Profiles/

Maps/SC_car_showcase/
  SC_car_showcase.json
  Meshes/
  Materials/
  Textures/
  Source/
```

目录名使用 PascalCase；资产文件继续使用现有类型前缀：

- `SC_*.json`：场景定义
- `SM_*.json`：模型/网格描述
- `MI_*.json`：材质实例
- `T_*.json`：纹理描述
- `Source/`：OBJ、glTF、GLB、SpeedTree、DCC 或其他原始输入

`M_*.json`、`M_*.glsl` 和其他 Shader 源文件仍属于引擎仓库的
`shader/glsl/`，不迁移到运行时资产仓库。

## 所有权规则

1. 从一个 `SC_*.json` 递归追踪其模型、材质、纹理和源文件引用。
2. 只被一个场景使用的依赖，放在对应的 `Maps/SC_<scene-name>/` 下。
3. 被多个场景使用的同一份资产，提升到 `Common/`，不复制文件。
4. Pass 材质、默认阴影材质和引擎级预览资源直接放入 `Common/`。
5. `Common/` 不能引用具体场景目录；场景资产可以引用 `Common/`。
6. 资产类型目录表达“它是什么”，内容域表达“谁拥有它”。不能仅凭
   文件名语义把某个目前只被单个场景使用的资产提前归入 `Common/`。
7. 材质参数差异通过新的 `MI_*.json` 表达，不复制模型和纹理来解决。

## 生成物

`Generated/` 按生命周期组织，不能与正式 `Common/` 或场景资产混用：

```text
Generated/
  Runtime/
  Import/
  Validation/
  Screenshots/
```

验证或导入过程需要按场景区分时，在这些目录下面继续使用
`SC_<scene-name>/`。生成物可以引用正式资产，但正式资产不能引用
`Generated/` 中的文件。导入审计清单、转换报告等非运行时记录也放在
`Generated/Import/`，不参与运行时资源解析。

## 迁移策略

资产迁移采用逐场景、逐依赖闭包的方式：

1. 先确定场景目录和资产归属，再移动描述文件及其源文件。
2. 同步更新所有 JSON 中的相对路径，保持路径相对于资源仓库根目录。
3. 对多个场景共享的资产只保留一份，并将引用改为 `Common/` 路径。
4. 每次迁移后验证场景加载、材质绑定、纹理源文件和可选运行时测试。

旧的平铺目录已废弃且不再兼容。新资产不得继续扩展旧的顶层
`models/`、`materials/`、`textures/` 或 `scenes/` 组织方式。
