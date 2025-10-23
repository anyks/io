#if defined(_WIN32) || defined(_WIN64)
#include "io/net.hpp"
#include <gtest/gtest.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

static uint16_t bind_listen_any(SOCKET &lfd) {
    lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    EXPECT_NE(lfd, INVALID_SOCKET);
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    int rc = ::bind(lfd, (sockaddr *)&addr, sizeof(addr));
    EXPECT_EQ(rc, 0);
    rc = ::listen(lfd, 16);
    EXPECT_EQ(rc, 0);
    sockaddr_in sa{};
    int slen = sizeof(sa);
    rc = ::getsockname(lfd, (sockaddr *)&sa, &slen);
    EXPECT_EQ(rc, 0);
    return ntohs(sa.sin_port);
}

TEST(WinIocp, ReadTimeoutCloses) {
    using namespace io;
    std::unique_ptr<INetEngine> engine(create_engine());
    ASSERT_NE(engine, nullptr);

    std::atomic<bool> closed{false};
    NetCallbacks cbs{};
    cbs.on_accept = [&](socket_t s) {
        static thread_local char buf[128];
        ASSERT_TRUE(engine->add_socket(s, buf, sizeof(buf), [&](socket_t, char*, size_t){}));
        ASSERT_TRUE(engine->set_read_timeout(s, 200));
    };
    cbs.on_close = [&](socket_t){ closed.store(true, std::memory_order_relaxed); };
    ASSERT_TRUE(engine->init(cbs));

    SOCKET lfd = INVALID_SOCKET;
    uint16_t port = bind_listen_any(lfd);
    ASSERT_NE(port, 0);
    ASSERT_TRUE(engine->accept((socket_t)lfd, true, 0));

    std::thread loop([&](){ engine->start(-1); });

    SOCKET cfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(cfd, INVALID_SOCKET);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
    dst.sin_port = htons(port);
    ASSERT_EQ(::connect(cfd, (sockaddr *)&dst, sizeof(dst)), 0);
    // Don't send anything; expect server to close by timeout
    std::this_thread::sleep_for(500ms);

    engine->stop();
    if (loop.joinable()) loop.join();
    engine->destroy();
    ::closesocket(cfd);
    ::closesocket(lfd);

    ASSERT_TRUE(closed.load());
}
#endif