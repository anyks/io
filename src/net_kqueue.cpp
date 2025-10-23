#include "io/net.hpp"
#if defined(IO_ENGINE_KQUEUE)
#include <arpa/inet.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Logging helpers: always provide impls; control emission via macros
static inline void io_kq_log_err_impl(const char *fmt, ...) {
	std::fprintf(stderr, "[io/kqueue][ERR] ");
	va_list args;
	va_start(args, fmt);
	std::vfprintf(stderr, fmt, args);
	va_end(args);
	std::fprintf(stderr, "\n");
}
static inline void io_kq_log_dbg_impl(const char *fmt, ...) {
	std::fprintf(stderr, "[io/kqueue][DBG] ");
	va_list args;
	va_start(args, fmt);
	std::vfprintf(stderr, fmt, args);
	va_end(args);
	std::fprintf(stderr, "\n");
}

// Error logs: enabled in non-NDEBUG builds by default
#ifndef NDEBUG
#define IO_LOG_ERR(...) io_kq_log_err_impl(__VA_ARGS__)
#else
#define IO_LOG_ERR(...) ((void)0)
#endif

// Debug logs: gated by IO_ENABLE_KQUEUE_VERBOSE flag (independent of NDEBUG)
#ifdef IO_ENABLE_KQUEUE_VERBOSE
#define IO_LOG_DBG(...) io_kq_log_dbg_impl(__VA_ARGS__)
#else
#define IO_LOG_DBG(...) ((void)0)
#endif

namespace io {

class KqueueEngine : public INetEngine {
  public:
	KqueueEngine() = default;
	~KqueueEngine() override {
		destroy();
	}

	bool loop_once(uint32_t timeout_ms) override;

	// Метрики
	NetStats get_stats() override;
	void reset_stats() override;

	bool init(const NetCallbacks &cbs) override;
	void destroy() override;
	bool add_socket(socket_t socket, char *buffer, size_t buffer_size, ReadCallback cb) override;
	bool delete_socket(socket_t socket) override;
	bool connect(socket_t socket, const char *host, uint16_t port, bool async) override;
	bool disconnect(socket_t socket) override;
	bool accept(socket_t listen_socket, bool async, uint32_t max_connections) override;
	bool write(socket_t socket, const char *data, size_t data_size) override;
	bool post(uint32_t user_event_value) override;
	bool set_read_timeout(socket_t socket, uint32_t timeout_ms) override;
	bool pause_read(socket_t socket) override;
	bool resume_read(socket_t socket) override;
	bool set_accept_depth(socket_t listen_socket, uint32_t depth) override {
		(void)listen_socket;
		(void)depth;
		return true;
	}
	bool set_accept_depth_ex(socket_t listen_socket, uint32_t depth, bool aggressive_cancel) override {
		(void)aggressive_cancel;
		return set_accept_depth(listen_socket, depth);
	}
	bool set_accept_autotune(socket_t listen_socket, const AcceptAutotuneConfig &cfg) override {
		(void)listen_socket;
		(void)cfg;
		return true;
	}
	// Основной цикл ожидания событий kqueue без внутренних потоков
	void event_loop(std::atomic<bool> &run_flag, int32_t wait_ms) override;

