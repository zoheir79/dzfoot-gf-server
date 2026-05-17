#!/bin/bash
set -e

# Build GF Server (Linux)
# Requirements: cmake 3.22+, g++11, libenet-dev

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo "Build complete: $BUILD_DIR/gf_server"
