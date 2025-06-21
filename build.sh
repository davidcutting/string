#!/usr/bin/env bash

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
BUILD_TYPE="debug"
TARGET="native"
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
CLEAN=false
INSTALL_DIR="$HOME/.local"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --target)
            TARGET="$2"
            shift 2
            ;;
        --buildtype)
            BUILD_TYPE="$2"
            shift 2
            ;;
        --jobs|-j)
            JOBS="$2"
            shift 2
            ;;
        --clean)
            CLEAN=true
            shift
            ;;
        --install-dir)
            INSTALL_DIR="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo "Options:"
            echo "  --target TARGET      Build target: native, windows (default: native)"
            echo "  --buildtype TYPE     Build type: debug, release, debugoptimized (default: debug)"
            echo "  --jobs|-j JOBS       Number of parallel jobs (default: $JOBS)"
            echo "  --clean              Clean build directories before building"
            echo "  --install-dir DIR    Installation directory (default: $INSTALL_DIR)"
            echo "  --help|-h            Show this help message"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Print configuration
echo -e "${BLUE}=== Build Configuration ===${NC}"
echo -e "Target: ${YELLOW}$TARGET${NC}"
echo -e "Build Type: ${YELLOW}$BUILD_TYPE${NC}"
echo -e "Jobs: ${YELLOW}$JOBS${NC}"
echo -e "Install Dir: ${YELLOW}$INSTALL_DIR${NC}"
echo -e "Clean: ${YELLOW}$CLEAN${NC}"
echo -e "Compiler: ${YELLOW}Clang + libc++${NC}"
echo ""

# Function to build native
build_native() {
    local project_dir="$1"
    local build_dir="build"
    
    echo -e "${GREEN}Building $project_dir (native)...${NC}"
    cd "$project_dir"
    
    if [[ "$CLEAN" == true ]] && [[ -d "$build_dir" ]]; then
        echo -e "${YELLOW}Cleaning $build_dir...${NC}"
        rm -rf "$build_dir"
    fi
    
    # Ensure PKG_CONFIG_PATH includes local installation
    export PKG_CONFIG_PATH="$INSTALL_DIR/lib/pkgconfig:$PKG_CONFIG_PATH"
    
    meson setup "$build_dir" \
        --prefix="$INSTALL_DIR" \
        --buildtype="$BUILD_TYPE" \
        --default-library=static \
        --reconfigure
    
    ninja -C "$build_dir" clean || true
    ninja -C "$build_dir" -j"$JOBS"
    ninja -C "$build_dir" install
    
    cd ..
}

# Function to build for Windows
build_windows() {
    local project_dir="$1"
    local build_dir="build-windows"
    
    echo -e "${GREEN}Building $project_dir (Windows x64)...${NC}"
    cd "$project_dir"
    
    if [[ "$CLEAN" == true ]] && [[ -d "$build_dir" ]]; then
        echo -e "${YELLOW}Cleaning $build_dir...${NC}"
        rm -rf "$build_dir"
    fi
    
    # Check if we're in the Nix flake Windows environment
    if [[ -z "$CROSS_COMPILE_TARGET" ]]; then
        echo -e "${RED}Error: Not in Windows cross-compilation environment${NC}"
        echo -e "${YELLOW}Please run: nix develop .#windows${NC}"
        exit 1
    fi
    
    # Use the cross-compilation file from the Nix environment
    local cross_file
    if [[ -n "$CLANG_CROSS_FILE" ]]; then
        cross_file="$CLANG_CROSS_FILE"
    else
        # Fallback: create a temporary cross-compilation file
        cross_file="$(mktemp)"
        cat > "$cross_file" << EOF
[binaries]
c = 'clang'
cpp = 'clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkgconfig = 'pkg-config'

[built-in options]
c_args = ['-target', 'x86_64-pc-windows-gnu']
cpp_args = ['-target', 'x86_64-pc-windows-gnu', '-stdlib=libc++']
c_link_args = ['-target', 'x86_64-pc-windows-gnu', '-static']
cpp_link_args = ['-target', 'x86_64-pc-windows-gnu', '-static', '-stdlib=libc++', '-lc++', '-lc++abi']

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
    fi
    
    local install_dir="$(pwd)/install-windows"
    
    meson setup "$build_dir" \
        --cross-file="$cross_file" \
        --prefix="$install_dir" \
        --buildtype="$BUILD_TYPE" \
        --default-library=static \
        --reconfigure
    
    ninja -C "$build_dir" clean || true
    ninja -C "$build_dir" -j"$JOBS"
    ninja -C "$build_dir" install
    
    echo -e "${GREEN}Windows build installed to: $install_dir${NC}"
    
    # Clean up temporary cross file if we created one
    if [[ -z "$CLANG_CROSS_FILE" ]]; then
        rm -f "$cross_file"
    fi
    
    cd ..
}

# Main build logic
case "$TARGET" in
    native)
        echo -e "${BLUE}=== Native Build ===${NC}"
        if [[ -d "string-engine" ]]; then
            build_native "string-engine"
        else
            echo -e "${RED}Error: string-engine directory not found${NC}"
            exit 1
        fi
        
        if [[ -d "string-sandbox" ]]; then
            build_native "string-sandbox"
        else
            echo -e "${YELLOW}Warning: string-sandbox directory not found, skipping${NC}"
        fi
        ;;
        
    windows)
        echo -e "${BLUE}=== Windows Cross-compilation ===${NC}"
        if [[ -d "string-engine" ]]; then
            build_windows "string-engine"
        else
            echo -e "${RED}Error: string-engine directory not found${NC}"
            exit 1
        fi
        
        if [[ -d "string-sandbox" ]]; then
            build_windows "string-sandbox"
        else
            echo -e "${YELLOW}Warning: string-sandbox directory not found, skipping${NC}"
        fi
        ;;
        
    *)
        echo -e "${RED}Error: Unknown target '$TARGET'${NC}"
        echo -e "${YELLOW}Supported targets: native, windows${NC}"
        exit 1
        ;;
esac

echo -e "${GREEN}=== Build Complete! ===${NC}"

if [[ "$TARGET" == "native" ]]; then
    echo -e "Libraries installed to: ${YELLOW}$INSTALL_DIR/lib${NC}"
    echo -e "Headers installed to: ${YELLOW}$INSTALL_DIR/include${NC}"
elif [[ "$TARGET" == "windows" ]]; then
    echo -e "Windows binaries available in respective ${YELLOW}install-windows${NC} directories"
fi