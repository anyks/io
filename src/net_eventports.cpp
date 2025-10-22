// Clean Solaris event ports backend implementation
#include "io/net.hpp"
#if defined(IO_ENGINE_EVENTPORTS)

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <port.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <condition_variable>
#include <chrono>

#ifndef NDEBUG
#define IO_LOG_ERR(fmt, ...) std::fprintf(stderr, "[io/eventports][ERR] " fmt "\n", ##__VA_ARGS__)
#define IO_LOG_DBG(fmt, ...) std::fprintf(stderr, "[io/eventports][DBG] " fmt "\n", ##__VA_ARGS__)
#else
#define IO_LOG_ERR(fmt, ...) ((void)0)
#define IO_LOG_DBG(fmt, ...) ((void)0)
#endif

namespace io {

class EventPortsEngine : public INetEngine {
  public:
    EventPortsEngine() = default;
    ~EventPortsEngine() override { destroy(); }

    bool init(const NetCallbacks &cbs) override {
        cbs_ = cbs;
        // Suppress SIGPIPE (POSIX) once per process
        io::suppress_sigpipe_once();
        port_ = ::port_create();
        if (port_ < 0) {
            IO_LOG_ERR("port_create failed errno=%d (%s)", errno, std::strerror(errno));
            return false;
        }
        if (pipe(user_pipe_) == 0) {
            int f0 = fcntl(user_pipe_[0], F_GETFL, 0);
            fcntl(user_pipe_[0], F_SETFL, f0 | O_NONBLOCK);
            int f1 = fcntl(user_pipe_[1], F_GETFL, 0);
            fcntl(user_pipe_[1], F_SETFL, f1 | O_NONBLOCK);
            (void)::port_associate(port_, PORT_SOURCE_FD, user_pipe_[0], POLLIN, nullptr);
        }
        running_ = true;
        timer_running_ = true;
        loop_ = std::thread(&EventPortsEngine::event_loop, this);
        timer_thr_ = std::thread(&EventPortsEngine::timer_loop, this);
        return true;
    }

