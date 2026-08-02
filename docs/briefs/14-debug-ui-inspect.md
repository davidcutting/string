# Brief 14 — Debug UI: inspection

Status: **DRAFT — seeded 2026-08-01 from a design conversation while finishing brief 13.** Only the
parts actually decided are written down; the rest of the brief is still to be scoped. Part of the
tooling/UI arc (11–15) before 08 VFX.

Deps: brief 11 (pass/target introspection — `Renderer::passes()`, `Renderer::resources()`,
`gpu_timing().stats()`), brief 12 (panels/workspace), brief 13 (widget kit).

## Scope reminder

Render/pipeline debug and bounded lookdev inspection — NOT a scene editor. Manipulation (light
gizmos, live material edit) is brief 15.

## The two target-inspection tools — DECIDED with the user (2026-08-01)

They cover **disjoint sets of targets**, which is why both exist rather than one replacing the other.

### 1. The lens (magnifying glass)

A draggable circular region over the rendered frame. Inside it, the frame is replaced by a chosen
debug view, at the **same screen pixels**. Inspired by Decima's debug loupe.

**Its whole value is spatial correspondence** — you point at an artifact and ask "what is this pixel's
normal / roughness / overdraw, *here*". That is exactly what a panel thumbnail throws away.

**TWO INDEPENDENT AXES**, which is what makes it more than a viewer:

| axis | range |
| --- | --- |
| WHAT | the scene itself, or a debug target substituted in place |
| HOW MUCH | 1x (exact correspondence) up to Nx (pixel loupe) |

Magnification defaults to **1x**, so correspondence is the default and zoom is opt-in. At Nx on the
SCENE with no substitution it is a plain pixel loupe — the tool you want for AA and single-pixel
artifacts. Both fall out of one shader.

**WHERE IT LIVES: the composite/post pass, NOT the UI pass.** It is a full-screen substitution, and
those passes already sample the scene with bindless `Sampler2D` arrays, already declare their target
reads, and already write the output. The UI's role shrinks to **chrome and controls** — the lens rect
as draggable state, an outline, a combo for the target and sliders for range and magnification, all
of which brief 13's widget kit already provides. No new UI rendering machinery at all.

**Sampling:**

```
src_uv = lens_centre_uv + (pixel - lens_centre) / (screen_size * magnification)
```

At magnification 1 this reduces to identity, so exact correspondence is structural rather than tuned.

**MAGNIFIED SAMPLING MUST SNAP TO TEXEL CENTRES.** A linear sampler at 4x produces a blurry smear,
destroying precisely what is being inspected — AA artifacts and fireflies ARE texel-level detail.
`(floor(uv * size) + 0.5) / size` makes the existing linear sampler behave as nearest; no second
sampler binding is needed.

**CORRECTNESS TRAP — debug targets must BYPASS TONEMAPPING.** Applied in composite, lens pixels
showing raw data (normals, roughness) would otherwise get exposure and tonemap applied and come out
wrong. Same discipline as the UI cancelling exposure to stay display-referred. Easy to get wrong and
hard to spot, because a tonemapped normal buffer still looks like a plausible image.

### 2. The image widget / target browser

A target drawn into a **panel**, with channel mask, mip selection, range remap and false colour.
Carried over from brief 13 M2, which deferred it here.

**THE LENS CANNOT REPLACE THIS.** In-place substitution is only meaningful for SCREEN-SPACE targets.
A **shadow map is in light space**; a cubemap face, an IBL atlas and a probe atlas have no screen
position to substitute at. Those can only be inspected in a panel. Depth prepass output happens to be
screen-space so either tool serves it, but shadow maps are image-widget-only.

It is also the "which pass produced garbage" tool — many targets at once, for orienting when you do
not yet know where the problem is. The lens is the "what is this pixel" tool once you do.

**What it needs** (from a read of the code, 2026-08-01): the sampling path already exists — the text
shader does `textures[pc.atlas_slot].Sample(...)` against bindless `Sampler2D textures[]`, with slots
from `descriptor_table_.get_binding_slot(image, TEXTURE)`. The work is:

- `element.image` as a 1-based index into a builder-side table, mirroring how `text` is held out of
  line so `element` stays small and trivially copyable.
- A third stream in `ui_pass`: pack + pipeline + a draw inside each layer batch. Brief 12 M1b's
  per-layer batching already accommodates a third draw.
- A shader: channel swizzle, `SampleLevel` for mips, range remap, false-colour ramp.
- The widget API.

**THE RISKY HALF IS NOT THE WIDGET — it is sampling a render target from the UI pass**, which needs a
dynamically declared `SampledRead` on whatever target is currently selected. That is expressible
(brief 16 made `PassSpec.usages()` a live getter read each frame), but wrong layout / missing barrier
/ target-absent-this-frame are all real failure modes.

**DE-RISKING: the glyph atlas is already a bindless sampled texture with a known slot.** The whole
widget, shader and pipeline can be built and verified against it with zero barrier risk, before any
render target is involved. Channel masking, mip selection and range remap are all visible on an SDF
atlas.

## Still to scope

Per-pass toggle + timing rows (the read seam exists: brief 11 M4 introspection), the render-graph DAG
view (brief 13's graph widget was built for exactly this), material preview sphere, and whatever else
the arc needs. Not yet designed — do not treat this section as decided.
