# Brief 19 — Migrate the build to pixi

**Status:** NOT STARTED. Written 2026-08-04 at the end of the asset-import session, so it starts from
a plan rather than from memory.

## Why

`flake.nix` is the build. That is fine for one developer on NixOS and wrong for the goal this repo
now has: handing the engine to an artist, and building on Windows and macOS. "Install nix" is a far
bigger ask than "install a package manager and VS Build Tools".

Pixi (conda-forge) gives pinned cross-platform toolchains + libraries with a lockfile, and
`pixi task`, which replaces the pile of ad-hoc scripts (`tools/gate.sh`, `capture.sh`, `ui_cost.sh`,
and the session scratch scripts) with something discoverable on every platform.

## Decisions already made with the user

- **Slang and libktx are both on conda-forge** (user, 2026-08-04). That was the open question — with
  those two covered, pixi can supply the whole dependency set rather than layering on top of the
  meson wraps.
- **clang on all three platforms** (Windows, Linux, macOS). One frontend, one set of diagnostics.
- **VS Build Tools is assumed present on Windows** — developers and artists both. So clang uses the
  MSVC ABI/STL there, and pixi is not expected to provide a compiler for that platform's runtime.
- **The meson wraps STAY** as the no-package-manager fallback. They work as of 2026-08-04 (see
  `docs/off-nix-build.md`) and they are what lets a bare clone build with nothing installed.

## Order of work — each step leaves the tree building

### 1. Spike: `pixi.toml` beside the flake, building `string-core` + its tests

The one genuinely unknown part is not "does the package exist" but "does meson find it the way we
ask for it":

- Slang is resolved by `dependency('slang')` (string-core/meson.build) — needs a pkg-config or CMake
  config the conda package actually ships.
- libktx is resolved by `dependency('Ktx', method: 'cmake', modules: ['KTX::ktx'])` — needs that
  exact CMake config name.

If either does not resolve, the fallback is already in place (both sites probe the system first and
fall back to a wrap), so a partial answer is still a working build.

**Retest while here:** the `cmake.subproject` workaround for libktx exists because KTX's bundled fmt
fails `consteval` under GCC 15. Under clang that may not happen, in which case the simpler
`[provide] ktx = ktx_dep` form works and the workaround should be deleted rather than carried.

### 2. Port the gates to `pixi task`

The bulk of the work, and the part that matters — every correctness claim in this repo runs through
these. `gate.sh` (frame-300 exterior AE=0), `capture.sh`, and the three check suites.

Gate the gates: a ported task must reproduce **AE=0 against the existing baselines** before it is
trusted. A task that runs and prints a number is not evidence until it has been seen to fail for the
right reason (see the verification rules in README.md).

### 3. Only then decide nix's fate

Keep `flake.nix` until pixi reproduces the same results on the same baselines. Retiring it earlier
means that when a capture differs, there is no way to tell whether pixi is at fault or the change is
a real regression — which is precisely the confusion the baselines exist to prevent.

## What this does not fix

- **Windows still needs VS Build Tools.** Accepted, per the decision above.
- **A Windows build is still unproven.** The mingw cross-compile (`tools/mingw-w64.cross`) builds
  `string-core` cleanly, but that is GCC-family; clang-with-MSVC-ABI is a different frontend. The
  real gate remains a Windows build from a clean clone.
