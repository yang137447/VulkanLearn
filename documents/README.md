# Documents

This folder contains repository-visible design and planning documents that are still relevant to ongoing development.

## Responsibility Split

Documentation in this repository is split by responsibility:

- repository root `README.md`
  - project entry point for humans
  - current build, run, and navigation guidance
- repository root `AGENTS.md`
  - working contract for AI coding agents
  - read order, repo assumptions, editing constraints, and risk hints
- `documents/`
  - formal design and planning documents that should remain useful across tasks

`documents/` should not duplicate the full content of `README.md` or `AGENTS.md`.

- If a document is about current setup and how to start, it belongs in `README.md`.
- If a document is about how an AI agent should work in this repo, it belongs in `AGENTS.md`.
- If a document is about architecture direction or future implementation planning, it belongs under `documents/`.

## Structure

- `architecture/`
  - long-term engine and renderer evolution plans
- `rendering/`
  - rendering feature roadmaps and technical implementation plans

Recommended meaning of each category:

- `architecture/`
  - thread model
  - ownership model
  - system boundaries
  - data flow
  - long-term engine structure
- `rendering/`
  - feature roadmaps
  - rendering technique plans
  - pass/resource/shader evolution plans for a rendering domain

## Current Active Docs

- `architecture/future-render-architecture.md`
  - long-term architecture direction for threading, reflection, data flow, and render-side ownership boundaries
- `rendering/pbr-ibl-tod-roadmap.md`
  - planned work for PBR completion, IBL automation, and time-of-day updates
- `rendering/sky-pass-environment-roadmap.md`
  - 独立 Sky Pass、运行时环境 cubemap 持有，以及面向后续大气与体积云的 render graph 改造路线
- `rendering/material-param-authoring-and-reflection.md`
  - 材质参数声明生成、GLSL 编写辅助，以及运行时反射负责真实绑定的精简方案
- `rendering/material-module-system.md`
  - 材质功能模块的边界、public/private 语义，以及基于显式依赖和拓扑排序的顺序规则
- `rendering/descriptor-imageinfo-management.md`
  - 贴图 `vk::DescriptorImageInfo` 的 3 种管理思路，以及当前仓库选择资源自带描述信息方案的原因
- `rendering/material-shader-variant-and-debugview-options.md`
  - 同一 shader 的材质宏变体、Specialization Constant、动态 Debug View 与混合策略的整理，用于后续确认真实需求边界
- `rendering/texture-asset-json-v1.md`
  - 贴图资产 JSON V1 字段、默认值、材质实例引用方式，以及暂不纳入 V1 的 UE 风格导入设置
- `rendering/tone-mapping-tutorial.html`
  - Tone mapping 的游戏管线位置、曝光/曲线/bloom 职责拆分、通用预设和 VulkanLearn 接入建议

## Classification Rule

Documents placed here should meet at least one of these conditions:

- they describe future work that is still intended to guide implementation
- they define architecture direction that is not yet fully realized in code
- they capture conventions that future contributors should still follow

## Maintenance Rule

When a document stops being an active guide, do one of these:

- update it so it reflects the current intended plan
- move its surviving conclusions into a newer document
- delete it if it is only historical implementation detail

Completed or obsolete implementation notes should not stay here as live guidance. Git history is the archive.
