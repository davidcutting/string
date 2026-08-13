# Building without nix

**Status: NOT YET WORKING.** Investigated 2026-08-04. One genuine bug found and fixed; three gaps
remain, listed below with what each actually needs. Written because brief 18 (asset import) exists to
hand the engine to someone else, and "it builds via the flake on my machine" does not do that.

## What is actually true about the dependency graph

The nix build supplies these from nixpkgs: `spdlog glm entt sdl3 vulkan-headers
vulkan-memory-allocator vulkan-volk shader-slang fastgltf simdjson ktx-tools meshoptimizer`.

Off-nix, **all but libktx and simdjson have a wrap**, and a `meson setup` of `sandbox/` with no
nixpkgs libraries on the path resolves fastgltf, slang, spdlog, glm, entt, volk, vulkan-headers, VMA
and SDL3 from those wraps. `string-core/subprojects/packagecache/` holds the tarballs, so no network
fetch is needed. The graph is in better shape than it looks.

**simdjson is a non-issue, and it is easy to get backwards.** The *wrap* path is the self-contained
one: `fastgltf.wrap` fetches simdjson's pinned single-header at configure time, so
`dependency('simdjson', required: false)` in `string-core/meson.build` finds nothing and skips. It is
**nix** that needs a system simdjson, because nixpkgs' fastgltf does not bundle it.

## Where wraps must live (got this wrong once — read before adding one)

**Only `string-core/subprojects/*.wrap` is tracked in git.** `sandbox/subprojects/.gitignore` ignores
everything except the sibling-library symlinks, so `sandbox/subprojects/meshoptimizer.wrap` and
`.../vulkan-memory-allocator.wrap` are **local build artifacts** — meson's auto-generated redirect
wraps — not repo content. A clean clone has neither.

That invalidated the first thing this investigation "fixed": the stale
`vulkan-memory-allocator.wrap` redirect pointing at `string-engine/subprojects/...` (a path deleted
at the 7-library split) was **stale local state on this machine, not a repo bug**. Deleting it and
re-running configure confirmed meson regenerates it correctly. A fresh clone was never affected.

So a new wrap goes in a **tracked** `subprojects/` dir of the library that consumes it — meson
promotes it to the root automatically, which is the mechanism those redirect files are evidence of.

## Fixed

