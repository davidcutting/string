#!/usr/bin/env bash

set -e

# Ensure PKG_CONFIG_PATH includes local installation
export PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig:$PKG_CONFIG_PATH"

echo "Building string-engine..."
cd string-engine
meson setup build --prefix="$HOME/.local" --reconfigure
ninja -C build clean
ninja -C build -j16
ninja -C build install
cd ..

echo "Building string-sandbox..."
cd string-sandbox
meson setup build --prefix="$HOME/.local" --reconfigure
ninja -C build clean
ninja -C build -j16
ninja -C build install
cd ..

echo "Build complete!"
echo "Libraries installed to: $HOME/.local/lib"
echo "Headers installed to: $HOME/.local/include"
