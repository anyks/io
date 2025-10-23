#!/usr/bin/env bash
set -euo pipefail

# Unified stress runner for NetHighload.ManyClientsEchoNoBlock across backends
# Ensures identical stress parameters (IO_STRESS_N, IO_STRESS_M) per backend.
#
# Usage:
#   scripts/stress_matrix.sh [build_type]
#
# Env overrides:
#   IO_STRESS_N (default: 300)    # clients
#   IO_STRESS_M (default: 20000)  # user events
#   PARALLEL (default: 2)         # parallel instances per backend
#   REPEATS (default: 50)         # sequential repeats per backend
#   GENERATOR (default: Ninja)    # CMake generator
#   PRESET (default: debug)       # build dir name under ./build
#
# Notes:
# - On Linux, runs twice: epoll and io_uring (if liburing is available).
# - On macOS/*BSD, only kqueue is available.
# - On Windows, IOCP is always used.
# - On SunOS, runs event ports or devpoll depending on IO_WITH_EVENTPORTS toggle.

BUILD_TYPE="${1:-Debug}"
GENERATOR="${GENERATOR:-Ninja}"
PRESET_DIR="${PRESET:-debug}"
PARALLEL="${PARALLEL:-2}"
REPEATS="${REPEATS:-50}"
export IO_STRESS_N="${IO_STRESS_N:-300}"
export IO_STRESS_M="${IO_STRESS_M:-20000}"

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT_DIR"

log() { echo "[stress-matrix] $*"; }

do_build() {
  local build_dir=$1; shift
  local cmake_args=("-DCMAKE_BUILD_TYPE=${BUILD_TYPE}" "$@")
  log "Configure: ${build_dir} (${cmake_args[*]})"
  cmake -S . -B "${build_dir}" -G "${GENERATOR}" "${cmake_args[@]}"
  log "Build: ${build_dir}"
  cmake --build "${build_dir}" -j"$(sysctl -n hw.logicalcpu 2>/dev/null || nproc || echo 4)"
}

do_stress() {
  local build_dir=$1; shift
  local test_regex=$1; shift || true
  export CTEST_OUTPUT_ON_FAILURE=1
  export CTEST_PARALLEL_LEVEL=1
  local logdir="${build_dir}/stress"
  mkdir -p "${logdir}"
  log "Run parallel=${PARALLEL} in ${build_dir}"
  PARALLEL="${PARALLEL}" BUILD_DIR="${build_dir}" TEST_REGEX="${test_regex}" \
    "${ROOT_DIR}/scripts/stress_local_highload.sh" "${PARALLEL}" 10
  log "Run repeats=${REPEATS} in ${build_dir}"
  ctest --test-dir "${build_dir}" -R "${test_regex}" --repeat until-fail:"${REPEATS}" -j1 --output-on-failure | tee "${logdir}/until_fail_${REPEATS}.log"
}

OS=$(uname -s)
case "${OS}" in
  Linux)
    # 1) epoll
    build_epoll="./build/${PRESET_DIR}-epoll"
    do_build "${build_epoll}" -DIO_WITH_IOURING=OFF
    do_stress "${build_epoll}" '^NetHighload\.ManyClientsEchoNoBlock$'

    # 2) io_uring if available
    if pkg-config --exists liburing || ldconfig -p 2>/dev/null | grep -q liburing || [ -f /usr/include/liburing.h ] || [ -f /usr/local/include/liburing.h ]; then
      build_iouring="./build/${PRESET_DIR}-iouring"
      do_build "${build_iouring}" -DIO_WITH_IOURING=ON
      do_stress "${build_iouring}" '^NetHighload\.ManyClientsEchoNoBlock$'
    else
      log "liburing not found; skipping io_uring backend"
    fi
    ;;
  Darwin|FreeBSD|NetBSD|OpenBSD)
    build_kq="./build/${PRESET_DIR}-kqueue"
    do_build "${build_kq}"
    do_stress "${build_kq}" '^NetHighload\.ManyClientsEchoNoBlock$'
    ;;
  SunOS)
    # Event Ports
    build_evports="./build/${PRESET_DIR}-evports"
    do_build "${build_evports}" -DIO_WITH_EVENTPORTS=ON
    do_stress "${build_evports}" '^NetHighload\.ManyClientsEchoNoBlock$'
    # devpoll
    build_devpoll="./build/${PRESET_DIR}-devpoll"
    do_build "${build_devpoll}" -DIO_WITH_EVENTPORTS=OFF
    do_stress "${build_devpoll}" '^NetHighload\.ManyClientsEchoNoBlock$'
    ;;
  MINGW*|MSYS*|CYGWIN*|Windows_NT)
    build_win="./build/${PRESET_DIR}-iocp"
    do_build "${build_win}"
    # Run two heavy IOCP stress tests with the same IO_STRESS_* env
    do_stress "${build_win}" '^WinIocp\.(HighloadLargePayload|WriteFloodServerToClients)$'
    ;;
  *)
    log "Unsupported or unknown OS: ${OS}"
    exit 1
    ;;
 esac

log "Done"
