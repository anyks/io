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

TEST(WinIocp, AsyncConnectClientReceives) {
    using namespace io;
    std::unique_ptr<INetEngine> engine(create_engine());
    ASSERT_NE(engine, nullptr);

    std::atomic<bool> got_user_accept{false};
    std::atomic<bool> got_data{false};

    NetCallbacks cbs{};
    cbs.on_accept = [&](socket_t){ got_user_accept.store(true, std::memory_order_relaxed); };
    ASSERT_TRUE(engine->init(cbs));

    // Prepare server (plain Winsock) that will accept and send payload
    SOCKET lfd = INVALID_SOCKET;
    uint16_t port = bind_listen_any(lfd);
    ASSERT_NE(port, 0u);
    std::thread srv([&](){
        SOCKET s = ::accept(lfd, nullptr, nullptr);
        if (s != INVALID_SOCKET) {
            const char msg[] = "srv->cli";
            ::send(s, msg, (int)sizeof(msg), 0);
            ::closesocket(s);
        }
    });

    // Create client socket managed by engine and connect asynchronously
    SOCKET cfd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    ASSERT_NE(cfd, INVALID_SOCKET);
    static thread_local char buf[256];
    ASSERT_TRUE(engine->add_socket((io::socket_t)cfd, buf, sizeof(buf), [&](io::socket_t, char* b, size_t n){
        if (n > 0) got_data.store(true, std::memory_order_relaxed);
    }));

    std::thread loop([&](){ engine->start(-1); });
    ASSERT_TRUE(engine->connect((io::socket_t)cfd, "127.0.0.1", port, /*async*/ true));

    auto until = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < until && (!got_user_accept.load() || !got_data.load())) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(got_user_accept.load());
    EXPECT_TRUE(got_data.load());

    engine->stop();
    if (loop.joinable()) loop.join();
    engine->destroy();
    if (srv.joinable()) srv.join();
    ::closesocket(lfd);
}
#endif