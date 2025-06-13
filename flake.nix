{
  description = "Vulkan C++";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      # This is the list of architectures that work with this project
      systems = [
        "x86_64-linux" "aarch64-linux" "aarch64-darwin" "x86_64-darwin"
      ];
      perSystem = { config, self', inputs', pkgs, system, ... }: {

        # devShells.default describes the default shell with C++, cmake, boost,
        # and catch2
        devShells = {
          gnu = pkgs.mkShell {
            packages = with pkgs; [
              gtest
              meson
              ninja
            ];
          };

          # macOS users use pkgs.gccStdenv instead of pkgs.clangStdenv

          default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
            packages = with pkgs; [
              clang-tools
              cmake
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
              linuxPackages_latest.perf
            ];
            shellHook = "export LD_LIBRARY_PATH=${pkgs.wayland}/lib:$LD_LIBRARY_PATH";
          };
        };
      };
    };
}
