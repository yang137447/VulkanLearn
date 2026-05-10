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
