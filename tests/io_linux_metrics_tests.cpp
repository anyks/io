#if defined(__linux__)
#include "io/net.hpp"
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>

using namespace std::chrono_literals;

static uint16_t bind_listen_any_metrics(int &lfd) {
    lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return 0;
    int yes = 1;
    (void)::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(lfd, (sockaddr *)&addr, sizeof(addr)) != 0) return 0;
    if (::listen(lfd, 16) != 0) return 0;
    sockaddr_in sa{}; socklen_t slen = sizeof(sa);
    if (::getsockname(lfd, (sockaddr *)&sa, &slen) != 0) return 0;
    return ntohs(sa.sin_port);
}

TEST(LinuxEngine, MetricsBasicEcho) {
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

    int lfd = -1;
    uint16_t port = bind_listen_any_metrics(lfd);
    ASSERT_NE(port, 0);
    ASSERT_TRUE(engine->accept((socket_t)lfd, /*async*/ true, /*max*/ 0));

    std::thread loop([&](){ engine->start(-1); });

    // Client connects and sends data
    int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(cfd, 0);
    sockaddr_in dst{}; dst.sin_family = AF_INET; inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr); dst.sin_port = htons(port);
    int rc = ::connect(cfd, (sockaddr *)&dst, sizeof(dst));
    ASSERT_EQ(rc, 0);

    const char msg[] = "metrics-echo";
    rc = (int)::send(cfd, msg, sizeof(msg), 0);
    ASSERT_GE(rc, 0);

    char echo[64]{}; int got = 0; auto until = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < until && got < (int)sizeof(msg)) {
        int r = (int)::recv(cfd, echo + got, (int)sizeof(echo) - got, 0);
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
    EXPECT_EQ(st.send_backlog_bytes, 0u);

    ::close(cfd);
    engine->stop();
    if (loop.joinable()) loop.join();
    engine->destroy();
    ::close(lfd);
}
#endif
