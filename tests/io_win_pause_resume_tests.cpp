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

TEST(WinIocp, PauseResumeRead) {
    using namespace io;
    std::unique_ptr<INetEngine> engine(create_engine());
    ASSERT_NE(engine, nullptr);

    std::atomic<io::socket_t> acc_fd{io::kInvalidSocket};
    std::atomic<bool> paused_ready{false};
    std::vector<char> recv_accum;
    recv_accum.reserve(1024);

    NetCallbacks cbs{};
    cbs.on_accept = [&](socket_t s) {
        acc_fd.store(s, std::memory_order_relaxed);
        static thread_local char buf[256];
        bool ok = engine->add_socket(s, buf, sizeof(buf), [&](socket_t, char *b, size_t n) {
            recv_accum.insert(recv_accum.end(), b, b + n);
        });
        EXPECT_TRUE(ok);
        EXPECT_TRUE(engine->pause_read(s));
        paused_ready.store(true, std::memory_order_relaxed);
    };
    ASSERT_TRUE(engine->init(cbs));

    SOCKET lfd = INVALID_SOCKET;
    uint16_t port = bind_listen_any(lfd);
    ASSERT_NE(port, 0u);
    ASSERT_TRUE(engine->accept((socket_t)lfd, true, 0));

    std::thread loop([&](){ engine->start(-1); });

    SOCKET cfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(cfd, INVALID_SOCKET);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
    dst.sin_port = htons(port);
    ASSERT_EQ(::connect(cfd, (sockaddr *)&dst, sizeof(dst)), 0);

    // Ensure server applied pause before sending
    auto until = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < until && !paused_ready.load()) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(paused_ready.load());

    const char msg[] = "pause-resume-data";
    ASSERT_GE(::send(cfd, msg, (int)sizeof(msg), 0), 0);
    // Give some time; server must NOT read while paused
    std::this_thread::sleep_for(200ms);
    EXPECT_TRUE(recv_accum.empty());

    // Resume and expect data to be delivered
    io::socket_t s = acc_fd.load();
    ASSERT_NE(s, io::kInvalidSocket);
    ASSERT_TRUE(engine->resume_read(s));

    auto until2 = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < until2 && recv_accum.size() < sizeof(msg)) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(recv_accum.size(), sizeof(msg));

    ::closesocket(cfd);
    engine->stop();
    if (loop.joinable()) loop.join();
    engine->destroy();
    ::closesocket(lfd);
}
#endif