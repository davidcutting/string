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
      perSystem = { config, self', inputs', pkgs, system, ... }: {

        # packages.default builds the engine (static lib + `string_demo`)
        # directly with Meson, using the pinned nixpkgs toolchain and deps.
        # Build with `nix build`, run with `nix run` (or ./result/bin/string_demo).
        packages.default = pkgs.clangStdenv.mkDerivation {
          pname = "string-engine";
          version = "0.0.1";
          src = ./string-engine;

          # meson project root is string-engine/ (where meson.build lives).
          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
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
          ];

          # Default WSI is glfw; flip to sdl/wayland here if desired.
          mesonFlags = [
            "-Dwsi=sdl"     # sdl_window is current; glfw port is a follow-up
            "-Dtests=false"
            "-Ddemo=true"
          ];

          # Point the demo at the shaders installed under $out/include/string/shaders, and
          # make the Khronos validation layer discoverable (debug builds require it). This
          # lets `nix run` / ./result/bin/string_demo work without the devShell env.
          postInstall = ''
            wrapProgram $out/bin/string_demo \
              --set STRING_RESOURCES_DIR $out/include/string \
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
              glslang
              shader-slang
              vulkan-headers
              vulkan-loader
              vulkan-validation-layers
              vulkan-tools
              vulkan-tools-lunarg
              vulkan-volk
              vulkan-memory-allocator
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
