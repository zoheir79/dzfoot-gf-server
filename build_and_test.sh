#!/bin/bash
# DZFoot 60Hz Refactor — Build & Test Script
# Usage: ./build_and_test.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "========================================"
echo "DZFoot 60Hz Refactor — Build & Test"
echo "========================================"

# 1. Configure
echo "[1/4] Configuring CMake..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release

# 2. Build server + tests
echo "[2/4] Building gf_server + test_60hz_physics..."
cmake --build . -j$(nproc)

# 3. Run C++ physics/determinism test
echo "[3/4] Running C++ physics tests..."
cd "$SCRIPT_DIR"
if [ -f "$BUILD_DIR/tests/test_60hz_physics" ]; then
    "$BUILD_DIR/tests/test_60hz_physics"
else
    echo "WARNING: test_60hz_physics binary not found"
fi

# 4. Run Python server integration test
echo "[4/4] Running Python server tick-rate test..."
if [ -f "$SCRIPT_DIR/tests/test_server_tickrate.py" ]; then
    cd "$SCRIPT_DIR/tests"
    python3 test_server_tickrate.py
else
    echo "WARNING: test_server_tickrate.py not found"
fi

echo "========================================"
echo "Build & Test sequence complete"
echo "========================================"
