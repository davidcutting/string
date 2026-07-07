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
          mesonFlags = [ "-Dwsi=sdl" "-Dtests=true" "-Ddemo=false" ];
          postInstall = "";   # no demo binary to wrap when -Ddemo=false
        });

        # packages.default builds the engine (static lib + `string_demo`)
        # directly with Meson, using the pinned nixpkgs toolchain and deps.
        # Build with `nix build`, run with `nix run` (or ./result/bin/string_demo).
        packages.default = pkgs.clangStdenv.mkDerivation {
          pname = "string-engine";
          version = "0.0.1";
          # Whole repo: the meson project root is now the top-level meson.build, which builds
          # the string-engine/ library and the sandbox/ demo application.
          src = ./.;

          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            cmake         # lets Meson's cmake dependency method find fastgltf (no .pc, cmake-only)
            makeWrapper   # wrap string_demo to point at its installed resources
            glslang       # glslangValidator, for GLSL -> SPIR-V
            shader-slang  # slangc, for Slang -> SPIR-V
          ];

          buildInputs = with pkgs; [
            spdlog
            glm
            entt
            glfw                       # WSI backend (-Dwsi=glfw)
            sdl3                       # WSI backend (-Dwsi=sdl); current default
            vulkan-headers
            vulkan-loader
            vulkan-memory-allocator    # vk_mem_alloc.h
            vulkan-volk                # volk.h (Vulkan meta-loader)
            shader-slang               # libslang runtime (shader.hpp)
            fastgltf                   # glTF 2.0 importer (Sponza et al.)
            simdjson                   # fastgltf's (public) dependency; needed on the prefix path
          ];

          # Default WSI is glfw; flip to sdl/wayland here if desired.
          mesonFlags = [
            "-Dwsi=sdl"     # sdl_window is current; glfw port is a follow-up
            "-Dtests=false"
            "-Ddemo=true"
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

          # Point the demo at the shaders installed under $out/include/string/shaders, and
          # make the Khronos validation layer discoverable (debug builds require it). This
          # lets `nix run` / ./result/bin/string_demo work without the devShell env.
          postInstall = ''
            # Symlink assets under STRING_RESOURCES_DIR rather than copying them into the
            # output. They live once in the store (content-addressed) and are shared across
            # builds, so large models don't get duplicated into every build result. Assets are
            # app content, so they live under sandbox/.
            ln -s ${./sandbox/assets} $out/include/string/assets

            wrapProgram $out/bin/string_demo \
              --set-default STRING_RESOURCES_DIR $out/include/string \
              --prefix VK_LAYER_PATH : ${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d \
              --prefix LD_LIBRARY_PATH : ${pkgs.vulkan-validation-layers}/lib
          '';

          meta.mainProgram = "string_demo";
        };

        # devShells.default describes the default shell with C++, cmake, boost,
        # and catch2
        devShells = {
          default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
            packages = with pkgs; [
              clang-tools
              cmake
              pkg-config
              llvm
              lcov
              gdb
              dbus
              gtest
              gcc
              gcovr
              meson
              ninja
              wget
              entt
              yaml-cpp
              argparse
              spdlog
              glm
              glfw
              sdl3
              glslang
              shader-slang
              vulkan-headers
              vulkan-loader
              vulkan-validation-layers
              vulkan-tools
              vulkan-tools-lunarg
              vulkan-volk
              vulkan-memory-allocator
              fastgltf
              simdjson
              liburing
              wayland
              perf
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
