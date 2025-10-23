#if defined(_WIN32) || defined(_WIN64)
#include "io/net.hpp"
#include <gtest/gtest.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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

TEST(WinIocp, AcceptReadWriteEcho) {
    using namespace io;
    std::unique_ptr<INetEngine> engine(create_engine());
    ASSERT_NE(engine, nullptr);

    std::atomic<bool> accepted{false};
    std::atomic<bool> got_read{false};
    std::vector<char> recv_accum;
    recv_accum.reserve(1024);

    NetCallbacks cbs{};
    cbs.on_accept = [&](socket_t s) {
        accepted.store(true, std::memory_order_relaxed);
        static thread_local char buf[512];
        bool ok = engine->add_socket(s, buf, sizeof(buf), [&](socket_t fd, char *b, size_t n) {
            recv_accum.insert(recv_accum.end(), b, b + n);
            got_read.store(true, std::memory_order_relaxed);
            // echo back
            engine->write(fd, b, n);
        });
        EXPECT_TRUE(ok);
    };
    ASSERT_TRUE(engine->init(cbs));

    SOCKET lfd = INVALID_SOCKET;
    uint16_t port = bind_listen_any(lfd);
    ASSERT_NE(port, 0);
    ASSERT_TRUE(engine->accept((socket_t)lfd, /*async*/ true, /*max*/ 0));

    std::thread loop([&](){ engine->start(-1); });

    // Client connects and sends data using plain Winsock, expects echo
    SOCKET cfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(cfd, INVALID_SOCKET);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
    dst.sin_port = htons(port);
    int rc = ::connect(cfd, (sockaddr *)&dst, sizeof(dst));
    ASSERT_EQ(rc, 0);

    const char msg[] = "minGW-echo";
    rc = ::send(cfd, msg, (int)sizeof(msg), 0);
    ASSERT_GE(rc, 0);

    // Read echo back
    char echo[64]{};
    int got = 0;
    auto until = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < until && got < (int)sizeof(msg)) {
        int r = ::recv(cfd, echo + got, (int)sizeof(echo) - got, 0);
        if (r > 0) got += r; else std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(got, (int)sizeof(msg));

    ::closesocket(cfd);
    engine->stop();
    if (loop.joinable()) loop.join();
    engine->destroy();
    ::closesocket(lfd);
}
#endif