#if defined(_WIN32) || defined(_WIN64)
#include "io/net.hpp"
#include <gtest/gtest.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <mutex>

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
    rc = ::listen(lfd, 512);
    EXPECT_EQ(rc, 0);
    sockaddr_in sa{};
    int slen = sizeof(sa);
    rc = ::getsockname(lfd, (sockaddr *)&sa, &slen);
    EXPECT_EQ(rc, 0);
    return ntohs(sa.sin_port);
}

// Stress the IOCP write path with many clients and large payloads.
// Tunables via env:
//   IO_STRESS_N: number of clients (default 8)
//   IO_STRESS_REPEATS: messages per client (default 50)
//   IO_STRESS_PAYLOAD: payload size in bytes (default 16384)
TEST(WinIocp, HighloadLargePayload) {
    using namespace io;

    int N = 8;
    int REPEATS = 50;
    int PAYLOAD = 16384;
    if (const char *s = std::getenv("IO_STRESS_N")) {
        int v = std::atoi(s); if (v > 0) N = v;
    }
    if (const char *s = std::getenv("IO_STRESS_REPEATS")) {
        int v = std::atoi(s); if (v > 0) REPEATS = v;
    }
    if (const char *s = std::getenv("IO_STRESS_PAYLOAD")) {
        int v = std::atoi(s); if (v > 0) PAYLOAD = v;
    }

    

    std::unique_ptr<INetEngine> engine(create_engine());
    ASSERT_NE(engine, nullptr);

    std::atomic<int> accepted{0};
    std::atomic<uint64_t> total_server_reads{0};
    // Per-connection server buffers to ensure unique stable memory for WSARecv
    std::mutex srv_mtx;
    std::unordered_map<io::socket_t, std::vector<char>> srv_bufs;

    NetCallbacks cbs{};
    cbs.on_accept = [&](socket_t s) {
        accepted.fetch_add(1, std::memory_order_relaxed);
        // allocate unique buffer per accepted socket (64KB)
        {
            std::lock_guard<std::mutex> lk(srv_mtx);
            srv_bufs.emplace(s, std::vector<char>(64 * 1024));
        }
        char *buf_ptr = nullptr;
        size_t buf_sz = 0;
        {
            std::lock_guard<std::mutex> lk(srv_mtx);
            auto it = srv_bufs.find(s);
            if (it != srv_bufs.end()) {
                buf_ptr = it->second.data();
                buf_sz = it->second.size();
            }
        }
        ASSERT_NE(buf_ptr, nullptr);
        bool ok = engine->add_socket(s, buf_ptr, (int)buf_sz, [&](socket_t fd, char *b, size_t n) {
            total_server_reads.fetch_add(n, std::memory_order_relaxed);
            engine->write(fd, b, n); // echo
        });
        EXPECT_TRUE(ok);
    };
    cbs.on_close = [&](socket_t s) {
        std::lock_guard<std::mutex> lk(srv_mtx);
        srv_bufs.erase(s);
    };
    ASSERT_TRUE(engine->init(cbs));

    SOCKET lfd = INVALID_SOCKET;
    uint16_t port = bind_listen_any(lfd);
    ASSERT_NE(port, 0);
    ASSERT_TRUE(engine->accept((socket_t)lfd, /*async*/ true, /*max*/ 0));

    std::thread loop([&]() { engine->start(-1); });

    // Prepare payload
    std::vector<char> payload(PAYLOAD);
    for (int i = 0; i < PAYLOAD; ++i) payload[i] = (char)(i * 131u);

    // Launch N client tasks
    std::vector<std::future<void>> futs;
    futs.reserve(N);
    std::atomic<int> clients_ok{0};

    for (int i = 0; i < N; ++i) {
        futs.emplace_back(std::async(std::launch::async, [&, i]() {
            SOCKET cfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            ASSERT_NE(cfd, INVALID_SOCKET);
            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
            dst.sin_port = htons(port);
            int rc = ::connect(cfd, (sockaddr *)&dst, sizeof(dst));
            if (rc != 0) {
                ::closesocket(cfd);
                GTEST_FAIL() << "client connect failed";
            }
            // switch client socket to non-blocking for pipelined send/recv
            u_long nb = 1;
            ioctlsocket(cfd, FIONBIO, &nb);

            // send REPEATS times; read back full echo
            uint64_t expect = (uint64_t)REPEATS * (uint64_t)PAYLOAD;
            uint64_t recvd = 0;
            std::vector<char> recvbuf; recvbuf.resize(PAYLOAD);
            for (int k = 0; k < REPEATS; ++k) {
                int sent = 0;
                auto soft_until = std::chrono::steady_clock::now() + 5s;
                while (sent < PAYLOAD) {
                    int n = ::send(cfd, payload.data() + sent, PAYLOAD - sent, 0);
                    if (n > 0) {
                        sent += n;
                    } else {
                        int e = WSAGetLastError();
                        if (e != WSAEWOULDBLOCK) std::this_thread::sleep_for(1ms);
                    }
                    // Opportunistic receive while sending to increase overlap
                    for (;;) {
                        int r = ::recv(cfd, recvbuf.data(), (int)recvbuf.size(), 0);
                        if (r > 0) recvd += r; else break;
                    }
                    if (std::chrono::steady_clock::now() > soft_until) soft_until = std::chrono::steady_clock::now() + 5s;
                }
            }
            // Drain remaining echo
            auto until = std::chrono::steady_clock::now() + 10s;
            while (recvd < expect && std::chrono::steady_clock::now() < until) {
                int r = ::recv(cfd, recvbuf.data(), (int)recvbuf.size(), 0);
                if (r > 0) {
                    recvd += r;
                } else {
                    int e = WSAGetLastError();
                    if (e != WSAEWOULDBLOCK) std::this_thread::sleep_for(2ms);
                    else std::this_thread::sleep_for(1ms);
                }
            }
            EXPECT_EQ(recvd, expect);
            ::closesocket(cfd);
            clients_ok.fetch_add(1, std::memory_order_relaxed);
        }));
        std::this_thread::sleep_for(1ms); // stagger connects a little
    }

    for (auto &f : futs) f.get();
    EXPECT_EQ(clients_ok.load(), N);

    engine->stop();
    if (loop.joinable()) loop.join();
    engine->destroy();
    ::closesocket(lfd);
}

