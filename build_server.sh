#!/bin/bash
set -e

echo "Building DZFoot Game Server (Linux)..."
cd server/build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo "Binary: server/build/gf_server"
