#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
echo "=== uname ==="
uname -a
echo "=== g++ ==="
g++ --version | head -n 1
echo "=== cmake ==="
cmake --version | head -n 1
echo "=== make ==="
make --version | head -n 1
echo "=== project ==="
ls -la
echo "=== configure ==="
cmake -S . -B build-linux-baseline -DCMAKE_BUILD_TYPE=Debug
echo "=== build ==="
cmake --build build-linux-baseline -j
echo "=== ctest ==="
ctest --test-dir build-linux-baseline --output-on-failure
echo "BASELINE_CTEST_DONE"
