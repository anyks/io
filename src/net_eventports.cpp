// Clean Solaris event ports backend implementation
#include "io/net.hpp"
#if defined(IO_ENGINE_EVENTPORTS)

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <port.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>

// Logging: errors in Debug, debug gated by IO_ENABLE_EVENTPORTS_VERBOSE
#ifndef NDEBUG
#define IO_LOG_ERR(fmt, ...) std::fprintf(stderr, "[io/eventports][ERR] " fmt "\n", ##__VA_ARGS__)
#else
#define IO_LOG_ERR(fmt, ...) ((void)0)
#endif

#ifdef IO_ENABLE_EVENTPORTS_VERBOSE
#define IO_LOG_DBG(fmt, ...) std::fprintf(stderr, "[io/eventports][DBG] " fmt "\n", ##__VA_ARGS__)
#else
#define IO_LOG_DBG(fmt, ...) ((void)0)
#endif

namespace io {

class EventPortsEngine : public INetEngine {
  public:
    EventPortsEngine() = default;
    ~EventPortsEngine() override { destroy(); }

        // Метрики
        NetStats get_stats() override;
        void reset_stats() override;

    bool init(const NetCallbacks &cbs) override {
        cbs_ = cbs;
        // Suppress SIGPIPE (POSIX) once per process
        io::suppress_sigpipe_once();
        port_ = ::port_create();
        if (port_ < 0) {
            IO_LOG_ERR("port_create failed errno=%d (%s)", errno, std::strerror(errno));
            return false;
        }
        // No internal threads: we rely on loop_once()/event_loop() like other backends
        return true;
    }

    void destroy() override {
        if (port_ != -1) {
            // Cleanup fds
            {
                for (auto &kv : sockets_) {
                    socket_t fd = kv.first;
                    (void)::port_dissociate(port_, PORT_SOURCE_FD, fd);
                    if (cbs_.on_close) cbs_.on_close(fd);
                    ::shutdown(fd, SHUT_RDWR);
                    ::close(fd);
                }
                sockets_.clear();
                listeners_.clear();
                // Disarm timers
                for (auto &kv : timers_) {
                    (void)kv.second.active; // metadata only; no kernel timer to delete
                }
                timers_.clear();
            }
            ::close(port_);
            port_ = -1;
        }
    }

    bool add_socket(socket_t fd, char *buffer, size_t buffer_size, ReadCallback cb) override {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) flags = 0;
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        // Construct in-place to avoid copying non-copyable atomics
        auto [it, inserted] = sockets_.try_emplace(fd);
        auto &st = it->second;
        st.buf = buffer;
        st.buf_size = buffer_size;
        st.read_cb = std::move(cb);
        st.out_queue.clear();
        st.want_write.store(false, std::memory_order_relaxed);
        st.connecting.store(false, std::memory_order_relaxed);
        st.paused.store(false, std::memory_order_relaxed);
        short mask = POLLIN | POLLHUP;
        (void)::port_associate(port_, PORT_SOURCE_FD, fd, mask, nullptr);
        return true;
    }

    bool delete_socket(socket_t fd) override {
        (void)::port_dissociate(port_, PORT_SOURCE_FD, fd);
        {
            auto it = sockets_.find(fd);
            if (it != sockets_.end()) {
                size_t pend = it->second.out_queue.size();
                if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
                sockets_.erase(it);
            }
        }
        timers_.erase(fd);
        return true;
    }

