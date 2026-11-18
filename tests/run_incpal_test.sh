#!/bin/sh
# One-shot acceptance run for the incremental palette codec.
# No network, no third-party libraries; uses only the system C/C++ toolchain.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/tests/build"
mkdir -p "$BUILD"
trap 'rm -rf "$BUILD"' EXIT

echo "== Compiling tests (C99 and C++11, -Wall -Wextra) =="
cc -std=c99 -Wall -Wextra -I"$ROOT/src" \
   "$ROOT/tests/incpal_test.c" -o "$BUILD/incpal_test_c"
c++ -std=c++11 -Wall -Wextra -I"$ROOT/src" \
    "$ROOT/tests/incpal_test.c" -o "$BUILD/incpal_test_cpp"

echo "== Regression: existing library translation unit still builds =="
c++ -Wall -Wextra -D__LINUX__ -I"$ROOT/src" -c \
    "$ROOT/src/AnimatedGIF.cpp" -o "$BUILD/AnimatedGIF.o"

echo
echo "########## C build run ##########"
"$BUILD/incpal_test_c"
echo
echo "########## C++ build run ##########"
"$BUILD/incpal_test_cpp"
