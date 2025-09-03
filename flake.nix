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

        # devShells.default describes the default shell with C++, cmake, boost,
        # and catch2
        devShells = {
          default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
            packages = with pkgs; [
              clang-tools
              cmake
              zig
              zls
              pkg-config
              llvm
              lcov
              gdb
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
              volk
              vulkan-memory-allocator
              liburing
              wayland
              linuxPackages_latest.perf
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
