{
  description = "String Engine Development";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs = inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [
        "x86_64-linux" "aarch64-linux" "aarch64-darwin" "x86_64-darwin"
      ];
      
      perSystem = { config, self', inputs', pkgs, system, ... }: 
      let
        # Windows cross-compilation setup
        mingwPkgs = import inputs.nixpkgs {
          inherit system;
          crossSystem = {
            config = "x86_64-w64-mingw32";
            libc = "msvcrt";
          };
        };

        # Clang cross-compilation file for Windows
        clangCrossFile = pkgs.writeText "cross-windows-clang.ini" ''
          [binaries]
          c = '${pkgs.llvmPackages.clang}/bin/clang'
          cpp = '${pkgs.llvmPackages.clang}/bin/clang++'
          ar = '${pkgs.llvmPackages.llvm}/bin/llvm-ar'
          strip = '${pkgs.llvmPackages.llvm}/bin/llvm-strip'
          pkgconfig = '${pkgs.pkg-config}/bin/pkg-config'

          [built-in options]
          c_args = ['-target', 'x86_64-pc-windows-gnu', '--sysroot=${mingwPkgs.stdenv.cc.libc}']
          cpp_args = ['-target', 'x86_64-pc-windows-gnu', '--sysroot=${mingwPkgs.stdenv.cc.libc}', '-stdlib=libc++']
          c_link_args = ['-target', 'x86_64-pc-windows-gnu', '--sysroot=${mingwPkgs.stdenv.cc.libc}', '-static']
          cpp_link_args = ['-target', 'x86_64-pc-windows-gnu', '--sysroot=${mingwPkgs.stdenv.cc.libc}', '-static', '-stdlib=libc++', '-lc++', '-lc++abi']

          [properties]
          needs_exe_wrapper = true
          
          [host_machine]
          system = 'windows'
          cpu_family = 'x86_64'
          cpu = 'x86_64'
          endian = 'little'
        '';

        # Build script for Windows cross-compilation
        buildScriptWindows = pkgs.writeShellScriptBin "build-windows" ''
          set -e
          
          echo "Building string-engine for Windows with Clang + libc++..."
          
          # Set up environment for cross-compilation
          export CC="${pkgs.llvmPackages.clang}/bin/clang"
          export CXX="${pkgs.llvmPackages.clang}/bin/clang++"
          export AR="${pkgs.llvmPackages.llvm}/bin/llvm-ar"
          export STRIP="${pkgs.llvmPackages.llvm}/bin/llvm-strip"
          
          cd string-engine
          
          # Clean previous builds
          rm -rf build-windows
          
          # Setup meson with clang cross-compilation
          meson setup build-windows \
            --cross-file ${clangCrossFile} \
            --prefix="$(pwd)/install-windows" \
            --default-library=static
          
          # Build
          ninja -C build-windows -j$(nproc)
          ninja -C build-windows install
          
          echo "Windows build complete!"
          echo "Output located in: $(pwd)/install-windows"
          
          cd ..
        '';

      in {
        devShells = {
          # Native development shell with Clang + libc++
          default = pkgs.mkShell.override { 
            stdenv = pkgs.llvmPackages.stdenv; 
          } {
            packages = with pkgs; [
              # Clang toolchain
              llvmPackages.clang
              llvmPackages.clang-tools
              llvmPackages.llvm
              llvmPackages.libcxx
              
              # Build tools
              meson
              ninja
              pkg-config
              
              # Development tools
              gdb
              lcov
              gtest
              gcovr
              
              # Dependencies
              entt
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
              
              # Performance tools
              linuxPackages_latest.perf
            ];
            
            shellHook = ''
              export LD_LIBRARY_PATH=${pkgs.wayland}/lib:$LD_LIBRARY_PATH
              export CC=clang
              export CXX=clang++
              export CXXFLAGS="-stdlib=libc++"
              export LDFLAGS="-stdlib=libc++ -lc++ -lc++abi"
            '';
          };

          # Windows cross-compilation shell
          windows = pkgs.mkShell.override { 
            stdenv = pkgs.llvmPackages.stdenv; 
          } {
            packages = with pkgs; [
              # Clang cross-compilation toolchain
              llvmPackages.clang
              llvmPackages.llvm
              llvmPackages.libcxx
              llvmPackages.libcxxabi
              
              # Build tools
              meson
              ninja
              pkg-config
              
              # Development tools
              gdb
              
              # Custom build scripts
              buildScriptWindows
              
              # Windows dependencies (static versions)
              mingwPkgs.pkgsStatic.glfw3
              mingwPkgs.pkgsStatic.spdlog
              mingwPkgs.vulkan-headers
              glm
              entt
            ];
            
            shellHook = ''
              echo "Windows cross-compilation environment loaded (Clang + libc++)!"
              echo ""
              echo "Available commands:"
              echo "  build-windows - Build with Clang cross-compiler for Windows"
              echo ""
              echo "Cross-compilation file: ${clangCrossFile}"
              echo ""
              echo "Target: x86_64-pc-windows-gnu (Windows 10/11 compatible)"
              echo ""
              
              # Set up environment variables
              export CROSS_COMPILE_TARGET="x86_64-pc-windows-gnu"
              export WINDOWS_SYSROOT="${mingwPkgs.stdenv.cc.libc}"
              export CC=clang
              export CXX=clang++
            '';
          };
        };
        
        # Packages for direct building
        packages = {
          # Cross-compilation file
          clang-cross-file = pkgs.writeText "meson-cross-windows-clang.ini" (builtins.readFile clangCrossFile);

          # Windows cross-compiled build
          windows = pkgs.stdenv.mkDerivation {
            pname = "string-engine";
            version = "0.0.1";
            
            src = ./.;
            
            nativeBuildInputs = with pkgs; [
              meson
              ninja
              pkg-config
              llvmPackages.clang
              llvmPackages.llvm
            ];
            
            buildInputs = with mingwPkgs.pkgsStatic; [
              glfw3
              spdlog
              vulkan-headers
            ] ++ (with pkgs; [
              glm
              entt
            ]);
            
            mesonFlags = [
              "--cross-file=${clangCrossFile}"
              "--default-library=static"
              "-Dbuildtype=release"
            ];
            
            buildPhase = ''
              cd string-engine
              meson setup build $mesonFlags --prefix=$out
              ninja -C build -j$NIX_BUILD_CORES
            '';
            
            installPhase = ''
              ninja -C build install
            '';
            
            meta = with pkgs.lib; {
              description = "Simple Tiny RenderING engine";
              license = licenses.mit;
              platforms = platforms.all;
            };
          };

          # Default package (native build)
          default = pkgs.stdenv.mkDerivation {
            pname = "string-engine";
            version = "0.0.1";
            
            src = ./.;
            
            nativeBuildInputs = with pkgs; [
              meson
              ninja
              pkg-config
              llvmPackages.clang
            ];
            
            buildInputs = with pkgs; [
              glfw
              spdlog
              vulkan-headers
              vulkan-loader
              glm
              entt
            ];
            
            mesonFlags = [
              "--default-library=static"
              "-Dbuildtype=release"
            ];
            
            buildPhase = ''
              cd string-engine
              meson setup build $mesonFlags --prefix=$out
              ninja -C build -j$NIX_BUILD_CORES
            '';
            
            installPhase = ''
              ninja -C build install
            '';
            
            meta = with pkgs.lib; {
              description = "Simple Tiny RenderING engine";
              license = licenses.mit;
              platforms = platforms.all;
            };
          };
        };
      };
    };
}