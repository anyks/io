# IO — portable project context

Date: 2025-10-21
Branch: main

This document captures the working context so the assistant on another machine (e.g., Linux) can load the essentials without re-deriving history. It summarizes architecture, recent changes, build/test steps, style, and next actions.

## Overview

- Language/Tooling: C++17, CMake, GoogleTest, CPack
- Library: cross-platform event loop + networking with multiple backends
  - Linux: epoll, io_uring (optional, via liburing)
  - macOS/BSD: kqueue
  - Windows: IOCP
  - Solaris/Illumos: event ports, devpoll
- Features
  - async accept/connect/read/write
  - post(uint32_t) user events
  - per-socket read timeouts (close on idle)
  - pause_read / resume_read
  - logging hooks
- Structure
  - `include/io/*.hpp`, `src/net_*.cpp`, `examples/`, `tests/`
  - CMakePresets for debug/release/asan/tsan/ubsan

## Recent changes (macOS validated)

- Formatting: unified via `.clang-format` (tabs, K&R attach braces, width 120). Repo-wide autoformat applied.
- Tests migrated to `io::socket_t` and `io::kInvalidSocket` for portability (Windows-friendly).
- io_uring hardening: guarded SQE submissions (ring mutex), safer accept path, avoid pre-connect recv; extra diagnostics.
- epoll: tolerate EPOLL_CTL_ADD EEXIST by falling back to MOD; improve connect success path.
- CMake: set global CTest timeout to prevent hangs; policy updates; liburing optional.
- All tests pass on macOS under Debug/ASan/TSan/UBSan. Highload TSan test repeated 100× — PASS.

## Build & test (Linux)

Dependencies (Debian/Ubuntu example):

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
  clang clang-format llvm \
  liburing-dev \
  libasan5 libubsan1 libtsan0 || true
```

Configure and build:

```bash
# Debug
cmake --preset debug
cmake --build --preset debug -j$(nproc)

# Sanitizers
cmake --preset asan && cmake --build --preset asan -j$(nproc)
cmake --preset tsan && cmake --build --preset tsan -j$(nproc)
cmake --preset ubsan && cmake --build --preset ubsan -j$(nproc)
```

Run tests:

```bash
ctest --test-dir ./build/debug -j4 --output-on-failure
ctest --test-dir ./build/asan  -j4 --output-on-failure
ctest --test-dir ./build/tsan  -j4 --output-on-failure
ctest --test-dir ./build/ubsan -j4 --output-on-failure

# Stress
ctest --test-dir ./build/tsan -R NetHighload.ManyClientsEchoNoBlock -j1 --repeat until-fail:100 --output-on-failure
```

io_uring notes:

- Requires modern kernel (5.x+). If liburing or kernel features are unavailable, build falls back to epoll (option-controlled).
- If hangs occur, capture `ctest` output and `dmesg` snippets for diagnostics.

## Style

- `.clang-format` (excerpt):
  - `UseTab: Always`, `IndentWidth: 4`, `TabWidth: 4`, `BreakBeforeBraces: Attach`, `ColumnLimit: 120`, `SortIncludes: true`, `AllowShortFunctionsOnASingleLine: Empty`
- `.editorconfig`: spaces=2 are set editor-wide, but clang-format governs C/C++ code (tabs). Editors should defer to clang-format.

## Key files

- API: `include/io/net.hpp`, `include/io/socket_t.hpp`, `include/io/io.hpp`
- Backends: `src/net_epoll.cpp`, `src/net_iouring.cpp`, `src/net_kqueue.cpp`, `src/net_iocp.cpp`, `src/net_eventports.cpp`, `src/net_devpoll.cpp`
- Factory: `src/net_factory.cpp`
- Tests: `tests/*.cpp` including highload, integration, timeouts, pause/resume
- Examples: `examples/client.cpp`, `examples/server.cpp`

## Next steps (Linux focus)

1) Validate io_uring on Linux with current hardening.
2) If issues: collect logs and add targeted fixes (SQE pressure, timeout bookkeeping, connect race edges).
3) Optionally add a CI style check (clang-format) and release packaging job.
4) Windows cross-check (IOCP) once Linux is stable.

## How to share results back

Share:
- `ctest` outputs (`--output-on-failure`), failing test names
- `build/*/CMakeFiles/CMakeError.log` if configuration fails
- `dmesg` excerpts for io_uring-related errors

This CONTEXT.md is designed to be read by the assistant to quickly regain full context on another machine.
