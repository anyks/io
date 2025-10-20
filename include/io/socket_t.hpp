#pragma once
#include <cstdint>

namespace io {
#if defined(_WIN32) || defined(_WIN64)
#  include <winsock2.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int32_t;
constexpr socket_t kInvalidSocket = static_cast<socket_t>(-1);
#endif
} // namespace io
