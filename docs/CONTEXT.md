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

## Solaris over SSH (remote debug)

Scripts in `scripts/solaris/` help sync/build/test on a remote Solaris host:

- Copy `scripts/solaris/.env.example` to `.env` and adjust:
  - `SOLARIS_SSH=user@host`, `SOLARIS_SSH_PORT=222` (if non-default), optional `SOLARIS_SSH_OPTS` like `-o StrictHostKeyChecking=no`
  - `SOLARIS_DIR=/opt/work/io`, `SOLARIS_BUILD=build/sol`
  - `SOLARIS_EVENTPORTS=ON` to use Event Ports (default), OFF to fallback to /dev/poll

Workflow from project root:

```bash
# 1) push sources
scripts/solaris/solaris_sync.sh

# 2) configure (Debug, tests/examples ON, Event Ports ON)
scripts/solaris/solaris_configure.sh

# 3) build
scripts/solaris/solaris_build.sh

# 4) run tests
scripts/solaris/solaris_test.sh

# 5) run examples (server+client)
scripts/solaris/solaris_run_examples.sh

# 6) debug a binary remotely with gdb
scripts/solaris/solaris_gdb.sh io_tests
```

Notes
- Ensure `cmake`/`ctest`/`gdb` are installed on Solaris. If needed, set absolute paths in `.env`.
- Event Ports backend is enabled with `-DIO_WITH_EVENTPORTS=ON` (default). `/dev/poll` fallback with `OFF`.
- If tests fail, grab failing output and share. For event ports timing issues, also check system logs.

## Solaris timing: event ports, timers, and port_getn behavior

Context
- Initial attempt to use kernel timers with SIGEV_PORT (timer_create/timer_settime) failed with EPERM under non-privileged users on Solaris/Illumos.
- To avoid privilege requirements and flakiness, we implemented software timers integrated with the event-loop via a user pipe.

Implementation
- A dedicated timer thread tracks per-socket deadlines (std::chrono) and wakes at the nearest deadline.
- On expiry, the thread writes a small notification into the event-loop's user pipe to wake the loop; expired FDs are processed in the main loop.
- Data structures: a map of socket -> {timeout_ms, deadline, active}; guarded by a mutex and condition_variable.
- Rearm semantics: deadlines refresh on successful reads; when reads are paused (pause_read), timers are suspended; on resume_read, timers continue and can expire if the remaining time elapses.
- File reference: `src/net_eventports.cpp` (look for `timer_loop`, `timers_`, and user-pipe handling).

port_getn
- We use blocking `port_getn` (passing nullptr timeout). Spurious ETIME returns previously caused flaky timeouts; treating ETIME like EINTR (ignored) stabilized the loop.

Behavior and guarantees
- Per-socket read timeouts: idle sockets close after timeout_ms without reads; any read activity resets the deadline.
- Pause/Resume: while paused, a socket won't be closed by the timeout; resuming re-enables the countdown from the remaining budget.
- User events and accept/read/write readiness continue to work unaffected; listener and user-pipe are re-armed appropriately on each loop.

Trade-offs
- No special privileges required; consistent behavior across systems.
- Minor jitter may occur (sub-few-milliseconds) due to thread wake-ups and scheduling; validated as acceptable by tests.

Validation status (Solaris)
- Full suite passing: 13/13 tests PASS.
- Representative timings: NetTimeout.ReadIdleCloseClient ~113 ms, NetTimeout.DisableTimeout ~612 ms, NetPauseResume.* ~112 ms.
- Run via: `scripts/solaris/solaris_test.sh` (stores logs in `build/sol/ctest_last.log`).

Related scripts
- `scripts/solaris/solaris_sync.sh`, `solaris_configure.sh`, `solaris_build.sh`, `solaris_test.sh` — end-to-end loop for remote Solaris.
