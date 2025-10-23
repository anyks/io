#if defined(_WIN32) || defined(_WIN64)
#include "io/net.hpp"
#include <gtest/gtest.h>
#include <winsock2.h>
#include <ws2tcpip.h>

TEST(WinIocpSmoke, InitAddDelete) {
    using namespace io;
    auto *engine = create_engine();
    ASSERT_NE(engine, nullptr) << "IOCP engine factory returned null";

    NetCallbacks cbs{}; // empty callbacks are fine for init
    ASSERT_TRUE(engine->init(cbs));

    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(fd, kInvalidSocket) << "socket() failed";

    static char buf[1024];
    // Register socket with a no-op read callback
    bool ok = engine->add_socket(fd, buf, sizeof(buf), [](socket_t, char *, size_t) {});
    EXPECT_TRUE(ok);

    // Cleanup path should work without throwing
    EXPECT_TRUE(engine->delete_socket(fd));
    ::closesocket((SOCKET)fd);

    engine->destroy();
}
#endif
