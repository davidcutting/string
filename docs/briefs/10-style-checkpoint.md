# Brief 10 — Asset style validation checkpoint

Status: not started

(Moved from slot 06 in the 2026-07-22 reorder; still last. Its tooling section
— Tracy, debug draw, stats HUD — moved out to brief 06 (debug tooling), so
those tools EXIST by now and this brief just uses them.)

## Goal

A checkpoint, not a feature: prove the renderer produces the **WildStar × PoE**
look on real-ish content. Real game art barely exists yet (user confirmed), so
this validates the **pipeline's range** on stylized stand-ins — swap in real
art later.

## Decisions (locked with user; revised 2026-07-21)

- **Primary content: standard high-detail test scenes** (user decision — game
  content is largely procedural, so hand-curated stylized kits are wasted
  effort). Sponza (already in-repo, incl. curtains/ivy) stays the baseline; add
  **Amazon Lumberyard Bistro** (exterior+interior — foliage alpha-test,
  transparency, night-lights stress) and optionally a warehouse-style interior
  scene. These are the benchmarking, correctness, and picky-visual-quality
  targets: the user can judge fine detail hard on them precisely because they
  are dense and well-known references. Cook through the existing KTX2/BC7 path.
- **The renderer is content-agnostic by design** — techniques validated on
  these scenes transfer to game assets; game-specific looks are custom shader
  tweaks later, not pipeline changes.
- **Keep one small stylized probe** for the decisions realistic scenes cannot
  answer (outline/rim choice, bloom character, sky palette against
  hand-painted-style albedo): a handful of CC0 stylized props + ONE stylized
  character still in bind pose. Minimal — a probe, not a kit; agent proposes,
  user approves.
- **The work is iteration, not code**: lookdev on Sponza + Bistro (day/night,
  time-of-day sweep, combat-effects vignette combining briefs 08+09 output) —
  iterate material response, lighting, sky colors, bloom/SSAO/exposure CVars
  against user screenshot feedback; the user judges these scenes *pickily*
  (they are the detail benchmark). Small shader tweaks are in scope; new
  systems are not. These scenes double as the standing performance benchmark
  set (capture reference timings with Tracy here).
- **Outline/rim decision happens here** (the brief-09 slot): try rim-light and
  edge-outline variants on the stylized probe (realistic scenes can't answer
  this); user picks one (or none). The tonemapper choice deferred from brief 07
  is also revisited here if ACES isn't serving the look.

## Milestones

1. Stand-in asset set approved + imported + cooked; lookdev scenes assembled
   (Bistro exercises brief 04's cutout/transparency at scale — verify).
2. Iteration rounds with user (screenshots → tweak → repeat).
3. Outline/rim decision recorded back into brief 09 / the spec.

## Acceptance

- User declares the three lookdev scenes "look like my game's direction" —
  explicitly subjective, user-judged via screenshots (agent reports what to
  compare, never claims the look is right).
- Reference Tracy/per-pass timings captured on the lookdev scenes and recorded
  as the standing benchmark numbers.
- All tuning that produced the approved look is committed as defaults (CVar
  defaults / sky members), not left in local state.