- **libktx now has a wrap** — `string-asset-tools/subprojects/ktx.wrap`, KTX-Software v4.4.2 source
  tarball, `method = cmake`. Verified: it downloads, configures and **builds** (76/76, links
  `libktx.so`).

  Three things worth knowing before touching it:
  - The plain GitHub source archive is enough. As of v4.4.2 the only submodule is `tests/cts` (test
    data) — basisu/astc-encoder/dfdutils are vendored in-tree — so no `clone-recursive` is needed.
  - **`[provide]` works fine for a CMake wrap** — the form is `<depname> = <target>_dep`, exactly as
    `fastgltf.wrap` documents, because meson's CMake integration exposes each target as
    `<target>_dep`. (`dependency_names = ktx` is the wrong form and fails with *"did not override
    'ktx' dependency and no variable name specified"* — that failure says nothing about CMake wraps.)
    It is still **not** used here, for a different and verified reason: with KTX's default CMake
    options the subproject puts all 61 targets in `all`, including the `ktx`/`toktx` CLI tools, whose
    bundled fmt does not compile with GCC 15 (*"call to consteval function … is not a constant
    expression"*). CMake defines can only be set via `cmake.subproject()`, so that is the route —
    tools/tests/docs off, 61 targets down to 35, and it builds.
  - **libktx is resolved in exactly one place** (`string-asset-tools`), which re-exports it via
    `project_deps`. `string-render-forward` gets it transitively; two `cmake.subproject('ktx')` calls
    in one build would collide.

  The tarball is ~210 MB and lands in `subprojects/packagecache/`, which is untracked and
  regenerable. Both meson.build sites still probe for the system CMake config first, so nix keeps
  using nixpkgs' ktx-tools — verified `checks.cook` and `checks.default` still green.

- **Slang is now per-platform.** `slang.wrap` became `slang-linux-x86_64.wrap` +
  `slang-windows-x86_64.wrap`, both sharing the existing `packagefiles/slang/meson.build` (it only
  needs `include/` and `lib/`, which both archives have at their root).

  A wrap-file pins exactly one archive and Slang ships prebuilts rather than source, so the platforms
  cannot share a wrap. They are selected in `string-core/meson.build` by an explicit
  `subproject(...)` call keyed on `host_machine.system()`, and neither carries a `[provide]` section
  — two wraps providing `slang` would be a duplicate-provider conflict. An unknown host system gets
  a clear error naming the fix rather than a confusing fallback failure.

  **Runtime difference that matters when packaging:** Slang `dlopen()`s its sibling libraries at
  runtime. On Linux those live in `lib/` (same dir as the link target); **on Windows they are the
  DLLs in `bin/`**. A Windows build has to ship `bin/`.

  Verified from Linux: the linux wrap resolves through the new selection path, and the Windows wrap's
  hash checks out, extracts with the expected `bin/ cmake/ include/ lib/ share/` layout, and takes
  the shared patchfile. **Not** verified: `cpp.find_library('slang')` against `lib/slang.lib`, which
  needs an MSVC toolchain.

## Cross-compiling to Windows (mingw-w64) — `string-core` BUILDS

`tools/mingw-w64.cross` cross-builds the engine library for Windows:

```
nix-shell -p pkgsCross.mingwW64.buildPackages.gcc meson ninja cmake pkg-config \
  --run "meson setup build-win string-core --cross-file tools/mingw-w64.cross -Dwsi=sdl && ninja -C build-win"
```

Result: **`libstring-core.a` links, no errors.** Every subproject resolved for a Windows host —
including `slang-windows-x86_64`, which means the new wrap works *and* `cpp.find_library('slang')`
locates `lib/slang.lib`.

**The `[cmake]` section is load-bearing.** Without it, meson's CMake integration invokes the *native*
compiler with Windows link flags and fastgltf dies at CMake's "compile a simple test program" probe
(`ld.bfd: unrecognized option '--major-image-version'`). The cross file has to tell CMake about the
toolchain separately from meson.

**Found + fixed by doing this:** `platform_detection.hpp` defined `STRING_PLATFORM_WINDOWS`
unguarded while meson defines it on the command line too — a redefinition warning in every
translation unit, invisible on Linux because the two never agree there.

### What this does and does not prove

- **Does:** our code compiles for Windows; headers, platform guards and the wrap graph are sound.
- **Does not:** that it builds with **MSVC**. mingw is GCC-family; MSVC differs on conformance,
  `consteval`, and warning surfaces — recall KTX's bundled fmt already fails on GCC 15.
### Full demo cross-build: CONFIGURES, does not link — and the failures are mingw artifacts

Every dependency resolves for a Windows host (libktx and meshoptimizer both cross-configure through
CMake). Compilation then fails in **third-party** code, three times, each specific to mingw-on-Linux
rather than to Windows:

1. KTX's `lib/astc_codec.cpp` includes `Windows.h`; mingw-w64 ships `windows.h`, and Linux
   filesystems are case-sensitive. Real Windows resolves either spelling. Workaround: an include dir
   holding a `Windows.h` symlink, injected via `[built-in options] cpp_args` in the cross file (a
   plain `CXXFLAGS=` does nothing — meson bakes flags at configure time).
2. SDL3 links `-lpthread` for the Windows target; nixpkgs' mingw uses **mcfgthread**, so there is no
   `libpthread`. A native Windows SDL build does not take this path.

**Attempting a Wine run was abandoned here, deliberately.** Every further step needed another
workaround in someone else's build, and each one makes the result less representative of a real
Windows build rather than more. A Wine pass built on that stack would not be the evidence it looks
like — and a Wine failure would be unattributable.

The useful signal from cross-compiling is `string-core`, which builds clean. Beyond that, the answer
is a real Windows toolchain, not a better shim.

## A second missing wrap, found the same way

`meshoptimizer` had **no tracked wrap either** — like the VMA redirect, it existed only as untracked
cruft in `sandbox/subprojects/`, so no clean clone ever had it and nix hid the gap by supplying one
from nixpkgs. Now tracked at `string-asset-tools/subprojects/meshoptimizer.wrap`, beside `ktx.wrap`.

Its cruft copy also carried a **stale hash**: GitHub's auto-generated archive tarballs are not
byte-stable across regenerations, so the pinned hash no longer matched what the URL serves. Anything
pinning a GitHub `/archive/refs/tags/` tarball is exposed to this.

## ozz-animation (brief 23, added 2026-08-13)

`string-core/subprojects/ozz-animation.wrap` — ozz 0.17.0 source tarball, `method = cmake`,
resolved in exactly ONE place (`string-core/meson.build`, the libktx single-resolution pattern)
with the offline half exported as the `ozz-animation-offline` dependency for string-asset-tools.
The wrap's version **must track the flake.nix derivation** (the meshoptimizer lesson); the
compile-time tripwire is the archive-type-version static_asserts in
`string-core/test/ozz_version_test.cpp` (ozz has no version macro).

Cross-build facts to know before touching it:

- **ozz 0.17.0 requires CMake >= 3.30** (0.16.0 only needed 3.24). If the cross toolchain's
  cmake is older, the pin has to drop to 0.16.0 — in both the wrap AND flake.nix, together.
- The `[cmake]` section of `tools/mingw-w64.cross` is load-bearing for this wrap exactly as it
  is for ktx/fastgltf: built-in options do not reach CMake subprojects.
- `CMAKE_COMPILE_WARNING_AS_ERROR=OFF` is passed by `string-core/meson.build` because 0.17.0
  hard-enables it — without the override, any new GCC warning kills the dependency build
  (the same class of failure as KTX's bundled fmt vs GCC 15).
- `ozz_build_postfix=OFF` is mandatory: the default appends per-config suffixes
  (`libozz_animation_r.a`) that no probe resolves.
- ozz is plain C++17 with SSE2 SIMD; if a cross target ever chokes on the intrinsics,
  `-Dozz_build_simd_ref=ON` exists but is an ABI trap — it switches `SimdFloat4` between
  `__m128` and a struct via a compile definition that does NOT propagate to consumers, so
  our own TUs would need `-DOZZ_BUILD_SIMD_REF` too or it is a silent ODR violation. Leave
  it OFF on x86_64.

## Remaining gaps

1. **SDL3 built from the wrap needs Linux platform dev packages** — configure walks udev, then dbus,
   then pipewire, and so on. These are normal SDL build prerequisites (`libudev-dev`,
   `libdbus-1-dev`, `libpipewire-0.3-dev`, …), not a defect in this repo, and they do not apply on
   Windows. Supplying them exposes a further wrap-graph conflict: SDL3 resolves `wayland-client`
   from the system, then the `wayland` wrap tries to override the same name and meson errors with
   *"Tried to override dependency 'wayland-client' which has already been resolved"*. The fix is
   probably to prefer a system SDL3 when present (as the VMA block does for its header) rather than
   to build SDL from source on Linux at all.

   **This is a Linux-only rabbit hole and was deliberately abandoned**, because it does not move the
   actual goal — Windows delivery, where none of these apply.

## The full Windows demo cross-compiles and RUNS (2026-08-04)

`tools/mingw-w64.cross` (gcc) now builds the **entire sandbox**, not just `string-core`:
`string_demo.exe` and `string_cook.exe`, both verified `pei-x86-64` PE binaries.

    PT=$(nix-build '<nixpkgs>' -A pkgsCross.mingwW64.windows.pthreads --no-out-link)
    nix-shell -p pkgsCross.mingwW64.buildPackages.gcc meson ninja cmake pkg-config glslang \
      --run "meson setup build-win-demo sandbox --cross-file tools/mingw-w64.cross \
             -Dc_link_args=-L$PT/lib -Dcpp_link_args=-L$PT/lib && ninja -C build-win-demo"

`glslang` is a BUILD-machine tool (shader compile step) and `sandbox` has no `wsi` option — both
easy to get wrong. The winpthreads `-L` is passed on the command line rather than baked into the
cross file so no `/nix/store` hash is hardcoded in tracked content.

**Four blockers cleared to get there**, in order:

1. **KTX `Windows.h`** — solved by `tools/wincompat/Windows.h`, a tracked forwarding header (not a
   symlink), injected via `[built-in options] cpp_args` AND restated in `[cmake]`, since built-in
   options do not reach CMake subprojects.
2. **SDL3 `-lpthread`** — `pkgsCross.mingwW64.windows.pthreads` really does provide `libpthread.a`;
   it just is not on the default link path. No stub archive needed.
3. **KTX `exports.def`** — its shared-library target wants a generated `.def` that its CMake does
   not produce under cross. Fixed by `KTX_FEATURE_STATIC_LIBRARY=ON` + `BUILD_SHARED_LIBS=OFF` in
   `string-asset-tools/meson.build`. Off-nix only — on nix, `dependency('Ktx')` resolves first.
4. **Two real portability bugs in OUR code**, the only genuine defects found:
   - `setenv` is POSIX; Windows has only `_putenv_s`. Added `string::core::set_env_default()`
     (`cvar.hpp`), which keeps the overwrite=0 semantics the caller relied on.
   - `std::println` in `logger.hpp` needs `std::__open_terminal` / `std::__write_to_terminal`,
     which mingw's libstdc++ does not provide — so **every binary** failed to link. The message was
     already formatted, so it is now a plain `std::fputs`.

**It runs.** Under Wine, `string_cook.exe` cooked a real glTF end-to-end — fastgltf parse,
MikkTSpace tangents, meshoptimizer meshlet build, file output. This is attributable evidence in a
way a Vulkan windowing test would not be: the cook tool touches no GPU, so nothing is being blamed
on Wine's graphics emulation.

**Shipping set** — only three DLLs beside the exe (`KERNEL32`/`msvcrt` are OS-provided):
`libSDL3.dll` (built), `slang.dll` + its siblings from `slang-2026.12.0.1-windows-x86_64/bin/`
(Slang dlopen()s them at runtime), and `libmcfgthread-2.dll` from the mcfgthread package.
`libstdc++-6.dll` is NOT needed — it links statically here. The tree is ~382MB unstripped, mostly
`-g` debug info plus `slang-llvm.dll`; strip before handing it to anyone.

### Packaging: use `tools/package-windows.sh`, do NOT hand-assemble

    tools/package-windows.sh <build-dir> <out-dir> [prebuilt-cache-dir]

Hand-assembling the folder shipped two broken packages in one session — a missing `assets/fonts/`
(null deref in stb_truetype) and missing `shaders/*.spv` (**the user hit this on real Windows: the
lookdev scene closed the app**, `Failed to open file: ...shaders/debug_line.vert.spv`). Both were
"I forgot a file". The script derives what to copy from the build tree and the exe's OWN IMPORT
TABLE, then verifies before claiming success.

**Shaders come from TWO places and both must ship** — this is the trap:
- `*.slang` — source, compiled at runtime by Slang, and the cache key is their content hash
- `*.spv` — prebuilt by glslangValidator at BUILD time (`debug_line`, `grid_2d_shader`), used by the
  debug-line and grid passes. Not in the source shader dirs; only in the build tree.

Two verifier bugs found by falsifying it (hide a file, confirm it fails) rather than trusting a
green run — worth repeating if you extend it:
- it grepped `sandbox/` including `subprojects/`, so vendored SDL3/KTX sample code demanded
  `skybox.vert.spv` and friends we never build (`--exclude-dir=subprojects`);
- the `.spv` pattern was anchored to an opening quote, but call sites write
  `"shaders/debug_line.vert.spv"` — so it matched nothing and the loop **silently never ran**. A
  check that cannot fail is worse than no check.

For a `-Dslang=disabled` build the script REQUIRES a cache dir and refuses without one (that package
cannot render anything otherwise), and prints cached-programs vs `.slang`-source counts, since
warming by running one scene only covers that scene.

### Is it hermetic? Verified 2026-08-04

**The binary is clean**: zero `/nix/store` references, zero `/home/dcutting` references (before OR
after stripping — no username leak). Strip takes `string_demo.exe` from 172MB to **6.9MB**,
`string_cook.exe` to 4.8MB. `libstdc++` links statically.

**But the exe alone is not the product** — it needs a folder beside it:

    string_demo.exe
    libSDL3.dll  libmcfgthread-2.dll
    slang.dll  slang-compiler.dll
    shaders/        <- .slang SOURCE; shaders are compiled AT RUNTIME, which is why slang ships
    assets/fonts/   <- DejaVuSans.ttf; without it the UI font atlas faults (see below)

**46MB total — one exe, four DLLs.** `string_cook.exe` is NOT shipped: the engine cooks in-process
(`registry.set_cook` -> `cook_scene_textures_for`, reachable from the Scene menu and the
`content.cook` cvar), so the CLI tool is a developer convenience, not an artist dependency.

**51MB with the cook tool — only FOUR DLLs.** Slang ships 7; five of them (`slang-llvm.dll` 105MB,
`slang-glslang.dll` 11MB, `gfx.dll` 2.6MB, `slang-glsl-module.dll`, `slang-rt.dll` — 122MB combined)
are never loaded. Slang emits SPIR-V directly, so the LLVM and glslang downstream backends are dead
weight, and `gfx.dll` is Slang's own graphics layer, which we do not use. `slang.dll` is a 157KB
forwarder whose exports resolve into `slang-compiler.dll`, so those two are inseparable — dropping
the big one fails with wine's `unimplemented function slang.dll.slang_createGlobalSession2`.

Verified by capture, not by inspection: cold shader cache, frame-60 capture byte-identical (AE=0) to
the full-DLL baseline. Two methodology notes that nearly produced false results — **use
`STRING_FIXED_DT`** (real-dt frame 60 is nondeterministic run-to-run, so an unfixed comparison shows
a huge spurious AE), and **clear `%LOCALAPPDATA%\string\cache\shaders` first**, or a warm SPIR-V
cache means Slang is never invoked and the test proves nothing. A same-config repeat run was
confirmed AE=0 before trusting any of it.

Caveat: this exercises the shader set the default scene compiles. A feature that drives Slang down a
GLSL path could still want `slang-glslang.dll`.

**Static linking is mostly NOT the lever here** (asked 2026-08-04). Shipping fewer files beat it
outright: 173MB -> 51MB by dropping unused DLLs, with zero build changes. Of what remains —
- **Slang cannot be static**: its distribution is prebuilt DLLs with no static libs, and it
  `dlopen`s siblings at runtime. Static would mean building Slang from source.
- **SDL3 static is possible in principle** (Zlib licence, no obstacle) but `--default-library=static`
  fails here: SDL's `-lpthread` then propagates to the final exe link and lands BEFORE our `-L`, and
  `LIBRARY_PATH` does not help because the nix cc-wrapper sanitises it. Saves one 16MB DLL; not
  pursued, it is someone else's build system for little gain.
- **libmcfgthread should NOT be statically linked**: nixpkgs records it **GPL-3.0-or-later** with no
  linking exception noted. Worth a real legal check before shipping either way — but note it exists
  only because nixpkgs' mingw gcc chose that thread model, and it **disappears entirely on the real
  target** (clang + MSVC ABI), where the runtime is Microsoft's.

**Two external requirements that are NOT in the folder** and cannot be:
- **A Vulkan driver.** volk loads `vulkan-1.dll` at runtime, so it is not in the import table; it
  comes from the GPU driver. Any machine with current GPU drivers has it.
- **`msvcrt.dll` / `KERNEL32.dll`** — shipped by Windows itself.

**Ship a RELEASE build.** A debug build sets `STRING_DEBUG`, which makes `driver.cpp` *require*
`VK_LAYER_KHRONOS_validation` and throw when it is absent — that layer only exists where the Vulkan
SDK is installed, so a debug build dies instantly on an artist's machine.

**Verified running on the Windows code path** (Wine, launched from an unrelated working directory):
resources resolved beside the exe, content root scanned, `0 scene(s) discovered` and it kept going
(the brief-18 "clone with no assets still runs" requirement, confirmed on Windows), SDL initialised
its **`windows`** video driver — not X11 — and it then stopped only at the debug validation-layer
check above.

`sandbox/main.cpp` had to be fixed for this: `resolve_resources_directory()` was POSIX-shaped
(`XDG_CONFIG_HOME`, `$HOME/.config/string`). Windows normally has no `HOME`, so a shipped build fell
through to the working directory and only ran when launched from its own folder — a desktop shortcut
sets a different cwd and nothing would load. It now probes beside the executable (via the existing
`string::core::executable_dir()`, which already had a `GetModuleFileNameW` branch) for a `shaders/`
dir, keeping dev builds on their old path.

### Shader program cache + lazy Slang session (2026-08-04)

`slang-compiler.dll` is 33.4MB of a 46MB folder — 73%. The engine itself is 6.7MB. That DLL ships
only because shaders are compiled at runtime.

Two changes toward not needing it, both done and gated:

1. **The disk cache now stores the WHOLE program** (`<key>.program`: every entry point's stage, name
   and SPIR-V, plus the reflected descriptor-set layout and push-constant range), not just SPIR-V.
   Previously the cache was consulted *after* Slang had already parsed, linked and reflected the
   module, so it only skipped codegen — Slang was needed on every run regardless. The lookup now
   happens before any Slang call, keyed on source hash + import-closure hash as before.
2. **The Slang global session is created lazily** (`ensure_global_session`, mutex-guarded for the
   file-watch job pool). A run whose shaders all hit the cache never constructs it.

Laziness alone does NOT let you delete the DLLs: the exe statically imports `slang.dll`, so Windows
refuses to start the process (`rc=53`, loader error) before any of our code runs, warm cache or not.
That is what the build option below is for.

Schema bumped to 6; stale caches are ignored rather than misread. Corrupt/truncated files fall back
to recompiling — covered by a test.

### SHIP MODE: `-Dslang=disabled` — 13MB folder / 5.1MB zip

    meson setup build-win-ship sandbox --cross-file tools/mingw-w64.cross \
      --buildtype release -Db_ndebug=if-release -Dstring-core:slang=disabled \
      -Dc_link_args=-L$PT/lib -Dcpp_link_args=-L$PT/lib

Slang is neither linked nor compiled against; `shader_compiler` serves programs only from the cache.
**Verified: `slang.dll` is gone from the import table**, and the package runs with no Slang DLLs
present, rendering byte-identical (AE=0) to a full build.

    string_demo.exe (6.7MB)  libSDL3.dll (4.4MB)  libmcfgthread-2.dll
    shaders/  assets/fonts/  shadercache/     <- 13MB total, 5.1MB zipped

`shadercache/` beside `shaders/` is the read-only prebuilt cache (`ShaderCompiler` consults the
writable user cache first, then this). **`shaders/` must still ship** even though nothing compiles
them: the cache key is the hash of the source plus its import closure, so the sources are what
prove a cache entry matches. They are 264KB.

To bake the cache: run a slang-ENABLED build, then copy `%LOCALAPPDATA%\string\cache\shaders\*.program`
(Linux: `~/.cache/string/shaders`) into the package's `shadercache/`.

> **Only bake for a `-Dslang=disabled` package, and only from the SAME commit and the same
> `slang-compiler.dll` you ship.** `package-windows.sh` now refuses to bake into a slang-enabled
> package for this reason. The cache key hashes shader source + import closure + schema version —
> **not the Slang compiler version** — and is location-independent by design so a shipped cache hits.
> Bake with one compiler and ship another and the key still matches while the SPIR-V behind it does
> not: the stale entry is *hit*, not skipped, and the GPU hangs on the first frame after a scene
> switch (`FRAME WAIT STALLED`, then `VK_ERROR_DEVICE_LOST` with "No fault detected"). It reproduces
> only on a cold `%LOCALAPPDATA%\string\cache\shaders` — any prior good run masks it, which is why it
> hits fresh machines and not the one that built the package. Folding the compiler version into the
> key seed (`shader_compiler.cpp`, `seed`) would make a mismatched cache miss cleanly and make baking
> safe again.

**A bug this exposed, now fixed:** the cache key folded in each module's *absolute native* path, so a
cache baked at package time missed 100% once the user unzipped it elsewhere — the first ship run
failed with `no cached program ... key e263fb23272342be`. `hash_import_closure` now hashes each
module's path RELATIVE to its search dir in generic (forward-slash) form, making keys independent of
both install location and path separator. Regression test:
`ShaderCompiler.CacheKeyIsIndependentOfInstallLocation`.

**KNOWN GAP — the cache must cover EVERY shader, and warming by running one scene does not.** The
verified package carries 5 `.program` files because that is what the default exterior scene compiles;
the tree has ~26 `.slang` files. In ship mode a shader with no cache entry is a HARD ERROR, so an
artist switching to an uncached scene would hit it. Warming needs a pass that compiles every entry-
point-bearing `.slang` (skipping import-only modules like `probe_common.slang`, which legitimately
declare none). Until that exists, treat `-Dslang=disabled` as verified-for-the-default-scene only.

`shader_compiler::can_compile()` reports which build this is, so shader-authoring UI (hot-reload,
the Shadertoy REPL) can be disabled honestly rather than offering something that cannot work.

### FIXED: a missing font used to be a null deref

With `assets/fonts/` absent the engine faulted in `stbtt__isfont`, because `read_file` returned an
empty vector for a missing file and `dynamic_font_atlas` passed `data()` (null) straight into
stb_truetype, which dereferences it *before* the existing "not a valid TrueType blob" throw. Found
by shipping an incomplete folder — the mistake a first-time packager makes.

Fixed at both layers: `read_file` (`sandbox/demo_scene.cpp`) now throws naming the path, and the
`dynamic_font_atlas` ctor guards an empty span regardless of caller. Falsified by deleting the font
from a real dist and re-running under Wine — the page fault is replaced by
`cannot open '...\assets/fonts/DejaVuSans.ttf' (missing or unreadable)`.

### Release build

    meson setup build-win-rel sandbox --cross-file tools/mingw-w64.cross \
      --buildtype release -Db_ndebug=if-release -Dc_link_args=-L$PT/lib -Dcpp_link_args=-L$PT/lib

`buildtype=release` swaps `-DSTRING_DEBUG` for `-DSTRING_RELEASE` (`string-core/meson.build`), which
is what drops the validation-layer requirement. `b_ndebug=if-release` keeps `NDEBUG` consistent, as
`platform_detection.hpp` keys on it too.

Stripped release: `string_demo.exe` **6.7MB**, `string_cook.exe` 4.8MB, ~173MB folder total.
**Verified under Wine: it ran the full engine for 4.3 minutes and exited cleanly, rc=0, no errors.**
Note release compiles TRACE/DEBUG/INFO/WARN to `((void)0)` — only ERROR and CRITICAL survive — so a
silent log is expected and is not evidence of a silent failure.

### Open: the wrap and nix builds cook DIFFERENT assets

Cooking the same glTF gave **43305 meshlets cross-built vs 41939 native-nix**, from byte-identical
input (vert/index counts match exactly). Cause is NOT the toolchain: **nixpkgs ships meshoptimizer
1.2 while `meshoptimizer.wrap` pins 0.21.** So cooked assets are not interchangeable between the
two build paths. Left alone deliberately — bumping either side moves cooked-asset baselines, which
is a decision with gate consequences, not a cleanup.

### clang-mingw does NOT work on this nixpkgs pin

See the header comment in `tools/mingw-w64-clang.cross` for the evidence. Two independent, fatal,
upstream blockers: libstdc++'s mcfgthread path uses `#include __FILE__`, which clang expands with
backslashes on a Windows target (unresolvable on Linux); and the libc++ alternative forces a
compiler-rt rebuild that fails its own CMake compiler probe (`cannot find crt2.o`). Neither is
fixable from this repo. Use the gcc cross file.

## What "done" looks like

Not a green `meson setup` on this machine — that only proves Linux. The real gate is **a Windows
build from a clean clone, by someone who has never run nix**, since that is the actual delivery
target. The cross-compile above is a strong signal — it is a real PE that really runs — but it is
still GCC-family on a Linux host, not clang-with-MSVC-ABI on Windows.

A CI job would keep it honest; there is no `.github/workflows` yet, and "Windows build in CI" is
still an open Phase A exit-gate item in `docs/briefs/README.md`.
