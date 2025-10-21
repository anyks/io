## Workspace Checklist

- [x] Verify that the copilot-instructions.md file in the .github directory is created. (Present)

- [x] Clarify Project Requirements (C++17 event-loop/networking lib with multi-backend, tests, CI/CD, packaging)

- [x] Scaffold the Project (CMake, tests, examples are in place; presets set up)

- [x] Customize the Project (Implement features, fix epoll/io_uring, add timeouts, pause/resume, logging; tests updated)

- [x] Install Required Extensions (No extensions required for this workspace)

- [x] Compile the Project (Debug/ASan/TSan/UBSan builds succeed)

- [x] Create and Run Task (ctest task exists and runs)

- [x] Launch the Project (Examples build; launch on demand via CMake presets)

- [x] Ensure Documentation is Complete (README present; this file cleaned of HTML comments)

Notes:
- Formatting style aligned via .clang-format.
- All tests pass under ASan/TSan/UBSan on macOS.