// Server write-flood: server pushes large payloads to clients without echoing.
TEST(WinIocp, WriteFloodServerToClients) {
    using namespace io;

    int N = 8; // clients
    int REPEATS = 40; // writes per client
    int PAYLOAD = 16384; // bytes per write
    if (const char *s = std::getenv("IO_STRESS_N")) { int v = std::atoi(s); if (v > 0) N = v; }
    if (const char *s = std::getenv("IO_STRESS_REPEATS")) { int v = std::atoi(s); if (v > 0) REPEATS = v; }
    if (const char *s = std::getenv("IO_STRESS_PAYLOAD")) { int v = std::atoi(s); if (v > 0) PAYLOAD = v; }

    std::unique_ptr<INetEngine> engine(create_engine());
    ASSERT_NE(engine, nullptr);

    std::mutex srv_mtx;
    std::unordered_map<socket_t, std::vector<char>> srv_bufs; // per-conn read buffer (unused but required)
    std::vector<char> payload(PAYLOAD);
    for (int i = 0; i < PAYLOAD; ++i) payload[i] = (char)(i * 17u);

    NetCallbacks cbs{};
    cbs.on_accept = [&](socket_t s) {
        // allocate stable buffer for potential reads; then pause reads
        {
            std::lock_guard<std::mutex> lk(srv_mtx);
            srv_bufs.emplace(s, std::vector<char>(64 * 1024));
        }
        char *buf = nullptr; size_t bsz = 0;
        {
            std::lock_guard<std::mutex> lk(srv_mtx);
            auto it = srv_bufs.find(s);
            buf = it->second.data(); bsz = it->second.size();
        }
        bool ok = engine->add_socket(s, buf, (int)bsz, [&](socket_t, char*, size_t){ /* ignore */ });
        EXPECT_TRUE(ok);
        engine->pause_read(s);
        // push writes immediately (no echo)
        for (int k = 0; k < REPEATS; ++k) {
            engine->write(s, payload.data(), payload.size());
        }
    };
    cbs.on_close = [&](socket_t s) {
        std::lock_guard<std::mutex> lk(srv_mtx);
        srv_bufs.erase(s);
    };
    ASSERT_TRUE(engine->init(cbs));

    // listen
    SOCKET lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(lfd, INVALID_SOCKET);
    int yes = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = 0;
    ASSERT_EQ(::bind(lfd, (sockaddr*)&addr, sizeof(addr)), 0);
    ASSERT_EQ(::listen(lfd, std::max(512, N * 2)), 0);
    int sl = sizeof(addr); ASSERT_EQ(::getsockname(lfd, (sockaddr*)&addr, &sl), 0);
    uint16_t port = ntohs(addr.sin_port);
    ASSERT_TRUE(engine->accept((socket_t)lfd, true, 0));

    std::thread loop([&]{ engine->start(-1); });

    // clients consume writes
    std::atomic<int> ok_clients{0};
    uint64_t expect = (uint64_t)REPEATS * (uint64_t)PAYLOAD;
    std::vector<std::future<void>> futs; futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        futs.emplace_back(std::async(std::launch::async, [&, i]{
            SOCKET cfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            ASSERT_NE(cfd, INVALID_SOCKET);
            sockaddr_in dst{}; dst.sin_family = AF_INET; inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr); dst.sin_port = htons(port);
            ASSERT_EQ(::connect(cfd, (sockaddr*)&dst, sizeof(dst)), 0);
            u_long nb = 1; ioctlsocket(cfd, FIONBIO, &nb);
            std::vector<char> rbuf(64 * 1024); uint64_t got = 0;
            auto until = std::chrono::steady_clock::now() + 30s;
            while (got < expect && std::chrono::steady_clock::now() < until) {
                int r = ::recv(cfd, rbuf.data(), (int)rbuf.size(), 0);
                if (r > 0) got += r; else std::this_thread::sleep_for(1ms);
            }
            EXPECT_EQ(got, expect);
            ::closesocket(cfd);
            ok_clients.fetch_add(1, std::memory_order_relaxed);
        }));
        std::this_thread::sleep_for(1ms);
    }
    for (auto &f : futs) f.get();
    EXPECT_EQ(ok_clients.load(), N);

    engine->stop(); if (loop.joinable()) loop.join(); engine->destroy(); ::closesocket(lfd);
}

