#if defined(_WIN32) || defined(_WIN64)
#include "io/net.hpp"
#include <gtest/gtest.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

static uint16_t bind_listen_any_metrics(SOCKET &lfd) {
    lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lfd == INVALID_SOCKET) return 0;
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    int rc = ::bind(lfd, (sockaddr *)&addr, sizeof(addr));
    if (rc != 0) return 0;
    rc = ::listen(lfd, 16);
    if (rc != 0) return 0;
    sockaddr_in sa{}; int slen = sizeof(sa);
    rc = ::getsockname(lfd, (sockaddr *)&sa, &slen);
    if (rc != 0) return 0;
    return ntohs(sa.sin_port);
}

TEST(WinIocp, MetricsBasicEcho) {
    using namespace io;
    std::unique_ptr<INetEngine> engine(create_engine());
    ASSERT_NE(engine, nullptr);

    std::atomic<bool> got_read{false};

    NetCallbacks cbs{};
    cbs.on_accept = [&](socket_t s) {
        static thread_local char buf[256];
        bool ok = engine->add_socket(s, buf, sizeof(buf), [&](socket_t fd, char *b, size_t n) {
            got_read.store(true, std::memory_order_relaxed);
            engine->write(fd, b, n); // echo back
        });
        EXPECT_TRUE(ok);
    };
    ASSERT_TRUE(engine->init(cbs));

    SOCKET lfd = INVALID_SOCKET;
    uint16_t port = bind_listen_any_metrics(lfd);
    ASSERT_NE(port, 0);
    ASSERT_TRUE(engine->accept((socket_t)lfd, /*async*/ true, /*max*/ 0));

    std::thread loop([&](){ engine->start(-1); });

    // Client connects and sends data
    SOCKET cfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(cfd, INVALID_SOCKET);
    sockaddr_in dst{}; dst.sin_family = AF_INET; inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr); dst.sin_port = htons(port);
    int rc = ::connect(cfd, (sockaddr *)&dst, sizeof(dst));
    ASSERT_EQ(rc, 0);

    const char msg[] = "metrics-echo";
    rc = ::send(cfd, msg, (int)sizeof(msg), 0);
    ASSERT_GE(rc, 0);

    char echo[64]{}; int got = 0; auto until = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < until && got < (int)sizeof(msg)) {
        int r = ::recv(cfd, echo + got, (int)sizeof(echo) - got, 0);
        if (r > 0) got += r; else std::this_thread::sleep_for(2ms);
    }
    EXPECT_EQ(got, (int)sizeof(msg));

    // Give engine a moment to process write completion
    std::this_thread::sleep_for(10ms);

    auto st = engine->get_stats();
    EXPECT_GE(st.accepts_ok, 1u);
    EXPECT_GE(st.reads, 1u);
    EXPECT_GE(st.writes, 1u);
    EXPECT_GE(st.bytes_read, (uint64_t)sizeof(msg));
    EXPECT_GE(st.bytes_written, (uint64_t)sizeof(msg));
    // backlog должен быть 0 к этому моменту
    EXPECT_EQ(st.send_backlog_bytes, 0u);

    ::closesocket(cfd);
    engine->stop();
    if (loop.joinable()) loop.join();
    engine->destroy();
    ::closesocket(lfd);
}
#endif
