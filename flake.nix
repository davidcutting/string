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
        # wrap under string-engine/subprojects/ instead.
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
      in {

        # `nix flake check` builds the engine with the gtest suite enabled and runs it. This is
        # what keeps the header-only bits (e.g. core/layout.hpp) actually compiled + verified —
        # the default package build never includes them.
        checks.default = self'.packages.default.overrideAttrs (old: {
          pname = "string-engine-tests";
          doCheck = true;
          buildInputs = old.buildInputs ++ [ pkgs.gtest ];
          mesonFlags = [ "-Dwsi=sdl" "-Dtests=true" ];
        });

        # packages.default builds ONLY the engine library (String, as intended). Its meson
        # project is self-contained under string-engine/ (own meson.build, meson_options.txt,
        # and subprojects/). Off-nix the deps come from the meson wraps; here they come from
        # nixpkgs (nix builds have no network, so `--wrap-mode=nodownload` is in effect and the
        # wraps are never fetched). Build with `nix build`.
        packages.default = pkgs.clangStdenv.mkDerivation {
          pname = "string-engine";
          version = "0.0.1";
          src = ./string-engine;

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
          ];

          mesonFlags = [
            "-Dwsi=sdl"
            "-Dtests=false"
          ];

          # Build with debug info (nixpkgs' meson hook defaults to --buildtype=plain, which
          # drops -g) and keep it in the binary, so gdb has line numbers + locals.
          mesonBuildType = "debug";
          # cmake is present only so Meson can read fastgltf's cmake config; don't let its
          # setup hook take over the configure phase from Meson.
          dontUseCmakeConfigure = true;
          dontStrip = true;
          # _FORTIFY_SOURCE (a default hardening flag) requires -O; the debug build is -O0,
          # so drop just that flag to avoid a warning on every TU.
          hardeningDisable = [ "fortify" ];
        };

        # packages.demo builds the sandbox app as an EXTERNAL consumer of the engine: sandbox/
        # is its own meson project that pulls string-engine in as a subproject (via the
        # sandbox/subprojects/string-engine link to the sibling engine). This is both the
        # worked example of "how to include String" and the integration test that the exported
        # dependency links. Build/run with `nix run .#demo`.
        packages.demo = pkgs.clangStdenv.mkDerivation {
          pname = "string-demo";
          version = "0.0.1";
          # Whole repo so both sandbox/ and its string-engine subproject link are present.
          # The flake self-src unpacks under a hash-named dir, so glob for the sandbox subdir
          # rather than hardcoding it.
          src = ./.;
          setSourceRoot = "sourceRoot=$(echo */sandbox)";

          nativeBuildInputs = with pkgs; [
            meson ninja pkg-config cmake makeWrapper glslang
          ];

          buildInputs = with pkgs; [
            spdlog glm entt sdl3
            vulkan-headers vulkan-memory-allocator vulkan-volk
            shader-slang fastgltf simdjson
          ];

          # WSI is an option of the engine subproject, so it is namespaced.
          mesonFlags = [ "-Dstring-engine:wsi=sdl" ];

          mesonBuildType = "debug";
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
