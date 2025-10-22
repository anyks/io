# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]
- Docs: added `docs/solaris.md` describing Solaris backends (Event Ports and /dev/poll), unified idle-timeout design, user events, and verbose flags.
- Solaris: refactored Event Ports and /dev/poll backends to a threadless, deadline-driven model:
	- no helper timer threads or extra mutexes in hot paths;
	- nearest per-socket deadline becomes wait timeout (`port_getn`/`DP_POLL`);
	- post‑processing closes expired sockets; pause/resume semantics aligned with epoll/io_uring.
- CI: added optional workflow `.github/workflows/solaris.yml` for self-hosted Solaris runners (build+tests for Event Ports and /dev/poll).
- Validation: full test suite PASS and highload stress PASS on Solaris (Event Ports and /dev/poll). Linux/macOS CI remains green.

## [0.1.0] - 2025-10-21
- Initial public release: cross-platform async IO engine (kqueue/epoll/io_uring/IOCP/eventports/devpoll)
- GoogleTest suite (integration, timeouts, highload), sanitizer presets
- Echo examples (client/server), extended logging for connect completion
