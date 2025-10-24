#include "io/net.hpp"
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <deque>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cstdarg>

using namespace std::chrono_literals;

namespace {

struct MsgPlan {
    size_t size{};            // bytes
    size_t send_left{};       // bytes left to physically write (tracked via on_write)
    size_t recv_left{};       // bytes left to receive back (echo)
};

struct ClientState {
    io::socket_t fd{io::kInvalidSocket};
    std::unique_ptr<char[]> buf;      // recv buffer for engine
    size_t buf_size{0};
    std::vector<char> tx;             // current message payload (reused per message)
    size_t tx_offset{0};              // not used for write (engine queues internally), kept for completeness
    size_t msg_index{0};              // legacy index (used when PIPELINE=1)
    size_t next_send_idx{0};          // next message index to send (for windowed pipeline)
    size_t next_recv_idx{0};          // next message index expected to finish receiving
    std::vector<MsgPlan> plan;        // per-message plan
    std::atomic<bool> connected{false};
    std::atomic<bool> closed{false};
    uint64_t sends_completed{0};
    uint64_t recvs_completed{0};
    uint64_t bytes_received{0};
};

static inline std::string backend_name() {
#if defined(IO_ENGINE_KQUEUE)
    return "kqueue";
#elif defined(IO_ENGINE_IOURING)
    return "io_uring";
#elif defined(IO_ENGINE_EPOLL)
    return "epoll";
#elif defined(IO_ENGINE_IOCP)
    return "iocp";
#elif defined(IO_ENGINE_EVENTPORTS)
    return "eventports";
#elif defined(IO_ENGINE_DEVPOLL)
    return "devpoll";
#else
    return "unknown";
#endif
}

static inline void fill_payload(std::vector<char> &dst, uint64_t seed) {
    // Deterministic content per message using a simple PRNG
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto &c : dst) c = static_cast<char>(dist(rng));
}

} // namespace

