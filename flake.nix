{
  description = "String Engine Development";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs = inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      # This is the list of architectures that work with this project
      systems = [
        "x86_64-linux" "aarch64-linux"
      ];
      perSystem = { config, self', inputs', pkgs, system, ... }:
      let
        # fastgltf isn't in nixpkgs yet, so build it from a hash-pinned source. Upstream is
        # CMake-only; find_package(simdjson CONFIG) picks up nixpkgs' simdjson (deps download
        # is thus skipped), and the CMake install exports fastgltfConfig.cmake, which Meson's
        # dependency('fastgltf') resolves via its cmake method. Off-Nix builds use the meson
        # wrap under string-core/subprojects/ instead.
        fastgltf = pkgs.stdenv.mkDerivation {
          pname = "fastgltf";
          version = "0.8.0";
          src = pkgs.fetchFromGitHub {
            owner = "spnda";
            repo = "fastgltf";
            rev = "v0.8.0";
            hash = "sha256-nPkVMrve1+04XRpSswBqjlSurliCFBRxR8Zxy4xZ5pE=";
          };
          nativeBuildInputs = [ pkgs.cmake ];
          buildInputs = [ pkgs.simdjson ];
          cmakeFlags = [
            "-DFASTGLTF_ENABLE_TESTS=OFF"
            "-DFASTGLTF_ENABLE_EXAMPLES=OFF"
          ];
        };

        # ozz-animation (brief 23): skeletal animation — runtime sampling/blending (ozz_base +
        # ozz_animation, linked by string-core) and the offline builders (ozz_animation_offline,
        # consumed by string-asset-tools at cook time). Not in nixpkgs, so hash-pinned like
        # fastgltf. Unlike fastgltf, ozz exports NO CMake config package (no install(EXPORT)) —
        # the install is plain lib/ + include/ — so string-core's meson.build probes it with
        # find_library(), not dependency(method:'cmake'). Off-nix builds use the meson wrap
        # under string-core/subprojects/ instead; ITS VERSION MUST TRACK THIS ONE.
        ozz-animation = pkgs.stdenv.mkDerivation {
          pname = "ozz-animation";
          version = "0.17.0";
          src = pkgs.fetchFromGitHub {
            owner = "guillaumeblanc";
            repo = "ozz-animation";
            rev = "0.17.0"; # tags carry no `v` prefix
            hash = "sha256-ugAjnJZye7VxnTuVsrY8Hb0Q8AD+/NaoDaAlCzSzTBA=";
          };
          nativeBuildInputs = [ pkgs.cmake ]; # 0.17.0 needs cmake >= 3.30
          cmakeFlags = [
            "-Dozz_build_samples=OFF"
            "-Dozz_build_howtos=OFF"
            "-Dozz_build_tests=OFF"
            "-Dozz_build_tools=OFF" # also force-disables the fbx/gltf importers
            "-Dozz_build_data=OFF"
            # Default ON appends per-config suffixes (libozz_animation_r.a) that find_library
            # can never resolve.
            "-Dozz_build_postfix=OFF"
            # 0.17.0 sets CMAKE_COMPILE_WARNING_AS_ERROR unconditionally; a new compiler
            # warning must not break the dependency build.
            "-DCMAKE_COMPILE_WARNING_AS_ERROR=OFF"
            "-DBUILD_SHARED_LIBS=OFF"
          ];
        };
      in {

        # `nix flake check` builds the engine with the gtest suite enabled and runs it. This is
        # what keeps the header-only bits (e.g. core/layout.hpp) actually compiled + verified —
        # the default package build never includes them.
        checks.default = self'.packages.default.overrideAttrs (old: {
          pname = "string-core-tests";
          doCheck = true;
          buildInputs = old.buildInputs ++ [ pkgs.gtest ];
          mesonFlags = [ "-Dwsi=sdl" "-Dtests=true" ];
        });

        # `nix flake check` also builds the demo with the string-asset-tools tests enabled and runs them
        # (brief 04b). This proves the cook LIBRARY builds + its cooked-format round-trip and
        # cook-twice-byte-identical determinism gates pass, and that the `.#cook` CLI compiles.
        # Mirrors packages.demo's build inputs (sandbox pulls the libraries in as subprojects) plus
        # gtest for -Dstring-asset-tools:tests=true.
        checks.cook = self'.packages.demo.overrideAttrs (old: {
          pname = "string-cook-tests";
          doCheck = true;
          buildInputs = old.buildInputs ++ [ pkgs.gtest ];
          mesonFlags = old.mesonFlags ++ [ "-Dstring-asset-tools:tests=true" ];
        });

        # `nix flake check` also runs the UI kit's suite (layout, text wrap, widgets, interaction,
        # panels, docking). Kept a SEPARATE check from cook so a failure names the library it came
        # from. string-ui depends on nothing, so this needs no device and no engine — it is built
        # through the demo only because that is where the subproject links live.
        checks.ui = self'.packages.demo.overrideAttrs (old: {
          pname = "string-ui-tests";
          doCheck = true;
          buildInputs = old.buildInputs ++ [ pkgs.gtest ];
          mesonFlags = old.mesonFlags ++ [ "-Dstring-ui:tests=true" ];
        });

        # `nix flake check` also runs the scene library's suite (camera; later the world tables and
        # the asset registry). Separate check so a failure names the library, same as checks.ui.
        checks.scene = self'.packages.demo.overrideAttrs (old: {
          pname = "string-scene-tests";
          doCheck = true;
          buildInputs = old.buildInputs ++ [ pkgs.gtest ];
          mesonFlags = old.mesonFlags ++ [ "-Dstring-scene:tests=true" ];
        });

        # `.#cook` (brief 04b M3): the offline asset-cook CLI. packages.demo installs the
        # `string_cook` binary from the string-asset-tools subproject, so the app just points at
        # it. Usage: `nix run .#cook -- [--chunk N | --no-chunk] [--no-textures] <file.gltf> ...` —
        # cooks each source glTF to a `.c<budget>.cooked` blob next to it, cooks that scene's
        # textures to KTX2/BC7 siblings, and maintains the `.cook_manifest` sidecar (incremental:
        # fresh entries are skipped). This is the pre-cook path the engine's in-process-cook WARN
        # hint tells the user to run.
        apps.cook = {
          type = "app";
          program = "${self'.packages.demo}/bin/string_cook";
        };

        # packages.default builds ONLY the engine library (String, as intended). Its meson
        # project is self-contained under string-core/ (own meson.build, meson_options.txt,
        # and subprojects/). Off-nix the deps come from the meson wraps; here they come from
        # nixpkgs (nix builds have no network, so `--wrap-mode=nodownload` is in effect and the
        # wraps are never fetched). Build with `nix build`.
        packages.default = pkgs.clangStdenv.mkDerivation {
          pname = "string-core";
          version = "0.0.1";
          src = ./string-core;

          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            cmake         # lets Meson's cmake dependency method read fastgltf's config (no .pc)
            glslang       # glslangValidator, for GLSL -> SPIR-V
          ];

          buildInputs = with pkgs; [
            spdlog
            glm
            entt
            sdl3                       # WSI backend (-Dwsi=sdl)
            vulkan-headers             # headers only; volk loads the loader at runtime (no link)
            vulkan-memory-allocator    # vk_mem_alloc.h
            vulkan-volk                # volk.h (impl compiled in src/gpu/driver.cpp)
            shader-slang               # libslang runtime (shader.hpp)
            fastgltf                   # glTF 2.0 importer (custom derivation below; not in nixpkgs)
            simdjson                   # system fastgltf does not bundle simdjson; link it alongside
            ozz-animation              # skeletal animation runtime (custom derivation; brief 23)
          ];

          mesonFlags = [
            "-Dwsi=sdl"
            "-Dtests=false"
          ];

          # Build with debug info (nixpkgs' meson hook defaults to --buildtype=plain, which
          # drops -g) and keep it in the binary, so gdb has line numbers + locals.
          mesonBuildType = "debugoptimized";
          # cmake is present only so Meson can read fastgltf's cmake config; don't let its
          # setup hook take over the configure phase from Meson.
          dontUseCmakeConfigure = true;
          dontStrip = true;
          # _FORTIFY_SOURCE (a default hardening flag) requires -O; the debug build is -O0,
          # so drop just that flag to avoid a warning on every TU.
          hardeningDisable = [ "fortify" ];
        };

        # packages.demo builds the sandbox app as an EXTERNAL consumer of the engine: sandbox/
        # is its own meson project that pulls string-core in as a subproject (via the
        # sandbox/subprojects/string-core link to the sibling engine). This is both the
        # worked example of "how to include String" and the integration test that the exported
        # dependency links. Build/run with `nix run .#demo`.
        packages.demo = pkgs.clangStdenv.mkDerivation {
          pname = "string-demo";
          version = "0.0.1";
          # Whole repo so both sandbox/ and its string-core subproject link are present.
          # The flake self-src unpacks under a hash-named dir, so glob for the sandbox subdir
          # rather than hardcoding it.
          src = ./.;
          setSourceRoot = "sourceRoot=$(echo */sandbox)";

          nativeBuildInputs = with pkgs; [
            meson ninja pkg-config cmake makeWrapper glslang
            ktx-tools     # `ktx` CLI, kept for inspecting/debugging cooked KTX2 files
          ];

          buildInputs = with pkgs; [
            spdlog glm entt sdl3
            vulkan-headers vulkan-memory-allocator vulkan-volk
            shader-slang fastgltf simdjson
            ozz-animation   # skeletal animation: runtime for the engine + offline builders for the cook
            ktx-tools       # libktx: the sandbox loader reads/transcodes .ktx2 (KTX::ktx cmake config)
            meshoptimizer   # meshlet build + LOD simplify (brief 03; meshoptimizer::meshoptimizer)
            nlohmann_json   # `.scene.json` descriptors, parsed app-side (brief 18)
          ];

          # WSI is an option of the engine subproject, so it is namespaced.
          mesonFlags = [ "-Dstring-core:wsi=sdl" ];

          mesonBuildType = "debugoptimized";
          dontUseCmakeConfigure = true;
          dontStrip = true;
          hardeningDisable = [ "fortify" ];

          # Point the demo at the shaders installed under $out/include/string/shaders, and make
          # the loader + Khronos validation layer discoverable (debug builds require it), so
          # `nix run .#demo` / ./result/bin/string_demo work without the devShell env.
          postInstall = ''
            # Symlink assets under STRING_RESOURCES_DIR rather than copying them in: they live
            # once in the store (content-addressed) and are shared across builds, so large
            # models don't get duplicated into every result.
            ln -s ${./sandbox/assets} $out/include/string/assets

            wrapProgram $out/bin/string_demo \
              --set-default STRING_RESOURCES_DIR $out/include/string \
              --prefix VK_LAYER_PATH : ${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d \
              --prefix LD_LIBRARY_PATH : ${pkgs.vulkan-loader}/lib:${pkgs.vulkan-validation-layers}/lib
          '';

          meta.mainProgram = "string_demo";
        };

        # packages.demo-tracy is packages.demo built with the Tracy profiler client compiled in
        # (-Dstring-core:tracy=true, which defines STRING_PROFILE and links the tracy wrap). The
        # normal .#demo stays Tracy-free. Run with `nix run .#demo-tracy` then attach the Tracy
        # viewer (`tracy` in the devShell / nixpkgs) — Connect to localhost, the client broadcasts
        # on the LAN and streams zones live. Build/run: `nix run .#demo-tracy`.
        packages.demo-tracy = self'.packages.demo.overrideAttrs (old: {
          pname = "string-demo-tracy";
          # The Tracy client comes from nixpkgs (CMake config Tracy::TracyClient); the engine's
          # meson.build probes that before the (undownloadable-in-nix) wrap.
          buildInputs = old.buildInputs ++ [ pkgs.tracy ];
          mesonFlags = (old.mesonFlags or []) ++ [ "-Dstring-core:tracy=true" ];
        });

        # devShells.default describes the default shell with C++, cmake, boost,
        # and catch2
        devShells = {
          default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
            packages = with pkgs; [
              # Toolchain
              meson ninja pkg-config cmake
              clang-tools gdb
              # Shader compilers
              glslang shader-slang
              # Engine dependencies (mirror the wraps; here from nixpkgs)
              spdlog glm entt sdl3
              vulkan-headers vulkan-memory-allocator vulkan-volk
              fastgltf simdjson
              ozz-animation               # skeletal animation runtime + offline builders (brief 23)
              ktx-tools                   # libktx + `ktx` CLI (texture cook + runtime load)
              meshoptimizer               # meshlet build + LOD simplify (brief 03)
              gtest                       # for -Dtests=true in-shell
              # Profiler: the tracy client links into the engine (-Dtracy=true); this is also
              # the standalone Tracy viewer so you don't have to build it manually.
              tracy
              # EXPERIMENTAL Linux WSI (wsi=wayland), not yet wired in code
              wayland
              # Runtime: loader + validation layers + tools for running the demo
              vulkan-loader vulkan-validation-layers vulkan-tools
            ];
            shellHook = ''
            export LD_LIBRARY_PATH="${pkgs.vulkan-loader}/lib:${pkgs.wayland}/lib:$LD_LIBRARY_PATH"
            export VK_LAYER_PATH="${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d"
            '';
          };
        };
      };
    };
}