// Accept autotune under connection pressure
TEST(WinIocp, AcceptAutotunePressure) {
    using namespace io;
    int N = 32;
    if (const char *s = std::getenv("IO_STRESS_N")) { int v = std::atoi(s); if (v > 0) N = v; }

    std::unique_ptr<INetEngine> engine(create_engine());
    ASSERT_NE(engine, nullptr);

    std::atomic<int> accepted{0};
    NetCallbacks cbs{};
    cbs.on_accept = [&](socket_t s) {
        accepted++;
        static thread_local std::vector<char> buf(4096);
        engine->add_socket(s, buf.data(), (int)buf.size(), [&, s](socket_t fd, char *b, size_t n){ (void)fd; engine->write(s, b, n); });
    };
    ASSERT_TRUE(engine->init(cbs));

    // listen
    SOCKET lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(lfd, INVALID_SOCKET);
    int yes = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = 0;
    ASSERT_EQ(::bind(lfd, (sockaddr*)&addr, sizeof(addr)), 0);
    ASSERT_EQ(::listen(lfd, std::max(512, N * 2)), 0);
    int sl = sizeof(addr); ASSERT_EQ(::getsockname(lfd, (sockaddr*)&addr, &sl), 0);
    uint16_t port = ntohs(addr.sin_port);
    ASSERT_TRUE(engine->accept((socket_t)lfd, true, 0));

    // Configure autotune: fast window, low thresholds to trigger upscale
    io::AcceptAutotuneConfig cfg{};
    cfg.enabled = true; cfg.window_ms = 200; cfg.min_depth = 1; cfg.max_depth = 64; cfg.low_watermark = 1; cfg.high_watermark = 8; cfg.up_step = 8; cfg.down_step = 4; cfg.aggressive_cancel_on_downscale = true;
    engine->set_accept_autotune((socket_t)lfd, cfg);
    engine->set_accept_depth_ex((socket_t)lfd, 1, false);

    std::thread loop([&]{ engine->start(-1); });

    // First wave
    auto connect_wave = [&](int count){
        std::vector<std::future<void>> futs; futs.reserve(count);
        std::atomic<int> done{0};
        for (int i = 0; i < count; ++i) {
            futs.emplace_back(std::async(std::launch::async, [&, i]{
                SOCKET cfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                ASSERT_NE(cfd, INVALID_SOCKET);
                sockaddr_in dst{}; dst.sin_family = AF_INET; inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr); dst.sin_port = htons(port);
                ASSERT_EQ(::connect(cfd, (sockaddr*)&dst, sizeof(dst)), 0);
                const char one = 'x'; ASSERT_GE(::send(cfd, &one, 1, 0), 0);
                char echo[8]{}; auto until = std::chrono::steady_clock::now() + 5s; int got = 0;
                while (got < 1 && std::chrono::steady_clock::now() < until) {
                    int r = ::recv(cfd, echo + got, 1 - got, 0); if (r > 0) got += r; else std::this_thread::sleep_for(1ms);
                }
                EXPECT_EQ(got, 1);
                ::closesocket(cfd);
                done++;
            }));
            std::this_thread::sleep_for(1ms);
        }
        for (auto &f : futs) f.get();
        EXPECT_EQ(done.load(), count);
    };

    connect_wave(N / 2);
    // allow window to roll and autotune to apply
    auto until = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(5ms);
    // Second wave should benefit from larger depth
    connect_wave(N - N / 2);

    engine->stop(); if (loop.joinable()) loop.join(); engine->destroy(); ::closesocket(lfd);
}
#endif
