#if defined(_WIN32) || defined(_WIN64)
#include "io/net.hpp"
#include <gtest/gtest.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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

TEST(WinIocpIntegration, ServerAcceptsAndReads) {
    using namespace io;
    auto *engine = create_engine();
    ASSERT_NE(engine, nullptr) << "IOCP engine factory returned null";

    std::atomic<bool> accepted{false};
    std::atomic<bool> got_data{false};
    std::vector<char> recv_accum;
    recv_accum.reserve(1024);

    NetCallbacks cbs{};
    cbs.on_accept = [&](socket_t s) {
        accepted.store(true, std::memory_order_relaxed);
        static thread_local char read_buf[512];
        bool ok = engine->add_socket(s, read_buf, sizeof(read_buf), [&](socket_t, char *b, size_t n) {
            recv_accum.insert(recv_accum.end(), b, b + n);
            got_data.store(true, std::memory_order_relaxed);
        });
        EXPECT_TRUE(ok);
    };
    ASSERT_TRUE(engine->init(cbs));

    SOCKET lfd = INVALID_SOCKET;
    uint16_t port = bind_listen_any(lfd);
    ASSERT_NE(port, 0);
    ASSERT_TRUE(engine->accept((socket_t)lfd, /*async*/ true, /*max*/ 0));

    // Run engine in background thread (test uses threads; engine itself is threadless)
    std::atomic<bool> running{true};
    std::thread t([&](){ engine->start(-1); running.store(false); });

    // Client connects and sends data using plain Winsock
    SOCKET cfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(cfd, INVALID_SOCKET);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
    dst.sin_port = htons(port);
    int rc = ::connect(cfd, (sockaddr *)&dst, sizeof(dst));
    ASSERT_EQ(rc, 0);
    const char msg[] = "hello-io-iocp";
    rc = ::send(cfd, msg, (int)sizeof(msg), 0);
    ASSERT_GE(rc, 0);

    // Wait for server to accept and read
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until && (!accepted.load() || !got_data.load())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(accepted.load());
    EXPECT_TRUE(got_data.load());

    ::closesocket(cfd);
    engine->stop();
    if (t.joinable()) t.join();
    engine->destroy();
    ::closesocket(lfd);
}
#endif