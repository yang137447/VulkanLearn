# 材质 Shader Variant 说明

本文档原先记录的是 `MI_*.json` 直接声明 shader、render states 和旧宏字段的讨论方案。

该路径已经废弃，当前实现以 `documents/rendering/deferred-gbuffer-ue-aligned-plan.html` 为准：

- `M_*.json` 是材质定义资产，负责 `name`、`shadingModel`、`renderStates`、`parameters`、数字 `macros` 和 `textures`。
- `MI_*.json` 是材质实例资产，只引用 shader 源目录下的 `material`，并覆写 `parameters`、`textures`、数字 `macros`，可选覆写 `renderStateOverrides`。
- shader pair 由 `M_*.json` 同级命名约定推导，例如 `shader/glsl/M_pbr.json -> shader/glsl/pbr.vert + shader/glsl/pbr.frag`。
- variant key 使用 `shaderName + renderMode + macros`。
- 旧 MI 格式不再兼容。
