#!/usr/bin/env bash
# seaclaw CI script — run locally to simulate CI builds
set -e

cd "$(dirname "$0")"
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo "=== seaclaw CI (local) ==="
echo "Jobs: $NPROC"

echo ""
echo "--- Debug build + test ---"
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_ASAN=ON
make -j"$NPROC"
./seaclaw_tests
cd ..

echo ""
echo "--- Release (MinSizeRel + LTO) build ---"
mkdir -p build-release && cd build-release
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON
make -j"$NPROC"
ls -lh seaclaw
cd ..

echo ""
echo "--- CI complete ---"
