#include "io/net.hpp"
#if defined(IO_ENGINE_DEVPOLL)
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/devpoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Logging: errors in Debug, debug gated by IO_ENABLE_DEVPOLL_VERBOSE
#ifndef NDEBUG
#define IO_LOG_ERR(fmt, ...) std::fprintf(stderr, "[io/devpoll][ERR] " fmt "\n", ##__VA_ARGS__)
#else
#define IO_LOG_ERR(fmt, ...) ((void)0)
#endif

#ifdef IO_ENABLE_DEVPOLL_VERBOSE
#define IO_LOG_DBG(fmt, ...) std::fprintf(stderr, "[io/devpoll][DBG] " fmt "\n", ##__VA_ARGS__)
#else
#define IO_LOG_DBG(fmt, ...) ((void)0)
#endif

namespace io {

class DevPollEngine : public INetEngine {
  public:
	DevPollEngine() = default;
	~DevPollEngine() override {
		destroy();
	}
	bool init(const NetCallbacks &cbs) override {
		cbs_ = cbs;
		// Suppress SIGPIPE (POSIX) once per process
		io::suppress_sigpipe_once();
		dpfd_ = ::open("/dev/poll", O_RDWR);
		if (dpfd_ < 0) {
			IO_LOG_ERR("open(/dev/poll) failed errno=%d (%s)", errno, std::strerror(errno));
			return false;
		}
		if (pipe(user_pipe_) == 0) {
			int f0 = fcntl(user_pipe_[0], F_GETFL, 0);
			fcntl(user_pipe_[0], F_SETFL, f0 | O_NONBLOCK);
			int f1 = fcntl(user_pipe_[1], F_GETFL, 0);
			fcntl(user_pipe_[1], F_SETFL, f1 | O_NONBLOCK);
			add_poll(user_pipe_[0], POLLIN);
		}
		return true;
	}
	void destroy() override {
		if (dpfd_ != -1) {
			// Cleanup all tracked sockets and timers
			for (auto &kv : sockets_) {
				int fd = (int)kv.first;
				rem_poll(fd);
				if (cbs_.on_close)
					cbs_.on_close(kv.first);
				::shutdown(fd, SHUT_RDWR);
				::close(fd);
			}
			sockets_.clear();
			timers_.clear();
			listeners_.clear();
			timeouts_ms_.clear();
			if (user_pipe_[0] != -1)
				::close(user_pipe_[0]);
			if (user_pipe_[1] != -1)
				::close(user_pipe_[1]);
			::close(dpfd_);
			dpfd_ = -1;
		}
	}
	bool add_socket(socket_t fd, char *buffer, size_t buffer_size, ReadCallback cb) override {
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0)
			flags = 0;
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		sockets_[fd] = SockState{buffer, buffer_size, std::move(cb), {}, false, false, false};
		add_poll(fd, POLLIN | POLLRDNORM | POLLHUP);
		return true;
	}
	bool delete_socket(socket_t fd) override {
		rem_poll(fd);
		timers_.erase(fd);
		sockets_.erase(fd);
		timeouts_ms_.erase(fd);
		return true;
	}
	bool connect(socket_t fd, const char *host, uint16_t port, bool async) override {
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1)
			return false;
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0)
			flags = 0;
		if (!async) {
			if (flags & O_NONBLOCK)
				fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
		}
		int r = ::connect(fd, (sockaddr *)&addr, sizeof(addr));
		int err = (r == 0) ? 0 : errno;
		int cur = fcntl(fd, F_GETFL, 0);
		if (cur < 0)
			cur = 0;
		fcntl(fd, F_SETFL, cur | O_NONBLOCK);
		if (r == 0) {
			add_poll(fd, POLLIN | POLLRDNORM | POLLHUP);
			if (cbs_.on_accept)
				cbs_.on_accept(fd);
			return true;
		}
		if (async && (err == EINPROGRESS)) {
			add_poll(fd, POLLOUT | POLLIN | POLLRDNORM | POLLHUP);
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
			if (cbs_.on_close)
				cbs_.on_close(fd);
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
		add_poll(listen_socket, POLLIN);
		return true;
	}
	bool write(socket_t fd, const char *data, size_t data_size) override {
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return false;
		auto &st = it->second;
		st.out_queue.insert(st.out_queue.end(), data, data + data_size);
		if (!st.want_write) {
			st.want_write = true;
			add_poll(fd, POLLIN | POLLRDNORM | POLLOUT | POLLHUP);
		}
		return true;
	}
	bool pause_read(socket_t socket) override {
		auto it = sockets_.find(socket);
		if (it == sockets_.end())
			return false;
		it->second.paused = true;
		short mask = POLLHUP;
		if (it->second.want_write)
			mask |= POLLOUT;
		add_poll(socket, mask);
		return true;
	}
	bool resume_read(socket_t socket) override {
		auto it = sockets_.find(socket);
		if (it == sockets_.end())
			return false;
		it->second.paused = false;
		short mask = POLLIN | POLLRDNORM | POLLHUP;
		if (it->second.want_write)
			mask |= POLLOUT;
		add_poll(socket, mask);
		return true;
	}
	bool post(uint32_t val) override {
		if (user_pipe_[1] == -1)
			return false;
		ssize_t n = ::write(user_pipe_[1], &val, sizeof(val));
		return n == (ssize_t)sizeof(val);
	}
	// IOCP-specific APIs: provide no-op implementations for non-IOCP backends
	bool set_accept_depth(socket_t /*listen_socket*/, uint32_t /*depth*/) override { return true; }
	bool set_accept_depth_ex(socket_t listen_socket, uint32_t depth, bool /*aggressive_cancel*/) override {
		return set_accept_depth(listen_socket, depth);
	}
	bool set_accept_autotune(socket_t /*listen_socket*/, const AcceptAutotuneConfig &/*cfg*/) override {
		return true;
	}
	bool set_read_timeout(socket_t socket, uint32_t timeout_ms) override {
		// Track logical timer; use DP_POLL timeout to drive expirations
		if (timeout_ms == 0) {
			timers_.erase(socket);
			timeouts_ms_.erase(socket);
			return true;
		}
		if (sockets_.find(socket) == sockets_.end())
			return false;
		{
			TimerInfo ti{};
			ti.ms = timeout_ms;
			ti.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
			ti.active = true;
			timers_[socket] = ti;
			timeouts_ms_[socket] = timeout_ms;
		}
		return true;
	}

  private:
	struct SockState {
		char *buf{nullptr};
		size_t buf_size{0};
		ReadCallback read_cb;
		std::vector<char> out_queue;
		std::atomic<bool> want_write{false};
		std::atomic<bool> connecting{false};
		std::atomic<bool> paused{false};
	};
	struct TimerInfo { uint32_t ms{0}; std::chrono::steady_clock::time_point deadline{}; bool active{false}; };
	int dpfd_{-1};
	NetCallbacks cbs_{};
	int user_pipe_[2]{-1, -1};
	std::unordered_map<socket_t, SockState> sockets_;
	std::unordered_set<socket_t> listeners_;
	std::unordered_map<socket_t, uint32_t> timeouts_ms_;
	std::unordered_map<socket_t, TimerInfo> timers_;
	std::atomic<uint32_t> max_conn_{0};
	std::atomic<uint32_t> cur_conn_{0};

	int compute_next_timeout_ms(std::chrono::steady_clock::time_point now) {
		bool has_any=false; auto next = now + std::chrono::hours(24);
		for (auto &kv : timers_) { if (!kv.second.active) continue; has_any=true; if (kv.second.deadline < next) next = kv.second.deadline; }
		if (!has_any) return 200; // default poll period
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
		return ms < 0 ? 0 : (int)ms;
	}
	void close_expired(std::chrono::steady_clock::time_point now) {
		std::vector<socket_t> expired;
		for (auto &kv : timers_) { if (kv.second.active && kv.second.deadline <= now) { expired.push_back(kv.first); kv.second.active=false; } }
		for (auto sfd : expired) {
			rem_poll((int)sfd);
			if (cbs_.on_close) cbs_.on_close(sfd);
			::shutdown((int)sfd, SHUT_RDWR); ::close((int)sfd);
			sockets_.erase(sfd); timers_.erase(sfd); timeouts_ms_.erase(sfd);
			if (cur_conn_>0) cur_conn_--;
		}
	}
	void add_poll(int fd, short events) {
		struct pollfd p{};
		p.fd = fd;
		p.events = events;
		ssize_t wr = ::write(dpfd_, &p, sizeof(p));
		if (wr != (ssize_t)sizeof(p)) {
			IO_LOG_ERR("write(/dev/poll add fd=%d events=0x%x) failed errno=%d (%s)", fd, (unsigned)events, errno,
					   std::strerror(errno));
		}
	}
	void rem_poll(int fd) {
		struct pollfd p{};
		p.fd = fd;
		p.events = POLLREMOVE; // remove fd from /dev/poll set
		p.revents = 0;
		ssize_t wr = ::write(dpfd_, &p, sizeof(p));
		if (wr != (ssize_t)sizeof(p)) {
			IO_LOG_ERR("write(/dev/poll del fd=%d) failed errno=%d (%s)", fd, errno, std::strerror(errno));
		}
	}

	bool loop_once(uint32_t timeout_ms) override {
		if (dpfd_ < 0) return false;
		dvpoll dv{}; pollfd evs[1024]; dv.dp_fds = evs; dv.dp_nfds = 1024;
		auto now = std::chrono::steady_clock::now();
		int base_to = (timeout_ms == 0xFFFFFFFFu) ? compute_next_timeout_ms(now) : (int)timeout_ms;
		dv.dp_timeout = base_to;
		int n = ::ioctl(dpfd_, DP_POLL, &dv);
		if (n < 0) { if (errno == EINTR) { close_expired(std::chrono::steady_clock::now()); return false; } IO_LOG_ERR("ioctl(DP_POLL) failed errno=%d (%s)", errno, std::strerror(errno)); close_expired(std::chrono::steady_clock::now()); return false; }
		if (n == 0) { close_expired(std::chrono::steady_clock::now()); return false; }
			for (int i = 0; i < n; ++i) {
				int fd = evs[i].fd;
				short re = evs[i].revents;
				if (fd == user_pipe_[0] && (re & POLLIN)) {
					uint32_t v;
					while (::read(user_pipe_[0], &v, sizeof(v)) == sizeof(v)) {
						if (cbs_.on_user)
							cbs_.on_user(v);
					}
					continue;
				}
				bool is_listener = false;
				is_listener = listeners_.find(fd) != listeners_.end();
				if (is_listener && (re & POLLIN)) {
					while (true) {
						sockaddr_storage ss{};
						socklen_t slen = sizeof(ss);
						socket_t client = ::accept(fd, (sockaddr *)&ss, &slen);
						if (client < 0) {
							if (errno == EAGAIN || errno == EWOULDBLOCK)
								break;
							else {
								IO_LOG_ERR("accept(listen fd=%d) failed errno=%d (%s)", fd, errno,
										   std::strerror(errno));
								break;
							}
						}
						if (max_conn_ > 0 && cur_conn_.load() >= max_conn_) {
							::close(client);
							continue;
						}
						int fl = fcntl(client, F_GETFL, 0);
						if (fl < 0)
							fl = 0;
						fcntl(client, F_SETFL, fl | O_NONBLOCK);
						// Предрегистрация клиента в sockets_ с пустым буфером до add_socket
						{
							SockState st{}; st.buf=nullptr; st.buf_size=0; st.read_cb = ReadCallback{};
							sockets_.emplace((socket_t)client, std::move(st));
						}
						cur_conn_++;
						IO_LOG_DBG("accept: client fd=%d", (int)client);
						if (cbs_.on_accept)
							cbs_.on_accept(client);
					}
					continue;
				}
				if (re & POLLOUT) {
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
							if (cbs_.on_accept)
								cbs_.on_accept(fd);
							// drop POLLOUT, keep read interest
							add_poll(fd, POLLIN | POLLHUP);
						} else {
							if (cbs_.on_close)
								cbs_.on_close(fd);
							::close(fd);
							sockets_.erase(fd);
							continue;
						}
					}
				}
				if (re & (POLLIN | POLLRDNORM)) {
					auto it = sockets_.find(fd);
					if (it == sockets_.end()) {
						continue;
					}
					auto st = it->second;
					bool paused = st.paused;
					if (paused)
						continue;
					if (!st.buf || st.buf_size == 0 || !st.read_cb)
						continue;
					ssize_t rn = ::recv(fd, st.buf, st.buf_size, 0);
					if (rn > 0) {
						st.read_cb(fd, st.buf, (size_t)rn);
						// Rearm timer deadline
						auto itT = timers_.find(fd);
						if (itT != timers_.end()) { uint32_t ms = itT->second.ms; itT->second.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms); itT->second.active = true; }
					} else if (rn == 0 || (re & (POLLHUP | POLLERR))) {
						// peer closed or error: remove from poll set and close fd
						rem_poll(fd);
						if (cbs_.on_close)
							cbs_.on_close(fd);
						::close(fd);
						sockets_.erase(fd);
						if (cur_conn_ > 0)
							cur_conn_--;
						timers_.erase(fd);
						timeouts_ms_.erase(fd);
					}
				}
				if (re & POLLOUT) {
					auto it = sockets_.find(fd);
					if (it != sockets_.end() && !it->second.out_queue.empty()) {
						auto &st = it->second;
						auto *p = st.out_queue.data();
						size_t sz = st.out_queue.size();
						ssize_t wn = ::send(fd, p, sz, 0);
						if (wn > 0) {
							if (cbs_.on_write)
								cbs_.on_write(fd, (size_t)wn);
							st.out_queue.erase(st.out_queue.begin(), st.out_queue.begin() + wn);
						} else if (wn < 0) {
							int e = errno;
							if (e == EPIPE || e == ECONNRESET) {
								io::record_broken_pipe();
								IO_LOG_DBG("write: fd=%d EPIPE/ECONNRESET -> drop OUT, want_write=0", fd);
								// Drop OUT and clear want_write quickly
								short mask = POLLHUP;
								if (!st.paused) mask |= POLLIN | POLLRDNORM;
								add_poll(fd, mask);
								st.want_write = false;
							}
						}
						if (st.out_queue.empty()) {
							short mask = POLLHUP;
							if (!st.paused)
								mask |= POLLIN;
							add_poll(fd, mask);
							st.want_write = false;
						}
					}
				}
			}
		close_expired(std::chrono::steady_clock::now());
		return true;
	}

	void event_loop(std::atomic<bool> &run_flag, int32_t wait_ms) override {
		while (run_flag.load(std::memory_order_relaxed)) {
			(void)loop_once((wait_ms < 0) ? 0xFFFFFFFFu : (uint32_t)wait_ms);
		}
	}
};


INetEngine *create_engine_devpoll() {
	return new DevPollEngine();
}

} // namespace io

#endif
