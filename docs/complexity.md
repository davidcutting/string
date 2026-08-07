# Cognitive complexity

```sh
tools/complexity.sh                     # whole engine, ranked worst-first
tools/complexity.sh string-render-forward   # one library
tools/complexity.sh --raw               # per-construct breakdown: WHERE the score accumulates
tools/complexity.sh --baseline          # record docs/complexity-baseline.txt
tools/complexity.sh --gate              # fail if anything got worse
```

## The gate

`--gate` is a **one-way ratchet**: no tracked function may score above its baseline, and no new
function may appear over the threshold. Complexity can fall freely; it cannot rise.

A fixed ceiling would have been useless here — the worst function is 230, so any ceiling that passed
today would bless everything beneath it. The ratchet leaves existing debt visible without blocking
work, and makes each new increment a deliberate act rather than an accident.

Keyed on `file::function`, not line numbers, so ordinary edits don't churn the baseline. Where a file
has several same-named functions (`widgets.cpp` has four `emit`), the highest score wins.

When a change genuinely justifies more complexity, accept it explicitly:

```sh
tools/complexity.sh --baseline    # re-record, then commit the baseline with the change
```

That is the point: raising the ceiling becomes a visible line in the diff instead of a silent drift.

**Verified to fail, not just to pass** (2026-08-06). Both paths were exercised with deliberate
sentinels and then reverted:

- a new convoluted function → `NEW  graph_plan.cpp::complexity_sentinel 67`
- nested noise inside an existing one → `REGRESSION  graph_plan.cpp::build 50 -> 94`

A gate that has only ever been seen to pass is not evidence of anything.

### Why this gate exists

The byte-parity gates (`tools/gate.sh`, AE=0) reward *adding* code: any restructuring that changes
output counts as failure, so the safest move is always to add another branch where the code already
is. Across many sessions that is pure accretion with a good changelog. This gate is the
counterweight — it makes the cost of that habit visible in the same way AE=0 makes rendering
regressions visible.

Config: `tools/complexity.clang-tidy`. Uses `build-uml/compile_commands.json` (same native-build and
`nix develop` requirements as `tools/uml.sh`).

## What is being measured

`readability-function-cognitive-complexity` — the Campbell/SonarSource metric, which scores how hard
code is to **follow**, not how many paths it has:

- nesting **multiplies** (an `if` at depth 3 costs more than one at depth 0)
- a flat sequence of `if`s barely costs anything
- a `&&`-chain counts once, not once per operand

That makes it a far better proxy for mental effort than cyclomatic complexity, which would rate a
200-line flat switch as terrible and a deeply nested 20-line function as fine.

Threshold is **25**, the check's default and the usual industry line. Left unmodified so the numbers
are comparable to other codebases rather than tuned to flatter this one.

`readability-function-size` runs alongside as a cross-check — a function can be long but simple (a
big flat initialiser) or short but dense, and reading both tells you which you have.

## Findings (2026-08-06)

60 functions over threshold. The worst, excluding vendored `third_party/`:

| Score | Function | Location |
|---|---|---|
| 230 | `author_dag` | `string-debug/src/panels.cpp:675` |
| 183 | `record_frame` | `string-core/src/vulkan/renderer.cpp:822` |
| 169 | `update` | `string-render-forward/src/geometry_pass.cpp:1076` |
| 107 | `rows` | `string-ui/src/widgets.cpp:927` |
| 89 | `nodes` | `string-ui/src/widgets.cpp:1311` |
| 79 | `nodes` | `string-ui/src/widgets.cpp:1140` |
| 69 | `flatten_geometry` | `string-asset-tools/src/gltf_loader.cpp:249` |
| 63 | `build_meshlet_gpu` | `string-render-forward/src/geometry_pass_meshlet.cpp:32` |
| 61 | `GeometryPass` (ctor) | `string-render-forward/src/geometry_pass.cpp:383` |

`GeometryPass::update` additionally trips every size threshold: **586 lines, 360 statements, nesting
level 5**. That is the quantitative version of the "why is the geometry pass a monolith" answer — the
function is a frame tick (input, time-of-day, streaming feedback, SceneData fill, stats readback,
overlay publishing) that happens to live inside a render pass.

Worth noting the top two are NOT the renderer's geometry path:

- `author_dag` (230) is debug-panel UI authoring — the highest score in the engine.
- `record_frame` (183) is the frame-graph executor, which grew through this session's barrier work.

So complexity is not concentrated where the architectural discussion has been focused. `string-ui`
has four functions over 79, which is a bigger cluster than anything in the renderer.

## Caveats

- **A high score is a question, not a verdict.** `build_meshlet_gpu` and `compute_cascades` are dense
  because the algorithms are dense; splitting them could easily make them harder to read.
- **`IgnoreMacros: true`** is on. Without it the `STRING_PROFILE_*` zones and Vulkan struct
  initialisers inflate scores with complexity nobody wrote or reads.
- **`third_party/` is excluded from the ranking**, not from the analysis — mikktspace alone
  contributes a dozen entries and will never be refactored. `--raw` still shows it.
- Header-defined functions are reported once per including TU; the ranking deduplicates by
  (function, location), so the "findings before dedup" count is larger than the table.
