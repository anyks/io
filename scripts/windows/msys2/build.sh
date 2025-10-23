#!/usr/bin/env bash
set -euo pipefail

# One-click MSYS2 MinGW64 build of io using bash (SSH-friendly)
# Usage: ./scripts/windows/msys2/build.sh [Debug|Release]  (default: Debug)

# Load env if present
ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
ENV_FILE="$ROOT_DIR/scripts/windows/.env"
if [[ -f "$ENV_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$ENV_FILE"
fi

CONFIG=${1:-${CONFIG:-Debug}}
BUILD_WIN=${BUILD_WIN:-/e/io}
BUILD_TIMEOUT=${BUILD_TIMEOUT:-900} # seconds (15 min default)
GEN="Ninja"

if ! command -v cmake >/dev/null 2>&1; then
  echo "[ERROR] cmake not found in PATH. In MSYS2 bash, install: pacman -S --needed mingw-w64-x86_64-cmake" >&2
  exit 1
fi

if ! command -v gcc >/dev/null 2>&1; then
  echo "[ERROR] gcc not found. In MSYS2 bash, install toolchain: pacman -S --needed mingw-w64-x86_64-toolchain" >&2
  exit 1
fi

if ! command -v ninja >/dev/null 2>&1; then
  GEN="MinGW Makefiles"
fi

BUILD_DIR="${BUILD_WIN}/build/mingw64-${CONFIG}"
mkdir -p "$BUILD_DIR"

echo "[io][build] CONFIG=$CONFIG"
echo "[io][build] BUILD_WIN=$BUILD_WIN"
printf "[io][build] Generator: %s\n" "$GEN"

echo "[io][build] CMake configure (timeout: ${BUILD_TIMEOUT}s)..."
if command -v timeout >/dev/null 2>&1; then
  timeout -k 10s "${BUILD_TIMEOUT}s" \
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G "$GEN" \
      -DCMAKE_BUILD_TYPE="${CONFIG}" \
      -DIO_BUILD_TESTS=ON -DIO_BUILD_EXAMPLES=ON || {
        code=$?
        if [[ $code -eq 124 ]]; then
          echo "[io][build] Configure timed out after ${BUILD_TIMEOUT}s" >&2
        fi
        exit $code
      }
else
  echo "[io][build] WARN: coreutils timeout not found; running without a hard limit" >&2
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G "$GEN" \
    -DCMAKE_BUILD_TYPE="${CONFIG}" \
    -DIO_BUILD_TESTS=ON -DIO_BUILD_EXAMPLES=ON
fi

echo "[io][build] Build (timeout: ${BUILD_TIMEOUT}s)..."
if command -v timeout >/dev/null 2>&1; then
  timeout -k 10s "${BUILD_TIMEOUT}s" cmake --build "$BUILD_DIR" -j || {
    code=$?
    if [[ $code -eq 124 ]]; then
      echo "[io][build] Build timed out after ${BUILD_TIMEOUT}s" >&2
    fi
    exit $code
  }
else
  cmake --build "$BUILD_DIR" -j
fi

echo "[io][build] Done. Outputs in: $BUILD_DIR"