    void destroy() override {
        if (port_ != -1) {
            running_ = false;
            timer_running_ = false;
            if (user_pipe_[1] != -1) {
                uint32_t v = 0;
                (void)::write(user_pipe_[1], &v, sizeof(v));
            }
            if (loop_.joinable()) loop_.join();
            if (timer_thr_.joinable()) timer_thr_.join();
            // Cleanup fds
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto &kv : sockets_) {
                    socket_t fd = kv.first;
                    (void)::port_dissociate(port_, PORT_SOURCE_FD, fd);
                    if (cbs_.on_close) cbs_.on_close(fd);
                    ::shutdown(fd, SHUT_RDWR);
                    ::close(fd);
                }
                sockets_.clear();
                listeners_.clear();
                timers_.clear();
                timed_out_.clear();
            }
            if (user_pipe_[0] != -1) ::close(user_pipe_[0]);
            if (user_pipe_[1] != -1) ::close(user_pipe_[1]);
            ::close(port_);
            port_ = -1;
        }
    }

    bool add_socket(socket_t fd, char *buffer, size_t buffer_size, ReadCallback cb) override {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) flags = 0;
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            sockets_[fd] = SockState{buffer, buffer_size, std::move(cb), {}, false, false, false};
        }
        short mask = POLLIN | POLLHUP;
        (void)::port_associate(port_, PORT_SOURCE_FD, fd, mask, nullptr);
        return true;
    }

    bool delete_socket(socket_t fd) override {
        (void)::port_dissociate(port_, PORT_SOURCE_FD, fd);
        std::lock_guard<std::mutex> lk(mtx_);
        sockets_.erase(fd);
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
            return true;
        }
        if (async && (err == EINPROGRESS || err == EALREADY)) {
            short mask = POLLIN | POLLOUT | POLLHUP;
            (void)::port_associate(port_, PORT_SOURCE_FD, fd, mask, nullptr);
            std::lock_guard<std::mutex> lk(mtx_);
            sockets_[fd].connecting = true;
            return true;
        }
        IO_LOG_ERR("connect(fd=%d) failed, res=%d, errno=%d (%s)", (int)fd, r, err, std::strerror(err));
        return false;
    }

    bool disconnect(socket_t fd) override {
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            IO_LOG_DBG("close: fd=%d", (int)fd);
            if (cbs_.on_close) cbs_.on_close(fd);
        }
        return true;
    }

    bool accept(socket_t listen_socket, bool async, uint32_t max_connections) override {
        if (async) {
            int flags = fcntl(listen_socket, F_GETFL, 0);
            fcntl(listen_socket, F_SETFL, flags | O_NONBLOCK);
        }
        max_conn_ = max_connections;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            listeners_.insert(listen_socket);
        }
        (void)::port_associate(port_, PORT_SOURCE_FD, listen_socket, POLLIN, nullptr);
        return true;
    }

    bool write(socket_t fd, const char *data, size_t data_size) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = sockets_.find(fd);
        if (it == sockets_.end()) return false;
        auto &st = it->second;
        st.out_queue.insert(st.out_queue.end(), data, data + data_size);
        if (!st.want_write) {
            st.want_write = true;
            short mask = POLLHUP | POLLOUT;
            if (!st.paused) mask |= POLLIN;
            (void)::port_associate(port_, PORT_SOURCE_FD, fd, mask, nullptr);
        }
        return true;
    }

    bool post(uint32_t user_event_value) override {
        if (user_pipe_[1] == -1) return false;
        ssize_t n = ::write(user_pipe_[1], &user_event_value, sizeof(user_event_value));
        return n == (ssize_t)sizeof(user_event_value);
    }

    bool set_read_timeout(socket_t socket, uint32_t timeout_ms) override {
        // Software timer managed in user thread, signaled via user_pipe_
        if (timeout_ms == 0) {
            std::lock_guard<std::mutex> lk(mtx_);
            timers_.erase(socket);
            cv_.notify_all();
            return true;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (sockets_.find(socket) == sockets_.end())
                return false;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto &ti = timers_[socket];
            ti.ms = timeout_ms;
            ti.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            ti.active = true;
            cv_.notify_all();
        }
        return true;
    }

    bool pause_read(socket_t socket) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = sockets_.find(socket);
        if (it == sockets_.end()) return false;
        it->second.paused = true;
        short mask = POLLHUP;
        if (it->second.want_write) mask |= POLLOUT; // drop POLLIN
        (void)::port_associate(port_, PORT_SOURCE_FD, socket, mask, nullptr);
        return true;
    }

    bool resume_read(socket_t socket) override {
        std::lock_guard<std::mutex> lk(mtx_);
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
        bool want_write{false};
        bool connecting{false};
        bool paused{false};
    };
    struct TimerInfoStore {
        uint32_t ms{0};
        std::chrono::steady_clock::time_point deadline{};
        bool active{false};
    };

    int port_{-1};
    NetCallbacks cbs_{};
    std::thread loop_{};
    std::thread timer_thr_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> timer_running_{false};
    int user_pipe_[2]{-1, -1};
    std::mutex mtx_;
    std::unordered_map<socket_t, SockState> sockets_;
    std::unordered_set<socket_t> listeners_;
    std::unordered_map<socket_t, TimerInfoStore> timers_;
    std::vector<socket_t> timed_out_;
    std::condition_variable cv_;
    std::atomic<uint32_t> max_conn_{0};
    std::atomic<uint32_t> cur_conn_{0};

    void event_loop() {
        constexpr uint32_t kTimerTickMagic = 0xFFFFFFFEu;
        while (running_) {
            port_event_t evs[64];
            uint_t nget = 1;
            int r = ::port_getn(port_, evs, 64, &nget, nullptr);
            if (r != 0) {
                if (errno == EINTR || errno == ETIME)
                    continue;
                IO_LOG_ERR("port_getn failed errno=%d (%s)", errno, std::strerror(errno));
                continue;
            }
            if (nget == 0) continue;
            for (uint_t i = 0; i < nget; ++i) {
                auto &ev = evs[i];
                if (ev.portev_source != PORT_SOURCE_FD) continue;
                int fd = static_cast<int>(ev.portev_object);
                if (fd == user_pipe_[0]) {
                    uint32_t v;
                    while (::read(user_pipe_[0], &v, sizeof(v)) == sizeof(v)) {
                        if (v != kTimerTickMagic) {
                            if (cbs_.on_user) cbs_.on_user(v);
                        }
                    }
                    // process software timer expirations
                    std::vector<socket_t> to_close;
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        to_close.swap(timed_out_);
                    }
                    for (auto sfd : to_close) {
                        bool exists = false;
                        {
                            std::lock_guard<std::mutex> lk(mtx_);
                            exists = sockets_.find(sfd) != sockets_.end();
                        }
                        if (exists) {
                            if (cbs_.on_close) cbs_.on_close(sfd);
                            ::close(sfd);
                            std::lock_guard<std::mutex> lk2(mtx_);
                            sockets_.erase(sfd);
                            timers_.erase(sfd);
                            if (cur_conn_ > 0) cur_conn_--;
                        }
                    }
                    (void)::port_associate(port_, PORT_SOURCE_FD, user_pipe_[0], POLLIN, nullptr);
                    continue;
                }

                // Listener accept
                bool is_listener = false;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    is_listener = listeners_.find(fd) != listeners_.end();
                }
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
                        // preregister client in sockets_
                        {
                            std::lock_guard<std::mutex> lk(mtx_);
                            SockState st{};
                            st.buf = nullptr;
                            st.buf_size = 0;
                            st.read_cb = ReadCallback{};
                            sockets_.emplace((socket_t)client, std::move(st));
                        }
                        cur_conn_++;
                        IO_LOG_DBG("accept: client fd=%d", (int)client);
                        if (cbs_.on_accept) cbs_.on_accept(client);
                    }
                    (void)::port_associate(port_, PORT_SOURCE_FD, fd, POLLIN, nullptr);
                    continue;
                }

                // Connect completion
                if (ev.portev_events & POLLOUT) {
                    std::unique_lock<std::mutex> lk(mtx_);
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
                            lk.unlock();
                            IO_LOG_DBG("connect: established fd=%d", fd);
                            if (cbs_.on_accept) cbs_.on_accept(fd);
                        } else {
                            lk.unlock();
                            if (cbs_.on_close) cbs_.on_close(fd);
                            ::close(fd);
                            std::lock_guard<std::mutex> lk2(mtx_);
                            sockets_.erase(fd);
                            continue;
                        }
                    }
                }

                // Read path
                if (ev.portev_events & POLLIN) {
                    std::unique_lock<std::mutex> lk(mtx_);
                    auto it = sockets_.find(fd);
                    if (it != sockets_.end()) {
                        auto st = it->second;
                        bool paused_now = st.paused;
                        lk.unlock();
                        if (st.buf && st.read_cb && st.buf_size > 0 && !paused_now) {
                            ssize_t rn = ::recv(fd, st.buf, st.buf_size, 0);
                            if (rn > 0) {
                                st.read_cb(fd, st.buf, (size_t)rn);
                                std::lock_guard<std::mutex> lk2(mtx_);
                                auto itT = timers_.find(fd);
                                if (itT != timers_.end()) {
                                    uint32_t ms = itT->second.ms;
                                    itT->second.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                                    itT->second.active = true;
                                    cv_.notify_all();
                                }
                            } else if (rn == 0 || (ev.portev_events & (POLLHUP | POLLERR))) {
                                if (cbs_.on_close) cbs_.on_close(fd);
                                std::lock_guard<std::mutex> lk2(mtx_);
                                sockets_.erase(fd);
                                if (cur_conn_ > 0) cur_conn_--;
                                timers_.erase(fd);
                            }
                        }
                    } else {
                        lk.unlock();
                    }
                }

                // Write flush
                if (ev.portev_events & POLLOUT) {
                    std::unique_lock<std::mutex> lk(mtx_);
                    auto it = sockets_.find(fd);
                    if (it != sockets_.end() && !it->second.out_queue.empty()) {
                        auto &st = it->second;
                        auto *p = st.out_queue.data();
                        size_t sz = st.out_queue.size();
                        lk.unlock();
                        ssize_t wn = ::send(fd, p, sz, 0);
                        lk.lock();
                        if (wn > 0) {
                            if (cbs_.on_write) cbs_.on_write(fd, (size_t)wn);
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
                    std::lock_guard<std::mutex> lk(mtx_);
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
        }
    }

    void timer_loop() {
        constexpr uint32_t kTimerTickMagic = 0xFFFFFFFEu;
        while (timer_running_) {
            std::unique_lock<std::mutex> lk(mtx_);
            if (!timer_running_) break;
            auto now = std::chrono::steady_clock::now();
            std::chrono::steady_clock::time_point next = now + std::chrono::hours(24);
            bool has_any = false;
            for (auto &kv : timers_) {
                if (!kv.second.active) continue;
                has_any = true;
                if (kv.second.deadline < next) next = kv.second.deadline;
            }
            if (!has_any) {
                cv_.wait_for(lk, std::chrono::milliseconds(250));
                continue;
            }
            if (next > now) {
                cv_.wait_until(lk, next);
            }
            now = std::chrono::steady_clock::now();
            std::vector<socket_t> expired;
            for (auto &kv : timers_) {
                if (kv.second.active && kv.second.deadline <= now) {
                    expired.push_back(kv.first);
                    kv.second.active = false;
                }
            }
            if (!expired.empty()) {
                timed_out_.insert(timed_out_.end(), expired.begin(), expired.end());
                lk.unlock();
                (void)::write(user_pipe_[1], &kTimerTickMagic, sizeof(kTimerTickMagic));
            }
        }
    }
};

INetEngine *create_engine_eventports() {
    return new EventPortsEngine();
}

} // namespace io

#endif