    bool connect(socket_t fd, const char *host, uint16_t port, bool async) override {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1)
            return false;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) flags = 0;
        if (!async) {
            if (flags & O_NONBLOCK)
                fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        }
        int r = ::connect(fd, (sockaddr *)&addr, sizeof(addr));
        int err = (r == 0) ? 0 : errno;
        int cur = fcntl(fd, F_GETFL, 0);
        if (cur < 0) cur = 0;
        fcntl(fd, F_SETFL, cur | O_NONBLOCK);
        if (r == 0 || err == EISCONN) {
            short mask = POLLIN | POLLHUP;
            (void)::port_associate(port_, PORT_SOURCE_FD, fd, mask, nullptr);
            IO_LOG_DBG("connect: established fd=%d", (int)fd);
            if (cbs_.on_accept) cbs_.on_accept(fd);
            stats_.connects_ok.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (async && (err == EINPROGRESS || err == EALREADY)) {
            short mask = POLLIN | POLLOUT | POLLHUP;
            (void)::port_associate(port_, PORT_SOURCE_FD, fd, mask, nullptr);
            sockets_[fd].connecting = true;
            return true;
        }
        IO_LOG_ERR("connect(fd=%d) failed, res=%d, errno=%d (%s)", (int)fd, r, err, std::strerror(err));
        stats_.connects_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool disconnect(socket_t fd) override {
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            IO_LOG_DBG("close: fd=%d", (int)fd);
            if (cbs_.on_close) cbs_.on_close(fd);
            stats_.closes.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    bool accept(socket_t listen_socket, bool async, uint32_t max_connections) override {
        if (async) {
            int flags = fcntl(listen_socket, F_GETFL, 0);
            fcntl(listen_socket, F_SETFL, flags | O_NONBLOCK);
        }
        max_conn_ = max_connections;
        listeners_.insert(listen_socket);
        (void)::port_associate(port_, PORT_SOURCE_FD, listen_socket, POLLIN, nullptr);
        return true;
    }

    bool write(socket_t fd, const char *data, size_t data_size) override {
        auto it = sockets_.find(fd);
        if (it == sockets_.end()) return false;
        auto &st = it->second;
        st.out_queue.insert(st.out_queue.end(), data, data + data_size);
        stats_.send_enqueued_bytes.fetch_add((uint64_t)data_size, std::memory_order_relaxed);
        if (!st.want_write) {
            st.want_write = true;
            short mask = POLLHUP | POLLOUT;
            if (!st.paused) mask |= POLLIN;
            (void)::port_associate(port_, PORT_SOURCE_FD, fd, mask, nullptr);
        }
        return true;
    }

    bool post(uint32_t user_event_value) override {
        // Deliver user events via native event ports user-source
        // Differentiate from timer wakeups by passing nullptr as user data
        if (::port_send(port_, (uintptr_t)user_event_value, nullptr) == 0) { stats_.user_events.fetch_add(1, std::memory_order_relaxed); return true; }
        return false;
    }

    bool set_read_timeout(socket_t socket, uint32_t timeout_ms) override {
        // Software timer managed in user thread, signaled via port_send (PORT_SOURCE_USER)
        if (timeout_ms == 0) {
            timers_.erase(socket);
            return true;
        }
        if (sockets_.find(socket) == sockets_.end())
            return false;
        {
            auto &ti = timers_[socket];
            ti.ms = timeout_ms;
            ti.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            ti.active = true;
        }
        return true;
    }

    bool pause_read(socket_t socket) override {
        auto it = sockets_.find(socket);
        if (it == sockets_.end()) return false;
        it->second.paused = true;
        short mask = POLLHUP;
        if (it->second.want_write) mask |= POLLOUT; // drop POLLIN
        (void)::port_associate(port_, PORT_SOURCE_FD, socket, mask, nullptr);
        return true;
    }

    bool resume_read(socket_t socket) override {
        auto it = sockets_.find(socket);
        if (it == sockets_.end()) return false;
        it->second.paused = false;
        short mask = POLLIN | POLLHUP;
        if (it->second.want_write) mask |= POLLOUT;
        (void)::port_associate(port_, PORT_SOURCE_FD, socket, mask, nullptr);
        return true;
    }

    // IOCP-specific — no-ops here
    bool set_accept_depth(socket_t /*listen_socket*/, uint32_t /*depth*/) override { return true; }
    bool set_accept_depth_ex(socket_t listen_socket, uint32_t depth, bool /*aggressive_cancel*/) override {
        return set_accept_depth(listen_socket, depth);
    }
    bool set_accept_autotune(socket_t /*listen_socket*/, const AcceptAutotuneConfig &/*cfg*/) override {
        return true;
    }

  private:
    struct SockState {
        char *buf{nullptr};
        size_t buf_size{0};
        ReadCallback read_cb;
        std::vector<char> out_queue;
        std::atomic_bool want_write{false};
        std::atomic_bool connecting{false};
        std::atomic_bool paused{false};
    };
    struct TimerInfoStore {
        uint32_t ms{0};
        std::chrono::steady_clock::time_point deadline{};
        bool active{false};
    };

    int port_{-1};
    NetCallbacks cbs_{};
    // No internal threads; timers are managed via computed timeouts passed to port_getn
    std::unordered_map<socket_t, SockState> sockets_;
    std::unordered_set<socket_t> listeners_;
    std::unordered_map<socket_t, TimerInfoStore> timers_;
    std::atomic<uint32_t> max_conn_{0};
    std::atomic<uint32_t> cur_conn_{0};
    struct StatsAtomic {
        std::atomic<uint64_t> accepts_ok{0}, accepts_fail{0};
        std::atomic<uint64_t> connects_ok{0}, connects_fail{0};
        std::atomic<uint64_t> reads{0}, bytes_read{0};
        std::atomic<uint64_t> writes{0}, bytes_written{0};
        std::atomic<uint64_t> closes{0}, timeouts{0};
        std::atomic<uint64_t> user_events{0}, wakes_posted{0};
        std::atomic<uint64_t> peak_connections{0};
        std::atomic<uint64_t> send_enqueued_bytes{0}, send_dequeued_bytes{0}, send_dropped_bytes{0};
    } stats_{};

    // Compute next timeout in milliseconds for active timers; returns -1 if none (infinite)
    int compute_next_timeout_ms(std::chrono::steady_clock::time_point now) {
        bool has_any = false;
        auto next = now + std::chrono::hours(24);
        for (auto &kv : timers_) {
            if (!kv.second.active) continue;
            has_any = true;
            if (kv.second.deadline < next) next = kv.second.deadline;
        }
        if (!has_any) return -1;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
        return ms < 0 ? 0 : (int)ms;
    }

    void close_expired(std::chrono::steady_clock::time_point now) {
        std::vector<socket_t> expired;
        for (auto &kv : timers_) {
            if (kv.second.active && kv.second.deadline <= now) {
                expired.push_back(kv.first);
                kv.second.active = false;
            }
        }
        for (auto sfd : expired) {
            auto itS = sockets_.find(sfd);
            if (itS != sockets_.end()) {
                if (cbs_.on_close) cbs_.on_close(sfd);
                (void)::port_dissociate(port_, PORT_SOURCE_FD, sfd);
                ::shutdown(sfd, SHUT_RDWR);
                ::close(sfd);
                {
                    size_t pend = itS->second.out_queue.size();
                    if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
                    sockets_.erase(itS);
                }
                if (cur_conn_ > 0) cur_conn_--;
                stats_.timeouts.fetch_add(1, std::memory_order_relaxed);
                stats_.closes.fetch_add(1, std::memory_order_relaxed);
            }
            timers_.erase(sfd);
        }
    }

    bool process_once(int to_ms) {
        port_event_t evs[64];
        uint_t nget = 1;
        timespec ts{}; timespec *pts = nullptr;
        if (to_ms >= 0) { ts.tv_sec = to_ms/1000; ts.tv_nsec = (to_ms%1000)*1000000L; pts = &ts; }
        int r = ::port_getn(port_, evs, 64, &nget, pts);
            if (r != 0) {
                if (errno == EINTR || errno == ETIME)
                    return false;
                IO_LOG_ERR("port_getn failed errno=%d (%s)", errno, std::strerror(errno));
            return false;
            }
        if (nget == 0) return false;
        for (uint_t i = 0; i < nget; ++i) {
                auto &ev = evs[i];
                if (ev.portev_source == PORT_SOURCE_USER) {
                    if (cbs_.on_user) cbs_.on_user(static_cast<uint32_t>(ev.portev_events));
                    continue;
                }
                if (ev.portev_source != PORT_SOURCE_FD) continue;
                int fd = static_cast<int>(ev.portev_object);

                // Listener accept
                bool is_listener = (listeners_.find(fd) != listeners_.end());
                if (is_listener && (ev.portev_events & POLLIN)) {
                    while (true) {
                        sockaddr_storage ss{};
                        socklen_t slen = sizeof(ss);
                        socket_t client = ::accept(fd, (sockaddr *)&ss, &slen);
                        if (client < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            else break;
                        }
                        if (max_conn_ > 0 && cur_conn_.load() >= max_conn_) {
                            ::close(client);
                            continue;
                        }
                        int fl = fcntl(client, F_GETFL, 0);
                        if (fl < 0) fl = 0;
                        fcntl(client, F_SETFL, fl | O_NONBLOCK);
                        // preregister client in sockets_ (construct in-place)
                        sockets_.try_emplace((socket_t)client);
                        cur_conn_++;
                        IO_LOG_DBG("accept: client fd=%d", (int)client);
                        if (cbs_.on_accept) cbs_.on_accept(client);
                    }
                    (void)::port_associate(port_, PORT_SOURCE_FD, fd, POLLIN, nullptr);
                    continue;
                }

                // Connect completion
                if (ev.portev_events & POLLOUT) {
                    auto it = sockets_.find(fd);
                    if (it != sockets_.end() && it->second.connecting) {
                        int err = 0;
                        socklen_t len = sizeof(err);
                        int gs = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                        if (gs < 0) {
                            err = errno;
                            IO_LOG_ERR("getsockopt(SO_ERROR fd=%d) failed errno=%d (%s)", fd, err, std::strerror(err));
                        } else {
                            IO_LOG_DBG("connect completion fd=%d, SO_ERROR=%d (%s)", fd, err, std::strerror(err));
                        }
                        it->second.connecting = false;
                        if (err == 0) {
                            IO_LOG_DBG("connect: established fd=%d", fd);
                            if (cbs_.on_accept) cbs_.on_accept(fd);
                        } else {
                            if (cbs_.on_close) cbs_.on_close(fd);
                            ::close(fd);
                            {
                                auto it2 = sockets_.find(fd);
                                if (it2 != sockets_.end()) {
                                    size_t pend = it2->second.out_queue.size();
                                    if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
                                    sockets_.erase(it2);
                                }
                            }
                            stats_.closes.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                    }
                }

                // Read path
                if (ev.portev_events & POLLIN) {
                    auto it = sockets_.find(fd);
                    if (it != sockets_.end()) {
                        auto &st_ref = it->second;
                        bool paused_now = st_ref.paused.load(std::memory_order_relaxed);
                        char *buf = st_ref.buf;
                        size_t buf_size = st_ref.buf_size;
                        ReadCallback cb = st_ref.read_cb; // copy callable; safe if state mutates inside
                        if (buf && cb && buf_size > 0 && !paused_now) {
                            ssize_t rn = ::recv(fd, buf, buf_size, 0);
                            if (rn > 0) {
                                cb(fd, buf, (size_t)rn);
                                auto itT = timers_.find(fd);
                                if (itT != timers_.end()) {
                                    uint32_t ms = itT->second.ms;
                                    itT->second.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                                    itT->second.active = true;
                                }
                            } else if (rn == 0 || (ev.portev_events & (POLLHUP | POLLERR))) {
                                // peer closed or error: dissociate and close fd, cleanup state
                                (void)::port_dissociate(port_, PORT_SOURCE_FD, fd);
                                if (cbs_.on_close) cbs_.on_close(fd);
                                ::shutdown(fd, SHUT_RDWR);
                                ::close(fd);
                                sockets_.erase(fd);
                                if (cur_conn_ > 0) cur_conn_--;
                                timers_.erase(fd);
                            }
                        }
                    }
                }

                // Write flush
                if (ev.portev_events & POLLOUT) {
                    auto it = sockets_.find(fd);
                    if (it != sockets_.end() && !it->second.out_queue.empty()) {
                        auto &st = it->second;
                        auto *p = st.out_queue.data();
                        size_t sz = st.out_queue.size();
                        ssize_t wn = ::send(fd, p, sz, 0);
                        if (wn > 0) {
                            if (cbs_.on_write) cbs_.on_write(fd, (size_t)wn);
                            stats_.writes.fetch_add(1, std::memory_order_relaxed);
                            stats_.bytes_written.fetch_add((uint64_t)wn, std::memory_order_relaxed);
                            stats_.send_dequeued_bytes.fetch_add((uint64_t)wn, std::memory_order_relaxed);
                            st.out_queue.erase(st.out_queue.begin(), st.out_queue.begin() + wn);
                        } else if (wn < 0) {
                            int e = errno;
                            if (e == EPIPE || e == ECONNRESET) {
                                io::record_broken_pipe();
                                IO_LOG_DBG("write: fd=%d EPIPE/ECONNRESET -> drop OUT, want_write=0", fd);
                                st.want_write = false;
                            }
                        }
                        if (st.out_queue.empty()) {
                            st.want_write = false;
                        }
                    }
                }

                // Re-associate with current mask (one-shot)
                short mask = POLLHUP;
                {
                    auto it = sockets_.find(fd);
                    if (it != sockets_.end()) {
                        if (!it->second.paused) mask |= POLLIN;
                        if (it->second.want_write) mask |= POLLOUT;
                    }
                }
                if (::port_associate(port_, PORT_SOURCE_FD, fd, mask, nullptr) != 0) {
                    IO_LOG_ERR("port_associate(rearm fd=%d, mask=0x%x) failed errno=%d (%s)", fd, mask, errno,
                               std::strerror(errno));
                }
        }
        return true;
    }

    // INetEngine overrides to align with other backends
    bool loop_once(uint32_t timeout_ms) override {
        auto now = std::chrono::steady_clock::now();
        int to_ms = (timeout_ms == 0xFFFFFFFFu) ? compute_next_timeout_ms(now) : (int)timeout_ms;
        bool got = process_once(to_ms);
        // Handle timer expirations opportunistically each tick
        close_expired(std::chrono::steady_clock::now());
        return got;
    }

    void event_loop(std::atomic<bool> &run_flag, int32_t wait_ms) override {
        while (run_flag.load(std::memory_order_relaxed)) {
            auto now = std::chrono::steady_clock::now();
            int base_to = (wait_ms < 0) ? compute_next_timeout_ms(now) : wait_ms;
            (void)process_once(base_to);
            close_expired(std::chrono::steady_clock::now());
        }
    }

    void wake() override {
        // Wake port_getn via user event
        (void)::port_send(port_, 0, nullptr);
        stats_.wakes_posted.fetch_add(1, std::memory_order_relaxed);
    }
};

INetEngine *create_engine_eventports() {
    return new EventPortsEngine();
}

NetStats EventPortsEngine::get_stats() {
    NetStats s{};
    s.accepts_ok = stats_.accepts_ok.load(std::memory_order_relaxed);
    s.accepts_fail = stats_.accepts_fail.load(std::memory_order_relaxed);
    s.connects_ok = stats_.connects_ok.load(std::memory_order_relaxed);
    s.connects_fail = stats_.connects_fail.load(std::memory_order_relaxed);
    s.reads = stats_.reads.load(std::memory_order_relaxed);
    s.bytes_read = stats_.bytes_read.load(std::memory_order_relaxed);
    s.writes = stats_.writes.load(std::memory_order_relaxed);
    s.bytes_written = stats_.bytes_written.load(std::memory_order_relaxed);
    s.closes = stats_.closes.load(std::memory_order_relaxed);
    s.timeouts = stats_.timeouts.load(std::memory_order_relaxed);
    s.user_events = stats_.user_events.load(std::memory_order_relaxed);
    s.wakes_posted = stats_.wakes_posted.load(std::memory_order_relaxed);
    s.current_connections = (uint64_t)cur_conn_.load(std::memory_order_relaxed);
    s.peak_connections = stats_.peak_connections.load(std::memory_order_relaxed);
    s.outstanding_accepts = 0;
    s.send_enqueued_bytes = stats_.send_enqueued_bytes.load(std::memory_order_relaxed);
    s.send_dequeued_bytes = stats_.send_dequeued_bytes.load(std::memory_order_relaxed);
    s.send_backlog_bytes = (s.send_enqueued_bytes >= s.send_dequeued_bytes) ? (s.send_enqueued_bytes - s.send_dequeued_bytes) : 0;
    s.send_dropped_bytes = stats_.send_dropped_bytes.load(std::memory_order_relaxed);
    return s;
}

void EventPortsEngine::reset_stats() {
    stats_.accepts_ok.store(0, std::memory_order_relaxed);
    stats_.accepts_fail.store(0, std::memory_order_relaxed);
    stats_.connects_ok.store(0, std::memory_order_relaxed);
    stats_.connects_fail.store(0, std::memory_order_relaxed);
    stats_.reads.store(0, std::memory_order_relaxed);
    stats_.bytes_read.store(0, std::memory_order_relaxed);
    stats_.writes.store(0, std::memory_order_relaxed);
    stats_.bytes_written.store(0, std::memory_order_relaxed);
    stats_.closes.store(0, std::memory_order_relaxed);
    stats_.timeouts.store(0, std::memory_order_relaxed);
    stats_.user_events.store(0, std::memory_order_relaxed);
    stats_.wakes_posted.store(0, std::memory_order_relaxed);
    stats_.peak_connections.store((uint64_t)cur_conn_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    stats_.send_enqueued_bytes.store(0, std::memory_order_relaxed);
    stats_.send_dequeued_bytes.store(0, std::memory_order_relaxed);
    stats_.send_dropped_bytes.store(0, std::memory_order_relaxed);
}

} // namespace io

#endif