	void wake() override {
		// Разбудить kevent, используя pipe или EVFILT_USER
		if (user_pipe_[1] != -1) {
			uint32_t v = 0;
			(void)::write(user_pipe_[1], &v, sizeof(v));
			stats_.wakes_posted.fetch_add(1, std::memory_order_relaxed);
		}
		struct kevent trig{};
		EV_SET(&trig, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
		(void)kevent(kq_, &trig, 1, nullptr, 0, nullptr);
	}

  private:
	int kq_ = -1;
	NetCallbacks cbs_{};
	int user_pipe_[2]{-1, -1};
	struct SockState {
		char *buf{nullptr};
		size_t buf_size{0};
		ReadCallback read_cb;
		std::vector<char> out_queue;
		bool want_write{false};
		bool connecting{false};
		bool paused{false};
	};
	std::unordered_map<socket_t, SockState> sockets_;
	std::unordered_set<socket_t> listeners_;
	std::unordered_map<socket_t, uint32_t> timeouts_ms_; // per-socket read idle timeouts
	std::atomic<uint32_t> max_conn_{0};
	std::atomic<uint32_t> cur_conn_{0};
	std::deque<uint32_t> userq_;
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
};
INetEngine *create_engine_kqueue() {
	return new KqueueEngine();
}

bool KqueueEngine::init(const NetCallbacks &cbs) {
	cbs_ = cbs;
	// Suppress SIGPIPE (POSIX) once per process
	io::suppress_sigpipe_once();
	kq_ = kqueue();
	if (kq_ == -1)
		return false;
	if (pipe(user_pipe_) == 0) {
		int f0 = fcntl(user_pipe_[0], F_GETFL, 0);
		fcntl(user_pipe_[0], F_SETFL, f0 | O_NONBLOCK);
		int f1 = fcntl(user_pipe_[1], F_GETFL, 0);
		fcntl(user_pipe_[1], F_SETFL, f1 | O_NONBLOCK);
		struct kevent ev{};
		EV_SET(&ev, user_pipe_[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
		kevent(kq_, &ev, 1, nullptr, 0, nullptr);
	}
	{
		struct kevent uev{};
		EV_SET(&uev, 1, EVFILT_USER, EV_ADD | EV_ENABLE, 0, 0, nullptr);
		kevent(kq_, &uev, 1, nullptr, 0, nullptr);
	}
	return true;
}

void KqueueEngine::destroy() {
	if (kq_ != -1) {
		// Best-effort cleanup of all tracked sockets/listeners and timers
		{
			for (auto &kv : sockets_) {
				socket_t fd = kv.first;
				struct kevent ev{};
				EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
				(void)kevent(kq_, &ev, 1, nullptr, 0, nullptr);
				EV_SET(&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
				(void)kevent(kq_, &ev, 1, nullptr, 0, nullptr);
				EV_SET(&ev, fd, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
				(void)kevent(kq_, &ev, 1, nullptr, 0, nullptr);
				if (cbs_.on_close)
					cbs_.on_close(fd);
				::shutdown(fd, SHUT_RDWR);
				::close(fd);
			}
			sockets_.clear();
			listeners_.clear();
			timeouts_ms_.clear();
		}
		if (user_pipe_[0] != -1)
			close(user_pipe_[0]);
		if (user_pipe_[1] != -1)
			close(user_pipe_[1]);
		close(kq_);
		kq_ = -1;
	}
}

bool KqueueEngine::loop_once(uint32_t timeout_ms) {
	if (kq_ == -1) return false;
	constexpr int MAX_EVENTS = 64;
	std::vector<struct kevent> events(MAX_EVENTS);
	struct timespec ts{}; struct timespec *pts = nullptr;
	if (timeout_ms == 0xFFFFFFFFu) {
		pts = nullptr; // infinite
	} else if (timeout_ms == 0) {
		ts.tv_sec = 0; ts.tv_nsec = 0; pts = &ts; // non-blocking
	} else {
		ts.tv_sec = timeout_ms / 1000; ts.tv_nsec = (timeout_ms % 1000) * 1000000L; pts = &ts;
	}
	int nev = kevent(kq_, nullptr, 0, events.data(), MAX_EVENTS, pts);
	if (nev < 0) {
		IO_LOG_ERR("kevent(wait) in loop_once failed errno=%d (%s)", errno, std::strerror(errno));
		return false;
	}
	if (nev == 0) return false;
	IO_LOG_DBG("loop_once: got %d event(s)", nev);
	for (int i = 0; i < nev; ++i) {
		auto &ev = events[i];
		socket_t fd = static_cast<socket_t>(ev.ident);
		IO_LOG_DBG("event fd=%d filter=%d flags=0x%x fflags=0x%x data=%lld", (int)fd, ev.filter, ev.flags, ev.fflags, (long long)ev.data);
		if (ev.filter == EVFILT_READ && fd == user_pipe_[0]) {
			uint32_t val;
			while (::read(user_pipe_[0], &val, sizeof(val)) == sizeof(val)) {
				if (cbs_.on_user) cbs_.on_user(val);
			}
			continue;
		}
		if (ev.filter == EVFILT_TIMER) {
			socket_t fd_to_close = static_cast<socket_t>(reinterpret_cast<intptr_t>(ev.udata));
			if (sockets_.find(fd_to_close) != sockets_.end()) {
				if (cbs_.on_close) cbs_.on_close(fd_to_close);
				::close(fd_to_close);
				{
					auto itS = sockets_.find(fd_to_close);
					if (itS != sockets_.end()) {
						size_t pend = itS->second.out_queue.size();
						if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
						sockets_.erase(itS);
					}
				}
				if (cur_conn_ > 0) cur_conn_--;
				timeouts_ms_.erase(fd_to_close);
				stats_.timeouts.fetch_add(1, std::memory_order_relaxed);
				stats_.closes.fetch_add(1, std::memory_order_relaxed);
			}
			continue;
		}
		if (ev.filter == EVFILT_USER && ev.ident == 1) {
			while (!userq_.empty()) {
				uint32_t v = userq_.front(); userq_.pop_front();
				if (cbs_.on_user) cbs_.on_user(v);
			}
			continue;
		}
		if (ev.filter == EVFILT_READ) {
			bool is_listener = (listeners_.find(fd) != listeners_.end());
			if (is_listener) {
				while (true) {
					sockaddr_storage ss{}; socklen_t slen = sizeof(ss);
					socket_t client = ::accept(fd, (sockaddr *)&ss, &slen);
					if (client < 0) { if (errno==EAGAIN || errno==EWOULDBLOCK) break; else { stats_.accepts_fail.fetch_add(1, std::memory_order_relaxed); break; } }
					if (max_conn_>0 && cur_conn_.load()>=max_conn_) { ::close(client); continue; }
					int fl = fcntl(client, F_GETFL, 0); if (fl<0) fl=0; fcntl(client, F_SETFL, fl|O_NONBLOCK);
					SockState st{}; sockets_.emplace(client, std::move(st)); cur_conn_++;
					{
						auto cur = (uint64_t)cur_conn_.load(std::memory_order_relaxed);
						uint64_t prev = stats_.peak_connections.load(std::memory_order_relaxed);
						while (cur > prev && !stats_.peak_connections.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {}
					}
					stats_.accepts_ok.fetch_add(1, std::memory_order_relaxed);
					IO_LOG_DBG("accept: client fd=%d", (int)client);
					if (cbs_.on_accept) cbs_.on_accept(client);
				}
				continue;
			}
			auto it = sockets_.find(fd); if (it == sockets_.end()) continue;
			auto &st = it->second;
			if (st.paused || !st.buf || st.buf_size==0 || !st.read_cb) continue;
			ssize_t n = ::recv(fd, st.buf, st.buf_size, 0);
			if (n > 0) {
				st.read_cb(fd, st.buf, (size_t)n);
				stats_.reads.fetch_add(1, std::memory_order_relaxed);
				stats_.bytes_read.fetch_add((uint64_t)n, std::memory_order_relaxed);
				auto itT = timeouts_ms_.find(fd);
				if (itT != timeouts_ms_.end() && itT->second > 0) {
					struct kevent tev{};
					EV_SET(&tev, fd, EVFILT_TIMER, EV_ADD | EV_ENABLE, NOTE_USECONDS,
						   (int64_t)itT->second * 1000, (void *)(intptr_t)fd);
					(void)kevent(kq_, &tev, 1, nullptr, 0, nullptr);
				}
			} else if (n == 0) {
				if (cbs_.on_close) cbs_.on_close(fd);
				struct kevent dv{}; EV_SET(&dv, fd, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
				(void)kevent(kq_, &dv, 1, nullptr, 0, nullptr);
				{
					auto itS = sockets_.find(fd);
					if (itS != sockets_.end()) {
						size_t pend = itS->second.out_queue.size();
						if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
						sockets_.erase(itS);
					}
				}
				timeouts_ms_.erase(fd); if (cur_conn_>0) cur_conn_--; 
				stats_.closes.fetch_add(1, std::memory_order_relaxed);
			} else {
				IO_LOG_ERR("recv(fd=%d) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno));
			}
		} else if (ev.filter == EVFILT_WRITE) {
			auto it = sockets_.find(fd); if (it == sockets_.end()) continue;
			auto &st = it->second;
			if (st.connecting) {
				int err=0; socklen_t len=sizeof(err); int gs=::getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len);
				if (gs<0) { err=errno; IO_LOG_ERR("getsockopt(SO_ERROR fd=%d) failed errno=%d (%s)", (int)fd, err, std::strerror(err)); }
				else { IO_LOG_DBG("connect completion fd=%d, SO_ERROR=%d (%s)", (int)fd, err, std::strerror(err)); }
				st.connecting=false;
				if (err==0) {
					struct kevent rev{}; EV_SET(&rev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
					::kevent(kq_, &rev, 1, nullptr, 0, nullptr);
					if (cbs_.on_accept) cbs_.on_accept(fd);
					stats_.connects_ok.fetch_add(1, std::memory_order_relaxed);
				} else {
					if (cbs_.on_close) cbs_.on_close(fd);
					::close(fd);
					struct kevent dv{}; EV_SET(&dv, fd, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
					(void)kevent(kq_, &dv, 1, nullptr, 0, nullptr);
					{
						auto itS = sockets_.find(fd);
						if (itS != sockets_.end()) {
							size_t pend = itS->second.out_queue.size();
							if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
							sockets_.erase(itS);
						}
					}
					timeouts_ms_.erase(fd);
					stats_.connects_fail.fetch_add(1, std::memory_order_relaxed);
					stats_.closes.fetch_add(1, std::memory_order_relaxed);
				}
				continue;
			}
			if (!st.out_queue.empty()) {
				ssize_t n = ::send(fd, st.out_queue.data(), st.out_queue.size(), 0);
				if (n < 0) {
					int e = errno;
					IO_LOG_ERR("send(fd=%d) failed errno=%d (%s)", (int)fd, e, std::strerror(e));
					if (e==EPIPE || e==ECONNRESET) {
						io::record_broken_pipe();
						struct kevent wev{}; EV_SET(&wev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
						(void)kevent(kq_, &wev, 1, nullptr, 0, nullptr);
						st.want_write=false;
					}
				}
				if (n > 0) {
					if (cbs_.on_write) cbs_.on_write(fd, (size_t)n);
					stats_.writes.fetch_add(1, std::memory_order_relaxed);
					stats_.bytes_written.fetch_add((uint64_t)n, std::memory_order_relaxed);
					stats_.send_dequeued_bytes.fetch_add((uint64_t)n, std::memory_order_relaxed);
					st.out_queue.erase(st.out_queue.begin(), st.out_queue.begin() + n);
				}
			}
			if (st.out_queue.empty()) {
				struct kevent wev{}; EV_SET(&wev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
				(void)kevent(kq_, &wev, 1, nullptr, 0, nullptr);
				st.want_write=false;
			}
		}
	}
	return true;
}

bool KqueueEngine::add_socket(socket_t socket, char *buffer, size_t buffer_size, ReadCallback cb) {
	int flags = fcntl(socket, F_GETFL, 0);
	fcntl(socket, F_SETFL, flags | O_NONBLOCK);
	sockets_[socket] = SockState{buffer, buffer_size, std::move(cb), {}, false};
	struct kevent ev{};
	EV_SET(&ev, socket, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
	if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
		IO_LOG_ERR("kevent(ADD read fd=%d) failed errno=%d (%s)", (int)socket, errno, std::strerror(errno));
	}
	IO_LOG_DBG("add_socket: fd=%d", (int)socket);
	return true;
}

bool KqueueEngine::delete_socket(socket_t socket) {
	struct kevent ev{};
	EV_SET(&ev, socket, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
	if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
		IO_LOG_ERR("kevent(DEL read fd=%d) failed errno=%d (%s)", (int)socket, errno, std::strerror(errno));
	}
	EV_SET(&ev, socket, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
	if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
		IO_LOG_ERR("kevent(DEL write fd=%d) failed errno=%d (%s)", (int)socket, errno, std::strerror(errno));
	}
	EV_SET(&ev, socket, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
	if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
		IO_LOG_ERR("kevent(DEL timer fd=%d) failed errno=%d (%s)", (int)socket, errno, std::strerror(errno));
	}
	{
		auto it = sockets_.find(socket);
		if (it != sockets_.end()) {
			size_t pend = it->second.out_queue.size();
			if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
			sockets_.erase(it);
		}
	}
	timeouts_ms_.erase(socket);
	return true;
}

bool KqueueEngine::connect(socket_t fd, const char *host, uint16_t port, bool async) {
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1)
		return false;
	// Если async=false, временно делаем блокирующим
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		flags = 0;
	if (!async) {
		int nb = flags & O_NONBLOCK;
		if (nb)
			fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	}
	int res = ::connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	int err = (res == 0) ? 0 : errno;
	// Вернуть неблокирующий режим
	int cur = fcntl(fd, F_GETFL, 0);
	if (cur < 0)
		cur = 0;
	fcntl(fd, F_SETFL, cur | O_NONBLOCK);
	if (res == 0 || err == EISCONN) {
		// Подписываемся на чтение и уведомляем
		// Для единообразия залогируем SO_ERROR (обычно 0 при мгновенном успехе)
		int soerr = 0;
		socklen_t sl = sizeof(soerr);
		if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0) {
			IO_LOG_DBG("connect completion fd=%d, SO_ERROR=%d (%s)", (int)fd, soerr, std::strerror(soerr));
		}
		bool paused = false;
		auto itp = sockets_.find(fd);
		if (itp != sockets_.end()) paused = itp->second.paused;
		struct kevent ev{};
		EV_SET(&ev, fd, EVFILT_READ, EV_ADD | (paused ? EV_DISABLE : EV_ENABLE), 0, 0, nullptr);
		(void)::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
		IO_LOG_DBG("connect: established fd=%d", (int)fd);
		if (cbs_.on_accept)
			cbs_.on_accept(fd);
		stats_.connects_ok.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	if (async && (err == EINPROGRESS || err == EALREADY)) {
		IO_LOG_DBG("connect: async pending fd=%d (errno=%d %s)", (int)fd, err, std::strerror(err));
		struct kevent ev{};
		EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
		if (::kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
			IO_LOG_ERR("kevent(ADD write fd=%d) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno));
			return false;
		}
		auto it = sockets_.find(fd);
		if (it == sockets_.end()) {
			SockState st; st.connecting = true; sockets_.emplace(fd, std::move(st));
		} else { it->second.connecting = true; }
		return true;
	}
	IO_LOG_ERR("connect(fd=%d) failed, res=%d, errno=%d (%s)", (int)fd, res, err, std::strerror(err));
	stats_.connects_fail.fetch_add(1, std::memory_order_relaxed);
	return false;
}

bool KqueueEngine::disconnect(socket_t fd) {
	if (fd >= 0) {
		shutdown(fd, SHUT_RDWR);
		close(fd);
		IO_LOG_DBG("close: fd=%d", (int)fd);
		if (cbs_.on_close)
			cbs_.on_close(fd);
		stats_.closes.fetch_add(1, std::memory_order_relaxed);
	}
	return true;
}

bool KqueueEngine::accept(socket_t listen_socket, bool async, uint32_t max_connections) {
	if (async) {
		int flags = fcntl(listen_socket, F_GETFL, 0);
		fcntl(listen_socket, F_SETFL, flags | O_NONBLOCK);
	}
	max_conn_ = max_connections;
	listeners_.insert(listen_socket);
	struct kevent ev{};
	EV_SET(&ev, listen_socket, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
	if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) != 0) {
		IO_LOG_ERR("kevent(ADD listen fd=%d) failed errno=%d (%s)", (int)listen_socket, errno, std::strerror(errno));
		return false;
	}
	return true;
}

bool KqueueEngine::write(socket_t fd, const char *data, size_t data_size) {
	auto it = sockets_.find(fd);
	if (it == sockets_.end())
		return false;
	auto &st = it->second;
	st.out_queue.insert(st.out_queue.end(), data, data + data_size);
	stats_.send_enqueued_bytes.fetch_add((uint64_t)data_size, std::memory_order_relaxed);
	if (!st.want_write) {
		struct kevent ev{};
		EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
		if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) == 0) {
			st.want_write = true;
		}
	}
	return true;
}

bool KqueueEngine::post(uint32_t user_event_value) {
	if (user_pipe_[1] != -1) {
		ssize_t n = ::write(user_pipe_[1], &user_event_value, sizeof(user_event_value));
		if (n == (ssize_t)sizeof(user_event_value)) { stats_.user_events.fetch_add(1, std::memory_order_relaxed); return true; }
	}
	userq_.push_back(user_event_value);
	struct kevent trig{};
	EV_SET(&trig, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
	(void)kevent(kq_, &trig, 1, nullptr, 0, nullptr);
	stats_.user_events.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void KqueueEngine::event_loop(std::atomic<bool> &run_flag, int32_t wait_ms) {
	constexpr int MAX_EVENTS = 64;
	std::vector<struct kevent> events(MAX_EVENTS);
	while (run_flag.load(std::memory_order_relaxed)) {
		struct timespec ts{};
		struct timespec *pts = nullptr;
		if (wait_ms < 0) {
			pts = nullptr; // infinite wait
		} else if (wait_ms == 0) {
			ts.tv_sec = 0; ts.tv_nsec = 0; pts = &ts; // non-blocking poll
		} else {
			ts.tv_sec = wait_ms / 1000; ts.tv_nsec = (wait_ms % 1000) * 1000000L; pts = &ts;
		}
		int nev = kevent(kq_, nullptr, 0, events.data(), MAX_EVENTS, pts);
		if (nev < 0) {
			if (errno == EINTR)
				continue;
			IO_LOG_ERR("kevent(wait) failed errno=%d (%s)", errno, std::strerror(errno));
			continue;
		}
		if (nev == 0)
			continue;
		for (int i = 0; i < nev; ++i) {
			auto &ev = events[i];
			socket_t fd = static_cast<socket_t>(ev.ident);
			if (ev.filter == EVFILT_READ && fd == user_pipe_[0]) {
				uint32_t val;
				while (::read(user_pipe_[0], &val, sizeof(val)) == sizeof(val)) {
					if (cbs_.on_user)
						cbs_.on_user(val);
				}
				continue;
			}
			if (ev.filter == EVFILT_TIMER) {
				socket_t fd_to_close = static_cast<socket_t>(reinterpret_cast<intptr_t>(ev.udata));
				// Double-check socket still exists
				bool exists = sockets_.find(fd_to_close) != sockets_.end();
				if (exists) {
					if (cbs_.on_close)
						cbs_.on_close(fd_to_close);
					::close(fd_to_close);
					sockets_.erase(fd_to_close);
					if (cur_conn_ > 0)
						cur_conn_--;
					timeouts_ms_.erase(fd_to_close);
				}
				continue;
			}
			if (ev.filter == EVFILT_USER && ev.ident == 1) {
				while (true) {
					uint32_t v = 0;
					if (userq_.empty())
						break;
					v = userq_.front();
					userq_.pop_front();
					if (cbs_.on_user)
						cbs_.on_user(v);
				}
				continue;
			}
			if (ev.filter == EVFILT_READ) {
				bool is_listener = (listeners_.find(fd) != listeners_.end());
				if (is_listener) {
					while (true) {
						sockaddr_storage ss{};
						socklen_t slen = sizeof(ss);
						socket_t client = ::accept(fd, (sockaddr *)&ss, &slen);
						if (client < 0) {
							if (errno == EAGAIN || errno == EWOULDBLOCK)
								break;
							else {
								IO_LOG_ERR("accept(listen fd=%d) failed errno=%d (%s)", (int)fd, errno,
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
							SockState st{};
							st.buf = nullptr;
							st.buf_size = 0;
							st.read_cb = ReadCallback{};
							sockets_.emplace(client, std::move(st));
						}
						cur_conn_++;
						IO_LOG_DBG("accept: client fd=%d", (int)client);
						if (cbs_.on_accept)
							cbs_.on_accept(client);
					}
					continue;
				}
				auto it = sockets_.find(fd);
				if (it == sockets_.end()) continue;
				auto &st = it->second;
				if (st.paused)
					continue; // respect paused even if EVFILT_READ was delivered earlier
				if (!st.buf || st.buf_size == 0 || !st.read_cb)
					continue;
				ssize_t n = recv(fd, st.buf, st.buf_size, 0);
				if (n < 0) {
					IO_LOG_ERR("recv(fd=%d) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno));
				}
				if (n > 0) {
					st.read_cb(fd, st.buf, static_cast<size_t>(n));
				} else if (n == 0) {
					if (cbs_.on_close)
						cbs_.on_close(fd);
					struct kevent dv{};
					EV_SET(&dv, fd, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
					(void)kevent(kq_, &dv, 1, nullptr, 0, nullptr);
					sockets_.erase(fd);
					timeouts_ms_.erase(fd);
					if (cur_conn_ > 0)
						cur_conn_--;
				}
				// Re-arm/refresh per-socket timer if configured
				{
					auto itT = timeouts_ms_.find(fd);
					if (itT != timeouts_ms_.end() && itT->second > 0) {
						struct kevent tev{};
						EV_SET(&tev, fd, EVFILT_TIMER, EV_ADD | EV_ENABLE, NOTE_USECONDS,
							   (int64_t)itT->second * 1000 /* usec */, (void *)(intptr_t)fd);
						(void)kevent(kq_, &tev, 1, nullptr, 0, nullptr);
					}
				}
			} else if (ev.filter == EVFILT_WRITE) {
				auto it = sockets_.find(fd);
				if (it == sockets_.end()) continue;
				auto &st = it->second;
				if (st.connecting) {
					int err = 0;
					socklen_t len = sizeof(err);
					int gs = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
					if (gs < 0) {
						err = errno;
						IO_LOG_ERR("getsockopt(SO_ERROR fd=%d) failed errno=%d (%s)", (int)fd, err, std::strerror(err));
					} else {
						IO_LOG_DBG("connect completion fd=%d, SO_ERROR=%d (%s)", (int)fd, err, std::strerror(err));
					}
					st.connecting = false;
					if (err == 0) {
						struct kevent rev{};
						EV_SET(&rev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
						::kevent(kq_, &rev, 1, nullptr, 0, nullptr);
						if (cbs_.on_accept)
							cbs_.on_accept(fd);
					} else {
						if (cbs_.on_close)
							cbs_.on_close(fd);
						::close(fd);
						struct kevent dv{};
						EV_SET(&dv, fd, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
						(void)kevent(kq_, &dv, 1, nullptr, 0, nullptr);
						sockets_.erase(fd);
						timeouts_ms_.erase(fd);
					}
					continue;
				}
				if (!st.out_queue.empty()) {
					auto data_ptr = st.out_queue.data();
					auto data_len = st.out_queue.size();
					ssize_t n = send(fd, data_ptr, data_len, 0);
					if (n < 0) {
						int e = errno;
						IO_LOG_ERR("send(fd=%d) failed errno=%d (%s)", (int)fd, e, std::strerror(e));
						if (e == EPIPE || e == ECONNRESET) {
							io::record_broken_pipe();
							IO_LOG_DBG("write: fd=%d EPIPE/ECONNRESET -> drop OUT, want_write=0", (int)fd);
							// Drop EVFILT_WRITE and clear want_write to avoid spinning
							struct kevent wev{};
							EV_SET(&wev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
							(void)kevent(kq_, &wev, 1, nullptr, 0, nullptr);
							st.want_write = false;
						}
					}
					if (n > 0) {
						if (cbs_.on_write)
							cbs_.on_write(fd, static_cast<size_t>(n));
						st.out_queue.erase(st.out_queue.begin(), st.out_queue.begin() + n);
					}
				}
				if (st.out_queue.empty()) {
					struct kevent wev{};
					EV_SET(&wev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
					(void)kevent(kq_, &wev, 1, nullptr, 0, nullptr);
					st.want_write = false;
				}
			}
		}
	}
}

bool KqueueEngine::set_read_timeout(socket_t fd, uint32_t timeout_ms) {
	if (timeout_ms == 0) {
		timeouts_ms_.erase(fd);
		struct kevent tev{};
		EV_SET(&tev, fd, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
		(void)kevent(kq_, &tev, 1, nullptr, 0, nullptr);
		return true;
	}
	if (sockets_.find(fd) == sockets_.end() && listeners_.find(fd) == listeners_.end())
		return false;
	timeouts_ms_[fd] = timeout_ms;
	struct kevent tev{};
	EV_SET(&tev, fd, EVFILT_TIMER, EV_ADD | EV_ENABLE, NOTE_USECONDS, (int64_t)timeout_ms * 1000 /* usec */,
		   (void *)(intptr_t)fd);
	return kevent(kq_, &tev, 1, nullptr, 0, nullptr) == 0;
}

bool KqueueEngine::pause_read(socket_t socket) {
	auto it = sockets_.find(socket);
	if (it != sockets_.end())
		it->second.paused = true;
	struct kevent ev{};
	EV_SET(&ev, socket, EVFILT_READ, EV_DISABLE, 0, 0, nullptr);
	if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
		IO_LOG_ERR("kevent(EV_DISABLE fd=%d) failed errno=%d (%s)", (int)socket, errno, std::strerror(errno));
		return false;
	}
	return true;
}

bool KqueueEngine::resume_read(socket_t socket) {
	auto it = sockets_.find(socket);
	if (it != sockets_.end())
		it->second.paused = false;
	struct kevent ev{};
	EV_SET(&ev, socket, EVFILT_READ, EV_ENABLE, 0, 0, nullptr);
	if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
		IO_LOG_ERR("kevent(EV_ENABLE fd=%d) failed errno=%d (%s)", (int)socket, errno, std::strerror(errno));
		return false;
	}
	return true;
}

NetStats KqueueEngine::get_stats() {
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

void KqueueEngine::reset_stats() {
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