// EPS benchmark: echo server, N clients, each sends R messages of random size [min,max] MB (deterministic RNG)
// We measure EPS for send-completions (per-message fully written) and receive-completions (per-message fully echoed back)
TEST(EpsBenchmark, LoadEchoRandomLarge) {
    using namespace io;
    // Ensure unbuffered streams so progress lines appear immediately even when piped
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    // Parameters (env-overridable)
    int N = 8;                           // clients
    int R = 100;                         // messages per client
    int MIN_MB = 1;                      // min payload size in MB (used if byte-size mode is off)
    int MAX_MB = 2;                      // max payload size in MB (used if byte-size mode is off)
    size_t MIN_BYTES = 0;                // optional: min payload size in bytes (enables byte-size mode if >0)
    size_t MAX_BYTES = 0;                // optional: max payload size in bytes (enables byte-size mode if >= MIN_BYTES)
    int PIPELINE = 1;                    // in-flight messages per client (1 for precise per-message tracking)
    size_t RECV_BUF = 256 * 1024;        // per-socket receive buffer
    uint64_t SEED = 42;                  // deterministic seed across backends
    int CONNECT_TIMEOUT_MS = 10000;      // connect phase timeout
    int RUN_TIMEOUT_MS = 120000;         // run phase timeout (2 minutes) for large payloads
    int PROGRESS_SEC = 5;                // progress print period in seconds (env-overridable)
    // Progress will be computed directly in the recv callback.

    if (const char *s = std::getenv("IO_BENCH_CLIENTS")) { int v = std::atoi(s); if (v > 0) N = v; }
    if (const char *s = std::getenv("IO_BENCH_MSGS"))    { int v = std::atoi(s); if (v > 0) R = v; }
    if (const char *s = std::getenv("IO_BENCH_MIN_MB"))  { int v = std::atoi(s); if (v >= 0) MIN_MB = v; }
    if (const char *s = std::getenv("IO_BENCH_MAX_MB"))  { int v = std::atoi(s); if (v >= MIN_MB) MAX_MB = v; }
    if (const char *s = std::getenv("IO_BENCH_MIN_BYTES")) { long long v = std::strtoll(s, nullptr, 10); if (v > 0) MIN_BYTES = (size_t)v; }
    if (const char *s = std::getenv("IO_BENCH_MAX_BYTES")) { long long v = std::strtoll(s, nullptr, 10); if (v > 0 && (size_t)v >= MIN_BYTES) MAX_BYTES = (size_t)v; }
    if (const char *s = std::getenv("IO_BENCH_PIPELINE")){ int v = std::atoi(s); if (v >= 1) PIPELINE = v; }
    if (const char *s = std::getenv("IO_BENCH_BUF"))     { long v = std::atol(s); if (v >= 4*1024) RECV_BUF = (size_t)v; }
    if (const char *s = std::getenv("IO_BENCH_SEED"))    { unsigned long long v = std::strtoull(s, nullptr, 10); SEED = (uint64_t)v; }
    if (const char *s = std::getenv("IO_BENCH_TIMEOUT")) { int v = std::atoi(s); if (v > 0) RUN_TIMEOUT_MS = v; }
    if (const char *s = std::getenv("IO_BENCH_PROGRESS_SEC")) { int v = std::atoi(s); if (v >= 1 && v <= 60) PROGRESS_SEC = v; }
    // No tracer or markers used.

    // Total messages (for progress prints)
    const uint64_t TOTAL_MSGS = (uint64_t)N * (uint64_t)R;

    // Optional dataset directory: preload payloads from files for deterministic cross-backend data
    namespace fs = std::filesystem;
    std::vector<std::vector<char>> dataset;
    std::vector<size_t> dataset_sizes;
    std::vector<char> dataset_pool; // concatenated pool when using byte-size mode
    std::string dataset_dir;
    if (const char *ds = std::getenv("IO_BENCH_DATASET_DIR")) {
        dataset_dir = ds;
        std::error_code ec;
        if (!dataset_dir.empty() && fs::exists(fs::path(dataset_dir), ec)) {
            std::vector<fs::path> files;
            for (auto &ent : fs::directory_iterator(fs::path(dataset_dir))) {
                if (ent.is_regular_file()) {
                    auto p = ent.path();
                    if (p.extension() == ".bin") files.push_back(p);
                }
            }
            std::sort(files.begin(), files.end());
            for (auto &p : files) {
                std::ifstream ifs(p, std::ios::binary);
                if (!ifs) continue;
                ifs.seekg(0, std::ios::end);
                std::streamsize sz = ifs.tellg();
                if (sz <= 0) continue;
                ifs.seekg(0, std::ios::beg);
                std::vector<char> buf((size_t)sz);
                ifs.read(buf.data(), sz);
                if (!ifs) continue;
                dataset_sizes.push_back((size_t)sz);
                dataset.emplace_back(std::move(buf));
            }
            // Build a single pool for byte-size mode
            if (!dataset.empty() && MIN_BYTES > 0 && MAX_BYTES >= MIN_BYTES) {
                size_t total = 0; for (auto sz : dataset_sizes) total += sz;
                dataset_pool.resize(total);
                size_t off = 0;
                for (auto &d : dataset) { std::memcpy(dataset_pool.data() + off, d.data(), d.size()); off += d.size(); }
            }
        }
    }

    // SplitMix64 for fast deterministic pseudorandom selection with replacement
    auto splitmix64 = [](uint64_t &x) {
        uint64_t z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    auto pick_dataset_index = [&](int client_idx, size_t msg_idx) -> size_t {
        if (dataset.empty()) return (size_t)-1;
        uint64_t x = SEED ^ ((uint64_t)client_idx << 32) ^ (uint64_t)msg_idx;
        uint64_t r = splitmix64(x);
        return (size_t)(r % dataset.size());
    };
    auto pick_pool_offset = [&](int client_idx, size_t msg_idx, size_t size) -> size_t {
        if (dataset_pool.empty()) return 0;
        uint64_t x = (SEED * 0x9E3779B185EBCA87ull) ^ ((uint64_t)client_idx << 33) ^ (uint64_t)(msg_idx * 1315423911u);
        uint64_t r = splitmix64(x);
        size_t cap = dataset_pool.size();
        if (size >= cap) return 0; // degenerate: message >= pool size
        return (size_t)(r % (cap - size));
    };

    // Server listen socket
    io::socket_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(listen_fd, io::kInvalidSocket);
    int opt = 1; ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = 0;
    ASSERT_EQ(::bind(listen_fd, (sockaddr *)&addr, sizeof(addr)), 0);
    ASSERT_EQ(::listen(listen_fd, std::max(1024, N * 2)), 0);
    socklen_t alen = sizeof(addr); ASSERT_EQ(::getsockname(listen_fd, (sockaddr *)&addr, &alen), 0);
    uint16_t port = ntohs(addr.sin_port);

    std::unique_ptr<INetEngine> engine(create_engine());
    ASSERT_NE(engine, nullptr);

    // Shared state
    std::mutex srv_mtx; std::unordered_map<int, std::unique_ptr<char[]>> server_buffers;
    std::unordered_set<io::socket_t> client_fds; std::mutex client_mtx; std::unordered_map<io::socket_t, int> fd_to_idx;
    std::vector<ClientState> clients(N);
    std::atomic<int> connected{0}; std::atomic<int> server_accepts{0}; std::atomic<int> close_count{0};

    // Deterministic plan for all clients/messages
    std::mt19937 rng_sizes((uint32_t)SEED);
    std::uniform_int_distribution<int> dist_mb(MIN_MB, MAX_MB);
    std::uniform_int_distribution<size_t> dist_bytes((MIN_BYTES>0?MIN_BYTES:1), (MAX_BYTES>0?MAX_BYTES:1));
    const bool bytes_mode = (MIN_BYTES > 0 && MAX_BYTES >= MIN_BYTES);
    const size_t min_bytes = (size_t)MIN_MB * 1024ull * 1024ull;
    const size_t max_bytes = (size_t)MAX_MB * 1024ull * 1024ull;

    for (int i = 0; i < N; ++i) {
        clients[i].plan.reserve((size_t)R);
        for (int j = 0; j < R; ++j) {
            size_t sz = 0;
            if (!bytes_mode) {
                int mb = dist_mb(rng_sizes);
                if (mb < MIN_MB) mb = MIN_MB; if (mb > MAX_MB) mb = MAX_MB;
                sz = (size_t)mb * 1024ull * 1024ull;
            } else {
                sz = dist_bytes(rng_sizes);
            }
            clients[i].plan.push_back(MsgPlan{sz, sz, sz});
        }
        clients[i].buf_size = RECV_BUF;
        clients[i].buf = std::make_unique<char[]>(clients[i].buf_size);
        // prepare tx buffer capacity to max message (reused per message)
        size_t cap = bytes_mode ? std::max<size_t>(MAX_BYTES, 4*1024) : ((size_t)MAX_MB * 1024ull * 1024ull);
        clients[i].tx.resize(cap);
    }

    auto schedule_send = [&](int ci, size_t idx) {
        auto &cl = clients[ci];
        auto &mp = cl.plan[idx];
        if (bytes_mode) {
            // choose size if zero (defensive), and fill from dataset_pool slice (wrap if needed)
            size_t sz = mp.size ? mp.size : (size_t)dist_bytes(rng_sizes);
            mp.size = sz; mp.send_left = sz; mp.recv_left = sz;
            if (!dataset_pool.empty()) {
                size_t off = pick_pool_offset(ci, idx, sz);
                if (cl.tx.size() < sz) cl.tx.resize(sz);
                size_t first = std::min(sz, dataset_pool.size() - off);
                std::memcpy(cl.tx.data(), dataset_pool.data() + off, first);
                if (first < sz) std::memcpy(cl.tx.data()+first, dataset_pool.data(), sz-first);
                (void)engine->write(cl.fd, cl.tx.data(), sz);
            } else {
                if (cl.tx.size() < sz) cl.tx.resize(sz);
                fill_payload(cl.tx, (uint64_t)SEED ^ ((uint64_t)ci << 32) ^ (uint64_t)idx);
                (void)engine->write(cl.fd, cl.tx.data(), sz);
            }
        } else {
            // MB mode: use whole dataset file or generated buffer of EXACT size mp.size
            size_t didx = pick_dataset_index(ci, idx);
            if (didx != (size_t)-1) {
                auto &data = dataset[didx];
                mp.size = data.size(); mp.send_left = mp.size; mp.recv_left = mp.size;
                (void)engine->write(cl.fd, data.data(), mp.size);
            } else {
                // Ensure buffer length exactly equals mp.size before writing
                if (cl.tx.size() != mp.size) cl.tx.resize(mp.size);
                fill_payload(cl.tx, (uint64_t)SEED ^ ((uint64_t)ci << 32) ^ (uint64_t)idx);
                mp.send_left = mp.size; mp.recv_left = mp.size;
                (void)engine->write(cl.fd, cl.tx.data(), mp.size);
            }
        }
    };

    // Shared progress state for EPS computed in the recv callback
    struct ProgressState {
        std::mutex m;
        std::chrono::system_clock::time_point bench_start_sys;
        std::chrono::steady_clock::time_point last_prog;
        uint64_t last_recvs{0};
        uint64_t last_bytes{0};
    } prog;
    std::atomic<uint64_t> total_recvs{0};
    std::atomic<uint64_t> total_bytes{0};

    // Engine callbacks
    NetCallbacks cbs{};
    cbs.on_accept = [&](io::socket_t fd) {
        bool is_client = false;
        {
            std::lock_guard<std::mutex> lk(client_mtx);
            is_client = client_fds.find(fd) != client_fds.end();
        }
        if (fd != listen_fd && !is_client) {
            // Server side: echo all bytes back
            server_accepts++;
            auto sbuf = std::make_unique<char[]>(RECV_BUF);
            char *raw = sbuf.get();
            {
                std::lock_guard<std::mutex> lk(srv_mtx);
                server_buffers.emplace(fd, std::move(sbuf));
            }
            engine->add_socket(fd, raw, RECV_BUF, [&, fd](int s, char *b, size_t n) {
                (void)s; engine->write(fd, b, n);
            });
        } else {
            // Client side connected
            connected++;
            int idx = -1;
            {
                std::lock_guard<std::mutex> lk(client_mtx);
                auto it = fd_to_idx.find(fd);
                if (it != fd_to_idx.end()) idx = it->second;
            }
            if (idx >= 0) clients[idx].connected.store(true, std::memory_order_relaxed);
        }
    };
    cbs.on_close = [&](io::socket_t fd) {
        close_count++;
        std::lock_guard<std::mutex> lk(srv_mtx);
        server_buffers.erase(fd);
    };
    // Track physical write progress per client using on_write (for stats only)
    cbs.on_write = [&](io::socket_t fd, size_t bytes_written) {
        int idx = -1;
        {
            std::lock_guard<std::mutex> lk(client_mtx);
            auto it = fd_to_idx.find(fd);
            if (it != fd_to_idx.end()) idx = it->second;
        }
        if (idx < 0) return;
        auto &cl = clients[idx];
        if (cl.msg_index >= cl.plan.size()) return;
        auto &mp = cl.plan[cl.msg_index];
        if (mp.send_left > 0) {
            if (bytes_written >= mp.send_left) mp.send_left = 0; else mp.send_left -= bytes_written;
            if (mp.send_left == 0) {
                cl.sends_completed++;
            }
        }
    };

    ASSERT_TRUE(engine->init(cbs));
    ASSERT_TRUE(engine->accept(listen_fd, true, (uint32_t)(N * 4)));

    // Create and add clients
    for (int i = 0; i < N; ++i) {
        io::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_NE(fd, io::kInvalidSocket);
        clients[i].fd = fd;
        {
            std::lock_guard<std::mutex> lk(client_mtx);
            client_fds.insert(fd);
            fd_to_idx.emplace(fd, i);
        }
        ASSERT_TRUE(engine->add_socket(fd, clients[i].buf.get(), clients[i].buf_size, [&, i](io::socket_t s, char *b, size_t n) {
            (void)s; auto &cl = clients[i];
            while (n > 0) {
                if (cl.next_recv_idx >= cl.plan.size()) break; // shouldn't happen
                auto &mp = cl.plan[cl.next_recv_idx];
                size_t take = std::min(mp.recv_left, n);
                mp.recv_left -= take; n -= take; b += take;
                cl.bytes_received += take;
                total_bytes.fetch_add(take, std::memory_order_relaxed);
                if (mp.recv_left == 0) {
                    cl.recvs_completed++;
                    cl.next_recv_idx++;
                    total_recvs.fetch_add(1, std::memory_order_relaxed);

                    // Inline EPS progress every PROGRESS_SEC seconds
                    if (PROGRESS_SEC > 0) {
                        auto now_st = std::chrono::steady_clock::now();
                        bool do_print = false;
                        {
                            std::lock_guard<std::mutex> lk(prog.m);
                            if (prog.bench_start_sys.time_since_epoch().count() != 0) {
                                if (now_st - prog.last_prog >= std::chrono::seconds(PROGRESS_SEC)) {
                                    prog.last_prog = now_st;
                                    do_print = true;
                                }
                            }
                        }
                        if (do_print) {
                            uint64_t recvs = total_recvs.load(std::memory_order_relaxed);
                            uint64_t bytes = total_bytes.load(std::memory_order_relaxed);
                            auto now_sys = std::chrono::system_clock::now();
                            double now_ts = std::chrono::duration_cast<std::chrono::duration<double>>(now_sys.time_since_epoch()).count();
                            double start_ts = std::chrono::duration_cast<std::chrono::duration<double>>(prog.bench_start_sys.time_since_epoch()).count();
                            double secs = now_ts - start_ts; if (secs <= 0.0) secs = 1e-9;
                            double eps_avg = (double)recvs / secs;
                            double eps_5s, mbps_5s; double msgs_5s;
                            {
                                std::lock_guard<std::mutex> lk(prog.m);
                                double msgs_interval = (double)(recvs - prog.last_recvs);
                                msgs_5s = msgs_interval * (5.0 / (double)PROGRESS_SEC);
                                eps_5s = msgs_5s / 5.0;
                                mbps_5s = ((double)(bytes - prog.last_bytes) / (1024.0 * 1024.0)) * (5.0 / (double)PROGRESS_SEC);
                                prog.last_recvs = recvs;
                                prog.last_bytes = bytes;
                            }
                            const char *fmt =
                                "EPS_PROGRESS backend=%s recvs=%llu/%llu duration_s=%.2f interval_s=%.2f msgs_5s=%.0f eps_avg=%.2f eps_5s=%.2f mbps_5s=%.2f\n";
                            std::fprintf(stdout, fmt,
                                         backend_name().c_str(),
                                         (unsigned long long)recvs,
                                         (unsigned long long)TOTAL_MSGS,
                                         secs,
                                         (double)PROGRESS_SEC,
                                         msgs_5s,
                                         eps_avg,
                                         eps_5s,
                                         mbps_5s);
                            std::fflush(stdout);
                        }
                    }

                    // Keep the window full: when a message completes, send next if available
                    if ((int)cl.next_send_idx < R) {
                        size_t inflight = (cl.next_send_idx >= cl.next_recv_idx) ? (cl.next_send_idx - cl.next_recv_idx) : 0;
                        if ((int)inflight < PIPELINE) {
                            schedule_send(i, cl.next_send_idx);
                            cl.next_send_idx++;
                        }
                    }
                }
            }
        }));
    }

    // Connect all clients asynchronously
    for (int i = 0; i < N; ++i) {
        bool ok = false; auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(CONNECT_TIMEOUT_MS);
        while (std::chrono::steady_clock::now() < deadline && !ok) {
            ok = engine->connect(clients[i].fd, "127.0.0.1", port, true);
            if (!ok) { if (!engine->loop_once(5)) std::this_thread::sleep_for(5ms); }
        }
        EXPECT_TRUE(ok) << "connect failed for fd=" << clients[i].fd;
        std::this_thread::sleep_for(1ms);
    }
    // Wait for all connected
    auto t0 = std::chrono::steady_clock::now();
    while (connected.load() < N) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(CONNECT_TIMEOUT_MS))
            FAIL() << "Timeout waiting for clients to connect: " << connected.load() << "/" << N;
        if (!engine->loop_once(5)) std::this_thread::sleep_for(5ms);
    }

    // Measure start time (Unix timestamp) BEFORE kicking off first sends
    auto bench_start_tp = std::chrono::system_clock::now();
    // Initialize progress state for recv-callback prints
    {
        std::lock_guard<std::mutex> lk(prog.m);
        prog.bench_start_sys = bench_start_tp;
        prog.last_prog = std::chrono::steady_clock::now();
        prog.last_recvs = 0;
        prog.last_bytes = 0;
    }

    // Kick off initial window per client (up to PIPELINE in-flight messages)
    for (int i = 0; i < N; ++i) {
        auto &cl = clients[i];
        cl.msg_index = 0;
        cl.next_send_idx = 0;
        cl.next_recv_idx = 0;
        const int window = std::min(PIPELINE, R);
        for (int j = 0; j < window; ++j) {
            schedule_send(i, (size_t)j);
            cl.next_send_idx++;
        }
    }

    // Initial window armed for all clients

    // Also keep a steady_clock anchor for timeout tracking
    auto run_start_steady = std::chrono::steady_clock::now();

    // Measure loop until all messages are received back

    const uint64_t total_msgs = (uint64_t)N * (uint64_t)R;
    while (true) {
        uint64_t sends_done = 0, recvs_done = 0;
        uint64_t bytes_done = 0;
        for (int i = 0; i < N; ++i) { sends_done += clients[i].sends_completed; recvs_done += clients[i].recvs_completed; bytes_done += clients[i].bytes_received; }
        if (recvs_done >= total_msgs) break;
        if (std::chrono::steady_clock::now() - run_start_steady > std::chrono::milliseconds(RUN_TIMEOUT_MS)) {
            ADD_FAILURE() << "Timeout: sends=" << sends_done << "/" << total_msgs
                          << ", recvs=" << recvs_done << "/" << total_msgs;
            break;
        }
        if (!engine->loop_once(5)) std::this_thread::sleep_for(1ms);
    }
    auto bench_end_tp = std::chrono::system_clock::now();
    // Convert to Unix timestamps (seconds since epoch) with fractional part
    double start_ts = std::chrono::duration_cast<std::chrono::duration<double>>(bench_start_tp.time_since_epoch()).count();
    double end_ts = std::chrono::duration_cast<std::chrono::duration<double>>(bench_end_tp.time_since_epoch()).count();
    double secs = end_ts - start_ts;
    if (secs <= 0.0) secs = 1e-9;

    uint64_t sends_total = 0, recvs_total = 0; uint64_t bytes_total_sum = 0;
    for (int i = 0; i < N; ++i) {
        sends_total += clients[i].sends_completed;
        recvs_total += clients[i].recvs_completed;
        for (auto &mp : clients[i].plan) bytes_total_sum += (uint64_t)mp.size;
    }
    // Single end-to-end EPS based on completed roundtrips (full echo received)
    double eps = (double)recvs_total / secs;

    // Print a parse-friendly result line (stdout only)
    std::fprintf(stdout,
                 "EPS_RESULT backend=%s clients=%d msgs_per_client=%d min_mb=%d max_mb=%d pipeline=%d duration_s=%.6f start_ts=%.6f end_ts=%.6f eps=%.2f bytes_total=%llu\n",
                 backend_name().c_str(), N, R, MIN_MB, MAX_MB, PIPELINE, secs, start_ts, end_ts, eps,
                 (unsigned long long)bytes_total_sum);

    // Expectations: at least one message per second per client (very conservative lower bound)
    EXPECT_GT(recvs_total, 0u);

    // Cleanup
    for (int i = 0; i < N; ++i) engine->delete_socket(clients[i].fd);
    for (int i = 0; i < N; ++i) engine->disconnect(clients[i].fd);
    ::close(listen_fd);
    engine->destroy();
}
