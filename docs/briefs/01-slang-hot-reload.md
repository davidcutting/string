# Brief 01 — Slang, reflection-driven layouts, hot reload

Status: milestones 1–4 implemented (composite/ui/text/sky ported to Slang with reflection-driven
push-constant layouts, generic mtime file-watch service on job_system, frame-boundary hot swap via
the garbage_collector ring, and a compile-error overlay on the UI pass). Remaining on GLSL:
3d_shader, shadow, cull.comp (device-address buffer references / compute — deferred, see report).
Pending user visual verification of the edit→save→see loop and the error overlay.

## Goal

Replace the offline GLSL/glslang shader path with in-process **Slang** compilation,
**reflection-driven pipeline layouts**, and **auto-on-save hot reload** with a
keep-last-good + on-screen-error-overlay failure mode. This is the iteration loop
for all of Phase A; correctness and workflow feel outrank cleverness.

## Current state

- Shaders are GLSL under `sandbox/shaders/` (3d, cull.comp, shadow, sky, ui, text,
  grid) and `string-engine/shaders/` (composite), compiled at build time via
  glslang in meson.
- `gpu/shader.hpp::compile_shader()` contains a Slang proof-of-concept seed.
- Pipeline layouts are hand-built via `pipeline_builder`; binding model is push
  constants + one global bindless `descriptor_table`.
- `job_system` exists (`core/job_system`); the locked watcher mechanism is
  **mtime polling on job_system** (no inotify/Win32 split; SDL3 has no watcher).

## Decisions (locked with user)

- **All shaders port to Slang.** Incremental conversion is fine (pass by pass),
  but Phase A ends with no GLSL.
- **Reflection-driven rework**: adopt Slang reflection (ParameterBlocks) to
  generate pipeline layouts instead of hand-building them. This deliberately
  touches `pipeline_builder` and the descriptor layer. Bindless global table
  remains the resource-access model; reflection replaces the *layout plumbing*,
  not the bindless philosophy.
- **Trigger: auto on save** — mtime poll; recompile + swap at the next frame
  boundary (safe pipeline recreation; no mid-frame swaps).
- **Failure mode: keep last good + overlay.** A failed compile never breaks the
  frame: previous pipeline keeps rendering; compile diagnostics render in an
  on-screen overlay (use the existing UI pass) until the next success clears it.
- **Watcher is generic from day one**: a file-watch service (path → callback on
  mtime change) that shaders merely subscribe to. Later consumers: VFX data
  files (brief 04), tuning/CVars, UI themes.

## Milestones

1. **In-process Slang compile**: replace build-time glslang for one pass
   (composite is the simplest) — Slang source → SPIR-V at startup, disk cache
   keyed by content hash so cold start doesn't recompile everything.
2. **Reflection layouts**: derive descriptor set/pipeline layouts + push-constant
   ranges from Slang reflection; `pipeline_builder` consumes them. Port the
   remaining passes to Slang as they're converted.
3. **Watch service + hot swap**: generic mtime watcher on job_system; on change,
   async recompile → validate → queue pipeline swap at frame boundary; deletion
   of old pipeline goes through the existing frame garbage_collector ring.
4. **Error overlay**: compile errors (file, line, message) rendered via the UI
   pass; clears on successful reload.

## Acceptance

- Edit any shader, save → change visible within a second, no hitch beyond the
  swap frame, no validation errors.
- Introduce a syntax error, save → rendering continues untouched, overlay shows
  the diagnostic; fix → overlay clears, change applies.
- `nix flake check` green; cold-start time not regressed (cache working).
- NEEDS VISUAL VERIFY: user confirms the edit→see loop and the error overlay.
