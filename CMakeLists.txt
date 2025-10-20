cmake_minimum_required(VERSION 3.16)

# Prefer modern timestamp behavior for URL extractions to avoid dev warnings
if (POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# Project
project(io LANGUAGES CXX VERSION 0.1.0)

# Options
option(IO_BUILD_TESTS "Build tests" ON)
option(IO_BUILD_EXAMPLES "Build examples" ON)
option(IO_WITH_IOURING "Use io_uring on Linux" OFF)
option(IO_WITH_EVENTPORTS "Use Event Ports on Solaris" ON)
option(IO_MAX_CONNECTIONS "Max simultaneous connections (0=unlimited)" 0)
option(IO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(IO_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(IO_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Threads (for std::thread on POSIX)
find_package(Threads REQUIRED)

# Warnings and default flags
if (MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Sanitizers (Clang/GCC)
if (IO_ENABLE_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
if (IO_ENABLE_TSAN AND NOT MSVC)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
if (IO_ENABLE_UBSAN AND NOT MSVC)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=undefined -fno-sanitize-recover=undefined)
endif()

# Library target
add_library(io STATIC
  src/io.cpp
  src/net_common.cpp
  src/net_factory.cpp
)

# Platform engines
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR CMAKE_SYSTEM_NAME STREQUAL "NetBSD" OR CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
  target_compile_definitions(io PUBLIC IO_ENGINE_KQUEUE)
  target_sources(io PRIVATE src/net_kqueue.cpp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if (IO_WITH_IOURING)
    # Try to locate liburing. If not found, fall back to epoll engine.
    find_path(URING_INCLUDE_DIR liburing.h)
    find_library(URING_LIBRARY uring)
    if (URING_INCLUDE_DIR AND URING_LIBRARY)
      message(STATUS "Found liburing: ${URING_LIBRARY}")
      target_compile_definitions(io PUBLIC IO_ENGINE_IOURING)
      target_include_directories(io PRIVATE ${URING_INCLUDE_DIR})
      target_link_libraries(io PRIVATE ${URING_LIBRARY})
      target_sources(io PRIVATE src/net_iouring.cpp)
    else()
      message(STATUS "liburing not found; falling back to epoll engine")
      target_compile_definitions(io PUBLIC IO_ENGINE_EPOLL)
      target_sources(io PRIVATE src/net_epoll.cpp)
    endif()
  else()
    target_compile_definitions(io PUBLIC IO_ENGINE_EPOLL)
    target_sources(io PRIVATE src/net_epoll.cpp)
  endif()
elseif (CMAKE_SYSTEM_NAME STREQUAL "Windows")
  target_compile_definitions(io PUBLIC IO_ENGINE_IOCP)
  target_sources(io PRIVATE src/net_iocp.cpp)
  target_link_libraries(io PRIVATE ws2_32 Mswsock)
  # Use a numeric cache variable for accept depth (not a boolean option)
  set(IO_IOCP_ACCEPT_DEPTH 4 CACHE STRING "Concurrent AcceptEx operations depth")
  target_compile_definitions(io PUBLIC IOCP_ACCEPT_DEPTH=${IO_IOCP_ACCEPT_DEPTH})
elseif (CMAKE_SYSTEM_NAME STREQUAL "SunOS")
  if (IO_WITH_EVENTPORTS)
    target_compile_definitions(io PUBLIC IO_ENGINE_EVENTPORTS)
    target_sources(io PRIVATE src/net_eventports.cpp)
  else()
    target_compile_definitions(io PUBLIC IO_ENGINE_DEVPOLL)
    target_sources(io PRIVATE src/net_devpoll.cpp)
  endif()
endif()

# Public headers
target_include_directories(io
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Interface usage requirements
target_compile_features(io PUBLIC cxx_std_17)
target_link_libraries(io PUBLIC Threads::Threads)

# Example application (optional)
if (IO_BUILD_EXAMPLES)
  add_executable(io_example src/main.cpp)
  target_link_libraries(io_example PRIVATE io)

  add_executable(example_server examples/server.cpp)
  target_link_libraries(example_server PRIVATE io)

  add_executable(example_client examples/client.cpp)
  target_link_libraries(example_client PRIVATE io)
  if (MSVC)
    target_link_libraries(io_example PRIVATE ws2_32 Mswsock)
    target_link_libraries(example_server PRIVATE ws2_32 Mswsock)
    target_link_libraries(example_client PRIVATE ws2_32 Mswsock)
  endif()
endif()

# Testing via GoogleTest (FetchContent)
if (IO_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  if (MSVC)
    # On Windows (MSVC), build only portable tests that don't require POSIX sockets
    add_executable(io_tests tests/io_tests.cpp)
    target_link_libraries(io_tests PRIVATE io GTest::gtest_main)
  else()
    add_executable(io_tests tests/io_tests.cpp tests/net_integration_tests.cpp tests/net_highload_tests.cpp tests/net_limits_tests.cpp tests/net_timeout_tests.cpp tests/net_pause_resume_tests.cpp tests/net_timeout_pause_tests.cpp tests/net_pause_resume_server_tests.cpp tests/net_pause_multi_timeout_tests.cpp)
    target_link_libraries(io_tests PRIVATE io GTest::gtest_main)
  endif()
  include(GoogleTest)
  # Ensure tests won't hang forever: set a per-test timeout (can be overridden by CTest args)
  gtest_discover_tests(io_tests)
  set_tests_properties(io_tests PROPERTIES TIMEOUT 120)
endif()

# Install rules
include(GNUInstallDirs)
install(TARGETS io
  EXPORT ioTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Export and package config
install(EXPORT ioTargets
  FILE ioTargets.cmake
  NAMESPACE io::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/io
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ioConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ioConfig.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/ioConfig.cmake @ONLY)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ioConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ioConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/io
)

# CPack skeleton
include(CPack)
set(CPACK_PACKAGE_NAME io)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "TGZ")
