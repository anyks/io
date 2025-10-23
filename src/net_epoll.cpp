#include "io/net.hpp"
#if defined(IO_ENGINE_EPOLL)
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <vector>

// Logging macros: errors in Debug, debug gated by IO_ENABLE_EPOLL_VERBOSE
#ifndef NDEBUG
#define IO_LOG_ERR(...)                                                                                                 \
    do {                                                                                                                \
        std::fprintf(stderr, "[io/epoll][ERR] ");                                                                       \
        std::fprintf(stderr, __VA_ARGS__);                                                                              \
        std::fprintf(stderr, "\n");                                                                                     \
    } while (0)
#else
#define IO_LOG_ERR(...) ((void)0)
#endif

#ifdef IO_ENABLE_EPOLL_VERBOSE
#define IO_LOG_DBG(...)                                                                                                 \
    do {                                                                                                                \
        std::fprintf(stderr, "[io/epoll][DBG] ");                                                                       \
        std::fprintf(stderr, __VA_ARGS__);                                                                              \
        std::fprintf(stderr, "\n");                                                                                     \
    } while (0)
#else
#define IO_LOG_DBG(...) ((void)0)
#endif

namespace io {

class EpollEngine : public INetEngine {
  public:
	EpollEngine() = default;
	~EpollEngine() override {
		destroy();
	}

	// Метрики
	NetStats get_stats() override;
	void reset_stats() override;

	bool loop_once(uint32_t timeout_ms) override {
		if (ep_ == -1) return false;
		constexpr int MAXE = 64;
		std::vector<epoll_event> evs(MAXE);
		int to = (timeout_ms == 0xFFFFFFFFu) ? -1 : (int)timeout_ms;
		int n = ::epoll_wait(ep_, evs.data(), MAXE, to);
		if (n <= 0) return false;
		for (int i = 0; i < n; ++i) {
			int fd = evs[i].data.fd;
			uint32_t events = evs[i].events;
			// timerfd fired
			if (owner_.find(fd) != owner_.end() && (events & EPOLLIN)) {
				uint64_t exp; while (::read(fd, &exp, sizeof(exp)) == sizeof(exp)) {}
				socket_t sfd = owner_[fd];
				if (sfd != -1) {
					if (cbs_.on_close) cbs_.on_close(sfd);
					(void)::epoll_ctl(ep_, EPOLL_CTL_DEL, sfd, nullptr);
					::shutdown(sfd, SHUT_RDWR); ::close(sfd);
					bool dec=false; {
						auto itS = sockets_.find(sfd);
						if (itS != sockets_.end()) {
							// учтём дроп по очереди отправки
							size_t pend = (itS->second.out_queue.size() > itS->second.out_head)
										? (itS->second.out_queue.size() - itS->second.out_head)
										: 0;
							if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
							dec = itS->second.server_side;
							sockets_.erase(itS);
						}
					}
					if (dec && cur_conn_>0) cur_conn_--;
					auto itT = timers_.find(sfd);
					if (itT != timers_.end()) { ::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr); ::close(fd); owner_.erase(fd); timers_.erase(itT); }
					timeouts_ms_.erase(sfd);
					stats_.timeouts.fetch_add(1, std::memory_order_relaxed);
					stats_.closes.fetch_add(1, std::memory_order_relaxed);
				}
				continue;
			}
			// user pipe
			if ((events & EPOLLIN) && fd == user_pipe_[0]) {
				// Drain the pipe (edge-triggered) and deliver queued user values
				uint8_t tmp[256];
				while (::read(user_pipe_[0], tmp, sizeof(tmp)) > 0) {}
				std::deque<uint32_t> pending;
				{
					std::lock_guard<std::mutex> lk(user_mtx_);
					pending.swap(user_vals_);
				}
				for (auto v : pending) {
					if (cbs_.on_user) cbs_.on_user(v);
				}
				continue;
			}
			bool is_listener = (listeners_.find(fd) != listeners_.end());
			if (is_listener && (events & EPOLLIN)) {
				while (true) {
					sockaddr_storage ss{}; socklen_t slen = sizeof(ss);
					socket_t client = ::accept(fd, (sockaddr*)&ss, &slen);
					if (client < 0) { if (errno==EAGAIN || errno==EWOULDBLOCK) break; else { IO_LOG_ERR("accept(listen fd=%d) failed errno=%d (%s)", fd, errno, std::strerror(errno)); stats_.accepts_fail.fetch_add(1, std::memory_order_relaxed); break; } }
					if (max_conn_>0 && cur_conn_.load()>=max_conn_) { ::close(client); continue; }
					int fl = fcntl(client, F_GETFL, 0); if (fl<0) fl=0; fcntl(client, F_SETFL, fl|O_NONBLOCK);
					epoll_event cev{}; cev.events = EPOLLIN | EPOLLRDHUP; cev.data.fd = client;
					if (::epoll_ctl(ep_, EPOLL_CTL_ADD, client, &cev) != 0) { IO_LOG_ERR("epoll_ctl(ADD client fd=%d) failed errno=%d (%s)", (int)client, errno, std::strerror(errno)); ::close(client); continue; }
					SockState st{}; st.active=true; st.server_side=true; sockets_.emplace(client, std::move(st));
					cur_conn_++;
					// peak
					auto cur = (uint64_t)cur_conn_.load(std::memory_order_relaxed);
					uint64_t prev = stats_.peak_connections.load(std::memory_order_relaxed);
					while (cur > prev && !stats_.peak_connections.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {}
					stats_.accepts_ok.fetch_add(1, std::memory_order_relaxed);
					IO_LOG_DBG("accept: client fd=%d", (int)client); if (cbs_.on_accept) cbs_.on_accept(client);
				}
				continue;
			}
			if (events & EPOLLOUT) {
				auto it = sockets_.find(fd);
				if (it != sockets_.end() && it->second.connecting) {
					int err=0; socklen_t len=sizeof(err); int gs=::getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len);
					if (gs<0) { err=errno; IO_LOG_ERR("getsockopt(SO_ERROR fd=%d) failed errno=%d (%s)", fd, err, std::strerror(err)); }
					else { IO_LOG_DBG("connect completion fd=%d, SO_ERROR=%d (%s)", fd, err, std::strerror(err)); }
					it->second.connecting=false; if (err==0) it->second.active=true;
					if (err==0) {
						IO_LOG_DBG("connect: established fd=%d", fd);
						stats_.connects_ok.fetch_add(1, std::memory_order_relaxed);
						if (cbs_.on_accept) cbs_.on_accept(fd);
						epoll_event ev{}; bool paused = it->second.paused; ev.events = (paused?0u:EPOLLIN) | EPOLLRDHUP; ev.data.fd = fd; (void)::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
					} else {
						if (cbs_.on_close) cbs_.on_close(fd);
						(void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr); ::close(fd); sockets_.erase(fd); continue;
						stats_.connects_fail.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
			if (events & EPOLLOUT) {
				auto it = sockets_.find(fd);
				if (it != sockets_.end() && !it->second.out_queue.empty()) {
					auto &st = it->second; const char *p = st.out_queue.data()+st.out_head; size_t sz = st.out_queue.size()-st.out_head;
					ssize_t wn = ::send(fd, p, sz, 0);
					if (wn < 0) {
						int e = errno;
						if (!(e==EAGAIN || e==EWOULDBLOCK || e==EINTR)) IO_LOG_ERR("send(fd=%d) failed errno=%d (%s)", fd, e, std::strerror(e));
						if (e == EPIPE || e == ECONNRESET) {
							io::record_broken_pipe(); IO_LOG_DBG("write: fd=%d EPIPE/ECONNRESET -> closing socket", fd);
							if (cbs_.on_close) cbs_.on_close(fd);
							(void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr); ::shutdown(fd, SHUT_RDWR); ::close(fd);
							bool dec5=false; { auto itE=sockets_.find(fd); if (itE!=sockets_.end()) { size_t pend=(itE->second.out_queue.size()>itE->second.out_head)?(itE->second.out_queue.size()-itE->second.out_head):0; if(pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed); dec5=itE->second.server_side; sockets_.erase(itE);} }
							if (dec5 && cur_conn_>0) { cur_conn_--; }
							auto itT=timers_.find(fd);
							if (itT!=timers_.end()) { int tfd=itT->second; (void)::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr); ::close(tfd); owner_.erase(tfd); timers_.erase(itT);} 
							timeouts_ms_.erase(fd);
							stats_.closes.fetch_add(1, std::memory_order_relaxed);
							continue;
						}
					}
					if (wn > 0) { if (cbs_.on_write) cbs_.on_write(fd, (size_t)wn); stats_.writes.fetch_add(1, std::memory_order_relaxed); stats_.bytes_written.fetch_add((uint64_t)wn, std::memory_order_relaxed); stats_.send_dequeued_bytes.fetch_add((uint64_t)wn, std::memory_order_relaxed); st.out_head += (size_t)wn; if (st.out_head == st.out_queue.size()) { st.out_queue.clear(); st.out_head = 0; } }
					if (st.out_queue.empty() || st.out_head >= st.out_queue.size()) {
						if (!st.out_queue.empty() && st.out_head > 0) { size_t remaining = st.out_queue.size()-st.out_head; if (remaining>0) std::memmove(st.out_queue.data(), st.out_queue.data()+st.out_head, remaining); st.out_queue.resize(remaining); st.out_head=0; }
						epoll_event ev{}; bool paused_now = st.paused; ev.events = (paused_now?0u:EPOLLIN) | EPOLLRDHUP; ev.data.fd=fd; (void)::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
						st.want_write=false;
					}
				}
			}
			if (events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
				auto it = sockets_.find(fd); if (it == sockets_.end()) continue; auto st = it->second;
				if (st.connecting || !st.active) continue;
				bool did_read = false;
				bool paused_now = (sockets_.find(fd) != sockets_.end()) ? sockets_[fd].paused : false;
				if (!paused_now && (events & EPOLLIN) && st.buf && st.buf_size > 0 && st.read_cb) {
					while (true) {
						ssize_t rn = ::recv(fd, st.buf, st.buf_size, 0);
						if (rn < 0) { int e=errno; if (e==EAGAIN || e==EWOULDBLOCK) break; IO_LOG_ERR("recv(fd=%d) failed errno=%d (%s)", fd, e, std::strerror(e)); break; }
						if (rn == 0) { if (cbs_.on_close) cbs_.on_close(fd); (void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr); bool dec=false; { auto itS=sockets_.find(fd); if (itS!=sockets_.end()) { size_t pend=(itS->second.out_queue.size()>itS->second.out_head)?(itS->second.out_queue.size()-itS->second.out_head):0; if(pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed); dec=itS->second.server_side; sockets_.erase(itS);} } if (dec && cur_conn_>0) cur_conn_--; auto itT = timers_.find(fd); if (itT != timers_.end()) { int tfd=itT->second; ::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr); ::close(tfd); owner_.erase(tfd); timers_.erase(itT);} timeouts_ms_.erase(fd); stats_.closes.fetch_add(1, std::memory_order_relaxed); break; }
						st.read_cb(fd, st.buf, (size_t)rn);
						stats_.reads.fetch_add(1, std::memory_order_relaxed);
						stats_.bytes_read.fetch_add((uint64_t)rn, std::memory_order_relaxed);
						did_read = true;
						int tfd=-1; uint32_t ms=0; auto itT = timers_.find(fd); if (itT != timers_.end()) { tfd=itT->second; ms=timeouts_ms_[fd]; }
						if (tfd != -1 && ms > 0) { itimerspec its{}; its.it_value.tv_sec = ms/1000; its.it_value.tv_nsec = (ms%1000) * 1000000L; (void)::timerfd_settime(tfd, 0, &its, nullptr); }
					}
				}
				if (!did_read && (events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))) {
					bool fatal=false; if (events & EPOLLERR) { int soerr=0; socklen_t sl=sizeof(soerr); if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl)==0 && soerr!=0) fatal=true; } if (events & (EPOLLHUP | EPOLLRDHUP)) fatal=true;
					if (fatal) { if (cbs_.on_close) cbs_.on_close(fd); (void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr); ::shutdown(fd, SHUT_RDWR); ::close(fd); bool dec=false; { auto itS=sockets_.find(fd); if (itS!=sockets_.end()) { size_t pend=(itS->second.out_queue.size()>itS->second.out_head)?(itS->second.out_queue.size()-itS->second.out_head):0; if(pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed); dec=itS->second.server_side; sockets_.erase(itS);} } if (dec && cur_conn_>0) cur_conn_--; auto itT=timers_.find(fd); if (itT != timers_.end()) { int tfd=itT->second; ::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr); ::close(tfd); owner_.erase(tfd); timers_.erase(itT);} timeouts_ms_.erase(fd); stats_.closes.fetch_add(1, std::memory_order_relaxed); }
				}
			}
		}
		return true;
	}

	bool init(const NetCallbacks &cbs) override {
		cbs_ = cbs;
		io::suppress_sigpipe_once();
		ep_ = ::epoll_create1(EPOLL_CLOEXEC);
		if (ep_ == -1) return false;
		// user pipe for wake()/post()
		if (::pipe(user_pipe_) == 0) {
			int f0 = fcntl(user_pipe_[0], F_GETFL, 0); if (f0 < 0) f0 = 0;
			fcntl(user_pipe_[0], F_SETFL, f0 | O_NONBLOCK);
			int f1 = fcntl(user_pipe_[1], F_GETFL, 0); if (f1 < 0) f1 = 0;
			fcntl(user_pipe_[1], F_SETFL, f1 | O_NONBLOCK);
			epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = user_pipe_[0];
			(void)::epoll_ctl(ep_, EPOLL_CTL_ADD, user_pipe_[0], &ev);
		}
		return true;
	}

	// Основной цикл обработки событий epoll. Ждёт по таймауту и проверяет run_flag.
	void event_loop(std::atomic<bool> &run_flag, int32_t wait_ms) override {
		if (ep_ == -1) return;
		constexpr int MAXE = 64;
		std::vector<epoll_event> evs(MAXE);
		const int base_timeout = (wait_ms < 0) ? -1 : (int)wait_ms;
		while (run_flag.load(std::memory_order_relaxed)) {
			int n = ::epoll_wait(ep_, evs.data(), MAXE, base_timeout);
			if (n < 0) {
				if (errno == EINTR) continue;
				IO_LOG_ERR("epoll_wait failed errno=%d (%s)", errno, std::strerror(errno));
				continue;
			}
			if (n == 0) continue;
			for (int i = 0; i < n; ++i) {
			int fd = evs[i].data.fd;
			uint32_t events = evs[i].events;
			// timerfd fired?
			if (owner_.find(fd) != owner_.end() && (events & EPOLLIN)) {
				uint64_t exp; while (::read(fd, &exp, sizeof(exp)) == sizeof(exp)) {}
				socket_t sfd = -1; auto it = owner_.find(fd); if (it != owner_.end()) sfd = it->second;
				if (sfd != -1) {
					if (cbs_.on_close) cbs_.on_close(sfd);
					(void)::epoll_ctl(ep_, EPOLL_CTL_DEL, sfd, nullptr);
					::shutdown(sfd, SHUT_RDWR); ::close(sfd);
					bool dec=false; { auto itS = sockets_.find(sfd); if (itS != sockets_.end()) { dec = itS->second.server_side; sockets_.erase(itS);} }
					if (dec && cur_conn_>0) cur_conn_--;
					auto itT = timers_.find(sfd);
					if (itT != timers_.end()) { ::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr); ::close(fd); owner_.erase(fd); timers_.erase(itT); }
					timeouts_ms_.erase(sfd);
				}
				continue;
			}
			// user pipe events
			if ((events & EPOLLIN) && fd == user_pipe_[0]) {
				uint8_t tmp[256];
				while (::read(user_pipe_[0], tmp, sizeof(tmp)) > 0) {}
				std::deque<uint32_t> pending;
				{
					std::lock_guard<std::mutex> lk(user_mtx_);
					pending.swap(user_vals_);
				}
				for (auto v : pending) {
					if (cbs_.on_user) cbs_.on_user(v);
				}
				continue;
			}
			bool is_listener = (listeners_.find(fd) != listeners_.end());
			if (is_listener && (events & EPOLLIN)) {
				while (true) {
					sockaddr_storage ss{}; socklen_t slen = sizeof(ss);
					socket_t client = ::accept(fd, (sockaddr*)&ss, &slen);
					if (client < 0) { if (errno==EAGAIN || errno==EWOULDBLOCK) break; else { IO_LOG_ERR("accept(listen fd=%d) failed errno=%d (%s)", fd, errno, std::strerror(errno)); stats_.accepts_fail.fetch_add(1, std::memory_order_relaxed); break; } }
					if (max_conn_>0 && cur_conn_.load()>=max_conn_) { ::close(client); continue; }
					int fl = fcntl(client, F_GETFL, 0); if (fl<0) fl=0; fcntl(client, F_SETFL, fl|O_NONBLOCK);
					epoll_event cev{}; cev.events = EPOLLIN | EPOLLRDHUP; cev.data.fd = client;
					if (::epoll_ctl(ep_, EPOLL_CTL_ADD, client, &cev) != 0) { IO_LOG_ERR("epoll_ctl(ADD client fd=%d) failed errno=%d (%s)", (int)client, errno, std::strerror(errno)); ::close(client); continue; }
					SockState st{}; st.active=true; st.server_side=true; sockets_.emplace(client, std::move(st));
					cur_conn_++;
					auto cur = (uint64_t)cur_conn_.load(std::memory_order_relaxed);
					uint64_t prev = stats_.peak_connections.load(std::memory_order_relaxed);
					while (cur > prev && !stats_.peak_connections.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {}
					stats_.accepts_ok.fetch_add(1, std::memory_order_relaxed);
					IO_LOG_DBG("accept: client fd=%d", (int)client); if (cbs_.on_accept) cbs_.on_accept(client);
				}
				continue;
			}
			if (events & EPOLLOUT) {
				auto it = sockets_.find(fd);
				if (it != sockets_.end() && it->second.connecting) {
					int err=0; socklen_t len=sizeof(err); int gs=::getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len);
					if (gs<0) { err=errno; IO_LOG_ERR("getsockopt(SO_ERROR fd=%d) failed errno=%d (%s)", fd, err, std::strerror(err)); }
					else { IO_LOG_DBG("connect completion fd=%d, SO_ERROR=%d (%s)", fd, err, std::strerror(err)); }
					it->second.connecting=false; if (err==0) it->second.active=true;
					if (err==0) {
						IO_LOG_DBG("connect: established fd=%d", fd);
						stats_.connects_ok.fetch_add(1, std::memory_order_relaxed);
						if (cbs_.on_accept) cbs_.on_accept(fd);
						epoll_event ev{}; bool paused = it->second.paused; ev.events = (paused?0u:EPOLLIN) | EPOLLRDHUP; ev.data.fd = fd; (void)::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
					} else {
						if (cbs_.on_close) cbs_.on_close(fd);
						(void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr); ::close(fd); sockets_.erase(fd); continue;
						stats_.connects_fail.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
			if (events & EPOLLOUT) {
				auto it = sockets_.find(fd);
				if (it != sockets_.end() && !it->second.out_queue.empty()) {
					auto &st = it->second; const char *p = st.out_queue.data()+st.out_head; size_t sz = st.out_queue.size()-st.out_head;
					ssize_t wn = ::send(fd, p, sz, 0);
					if (wn < 0) {
						int e = errno;
						if (!(e==EAGAIN || e==EWOULDBLOCK || e==EINTR)) IO_LOG_ERR("send(fd=%d) failed errno=%d (%s)", fd, e, std::strerror(e));
						if (e == EPIPE || e == ECONNRESET) {
							io::record_broken_pipe(); IO_LOG_DBG("write: fd=%d EPIPE/ECONNRESET -> closing socket", fd);
							if (cbs_.on_close) cbs_.on_close(fd);
							(void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr); ::shutdown(fd, SHUT_RDWR); ::close(fd);
							bool dec5=false; { auto itE=sockets_.find(fd); if (itE!=sockets_.end()) { dec5=itE->second.server_side; sockets_.erase(itE);} }
							if (dec5 && cur_conn_>0) cur_conn_--; auto itT=timers_.find(fd); if (itT!=timers_.end()) { int tfd=itT->second; (void)::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr); ::close(tfd); owner_.erase(tfd); timers_.erase(itT);} timeouts_ms_.erase(fd);
							continue;
						}
					}
					if (wn > 0) { IO_LOG_DBG("send: fd=%d, wrote=%zd/%zu (head=%zu -> %zu, total=%zu)", fd, wn, sz, st.out_head, st.out_head+(size_t)wn, st.out_queue.size()); if (cbs_.on_write) cbs_.on_write(fd, (size_t)wn); stats_.writes.fetch_add(1, std::memory_order_relaxed); stats_.bytes_written.fetch_add((uint64_t)wn, std::memory_order_relaxed); stats_.send_dequeued_bytes.fetch_add((uint64_t)wn, std::memory_order_relaxed); st.out_head += (size_t)wn; if (st.out_head == st.out_queue.size()) { st.out_queue.clear(); st.out_head = 0; } }
					if (st.out_queue.empty() || st.out_head >= st.out_queue.size()) {
						if (!st.out_queue.empty() && st.out_head > 0) { size_t remaining = st.out_queue.size()-st.out_head; if (remaining>0) std::memmove(st.out_queue.data(), st.out_queue.data()+st.out_head, remaining); st.out_queue.resize(remaining); st.out_head=0; }
						epoll_event ev{}; bool paused_now = st.paused; ev.events = (paused_now?0u:EPOLLIN) | EPOLLRDHUP; ev.data.fd=fd; if (::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev) != 0) { IO_LOG_ERR("epoll_ctl(MOD fd=%d->IN) failed errno=%d (%s)", fd, errno, std::strerror(errno)); }
						st.want_write=false;
					}
				}
			}
			if (events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
				auto it = sockets_.find(fd); if (it == sockets_.end()) continue; auto st = it->second;
				int has_cb = st.read_cb ? 1 : 0; (void)has_cb; // suppress unused when DBG disabled
				IO_LOG_DBG("event fd=%d ev=%s%s%s%s, connecting=%d, active=%d, has_cb=%d, buf=%p, size=%zu",
				           fd,
				           (events & EPOLLIN) ? "IN" : "",
				           (events & EPOLLOUT) ? "|OUT" : "",
				           (events & EPOLLERR) ? "|ERR" : "",
				           (events & (EPOLLHUP | EPOLLRDHUP)) ? "|HUP" : "",
				           st.connecting ? 1 : 0,
				           st.active ? 1 : 0,
				           has_cb,
				           (void *)st.buf,
				           st.buf_size);
				if (st.connecting || !st.active) continue;
				bool did_read = false;
				bool paused_now = (sockets_.find(fd) != sockets_.end()) ? sockets_[fd].paused : false;
				if (!paused_now && (events & EPOLLIN) && st.buf && st.buf_size > 0 && st.read_cb) {
					while (true) {
						ssize_t rn = ::recv(fd, st.buf, st.buf_size, 0);
						if (rn < 0) { int e=errno; if (e==EAGAIN || e==EWOULDBLOCK) break; IO_LOG_ERR("recv(fd=%d) failed errno=%d (%s)", fd, e, std::strerror(e)); break; }
						if (rn == 0) { if (cbs_.on_close) cbs_.on_close(fd); (void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr); bool dec=false; { auto itS=sockets_.find(fd); if (itS!=sockets_.end()) { dec=itS->second.server_side; sockets_.erase(itS);} } if (dec && cur_conn_>0) cur_conn_--; auto itT = timers_.find(fd); if (itT != timers_.end()) { int tfd=itT->second; ::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr); ::close(tfd); owner_.erase(tfd); timers_.erase(itT);} timeouts_ms_.erase(fd); break; }
						IO_LOG_DBG("recv: fd=%d, got=%zd", fd, rn);
						st.read_cb(fd, st.buf, (size_t)rn);
						stats_.reads.fetch_add(1, std::memory_order_relaxed);
						stats_.bytes_read.fetch_add((uint64_t)rn, std::memory_order_relaxed);
						did_read = true;
						int tfd=-1; uint32_t ms=0; auto itT = timers_.find(fd); if (itT != timers_.end()) { tfd=itT->second; ms=timeouts_ms_[fd]; }
						if (tfd != -1 && ms > 0) { itimerspec its{}; its.it_value.tv_sec = ms/1000; its.it_value.tv_nsec = (ms%1000) * 1000000L; if (::timerfd_settime(tfd, 0, &its, nullptr) != 0) { IO_LOG_ERR("timerfd_settime(fd=%d) failed errno=%d (%s)", tfd, errno, std::strerror(errno)); } }
					}
				}
				if (!did_read && (events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))) {
					bool fatal=false; if (events & EPOLLERR) { int soerr=0; socklen_t sl=sizeof(soerr); if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl)==0 && soerr!=0) fatal=true; } if (events & (EPOLLHUP | EPOLLRDHUP)) fatal=true;
					if (fatal) { if (cbs_.on_close) cbs_.on_close(fd); (void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr); ::shutdown(fd, SHUT_RDWR); ::close(fd); bool dec=false; { auto itS=sockets_.find(fd); if (itS!=sockets_.end()) { size_t pend=(itS->second.out_queue.size()>itS->second.out_head)?(itS->second.out_queue.size()-itS->second.out_head):0; if(pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed); dec=itS->second.server_side; sockets_.erase(itS);} } if (dec && cur_conn_>0) cur_conn_--; auto itT=timers_.find(fd); if (itT != timers_.end()) { int tfd=itT->second; ::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr); ::close(tfd); owner_.erase(tfd); timers_.erase(itT);} timeouts_ms_.erase(fd); stats_.closes.fetch_add(1, std::memory_order_relaxed); }
				}
			}
			}
		}
	}
	/* removed duplicate init() */
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
	void destroy() override {
		if (ep_ != -1) {
			// After loop stops, proactively close all tracked sockets and timers
			{
				std::vector<socket_t> to_close;
				to_close.reserve(sockets_.size());
				for (auto &p : sockets_) to_close.push_back(p.first);
				for (socket_t fd : to_close) {
					(void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
					if (cbs_.on_close)
						cbs_.on_close(fd);
					::shutdown(fd, SHUT_RDWR);
					::close(fd);
					IO_LOG_DBG("destroy: closed fd=%d", (int)fd);
					bool dec = false;
					{
						auto itS = sockets_.find(fd);
						if (itS != sockets_.end()) {
							dec = itS->second.server_side;
							sockets_.erase(itS);
						}
					}
					if (dec && cur_conn_ > 0)
						cur_conn_--;
					auto itT = timers_.find(fd);
					if (itT != timers_.end()) {
						int tfd = itT->second;
						(void)::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr);
						::close(tfd);
						owner_.erase(tfd);
						timers_.erase(itT);
					}
					timeouts_ms_.erase(fd);
				}
				// Close and deregister listeners as well
				for (auto lfd : listeners_) {
					( void)::epoll_ctl(ep_, EPOLL_CTL_DEL, lfd, nullptr);
					::shutdown(lfd, SHUT_RDWR);
					::close(lfd);
				}
				listeners_.clear();
			}
			if (user_pipe_[0] != -1)
				::close(user_pipe_[0]);
			if (user_pipe_[1] != -1)
				::close(user_pipe_[1]);
			::close(ep_);
			ep_ = -1;
		}
	}
	bool add_socket(socket_t fd, char *buffer, size_t buffer_size, ReadCallback cb) override {
		int flags = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		{
			// Если запись уже существует (например, connect(async) создал её ранее и установил connecting=true),
			// то нельзя перетирать состояние по умолчанию, иначе потеряем флаги (connecting/want_write/очередь).
			auto it = sockets_.find(fd);
			if (it == sockets_.end()) {
				SockState st;
				st.buf = buffer;
				st.buf_size = buffer_size;
				st.read_cb = std::move(cb);
				// Определим, подключён ли уже сокет (например, серверный accept'нутый), чтобы сделать его активным.
				sockaddr_storage peer{}; socklen_t slen = sizeof(peer);
				if (::getpeername(fd, (sockaddr*)&peer, &slen) == 0) {
					st.active = true;
				}
				sockets_.emplace(fd, std::move(st));
#ifndef NDEBUG
				IO_LOG_DBG("add_socket NEW fd=%d, buf=%p, size=%zu, active=%d", (int)fd, (void *)buffer, buffer_size,
				           sockets_[fd].active ? 1 : 0);
#endif
			} else {
#ifndef NDEBUG
				IO_LOG_DBG(
				    "add_socket UPDATE fd=%d, prev: buf=%p, size=%zu, connecting=%d, want_write=%d, out_q=%zu, active=%d",
				    (int)fd,
				    (void *)it->second.buf,
				    it->second.buf_size,
				    it->second.connecting ? 1 : 0,
				    it->second.want_write ? 1 : 0,
				    it->second.out_queue.size(),
				    it->second.active ? 1 : 0);
#endif
				it->second.buf = buffer;
				it->second.buf_size = buffer_size;
				it->second.read_cb = std::move(cb);
				// connecting/want_write/out_queue сохраняем как есть
#ifndef NDEBUG
				IO_LOG_DBG(
				    "add_socket UPDATED fd=%d, now: buf=%p, size=%zu, connecting=%d, want_write=%d, out_q=%zu, active=%d",
				    (int)fd,
				    (void *)it->second.buf,
				    it->second.buf_size,
				    it->second.connecting ? 1 : 0,
				    it->second.want_write ? 1 : 0,
				    it->second.out_queue.size(),
				    it->second.active ? 1 : 0);
#endif
			}
		}

		epoll_event ev{};
		// уважим возможную паузу (если запись уже существовала и была на паузе)
		{
			auto it = sockets_.find(fd);
			bool paused = (it != sockets_.end()) ? it->second.paused : false;
			ev.events = (paused ? 0u : EPOLLIN) | EPOLLRDHUP;
			ev.data.fd = fd;
		}
		if (::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) != 0) {
			if (errno == EEXIST) {
				(void)::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
			} else {
				IO_LOG_ERR("epoll_ctl(ADD fd=%d) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno));
				return false;
			}
		}
		return true;
	}
	bool delete_socket(socket_t fd) override {
		if (::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr) != 0) {
			IO_LOG_ERR("epoll_ctl(DEL fd=%d) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno));
		}
		auto itT = timers_.find(fd);
		if (itT != timers_.end()) {
			int tfd = itT->second;
			::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr);
			::close(tfd);
			owner_.erase(tfd);
			timers_.erase(itT);
			timeouts_ms_.erase(fd);
		}
		{
			auto it = sockets_.find(fd);
			if (it != sockets_.end()) {
				size_t pend = (it->second.out_queue.size() > it->second.out_head) ? (it->second.out_queue.size() - it->second.out_head) : 0;
				if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
				sockets_.erase(it);
			}
		}
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
#ifndef NDEBUG
		IO_LOG_DBG("connect start fd=%d, async=%d, res=%d, errno=%d (%s)", (int)fd, async ? 1 : 0, r, err, std::strerror(err));
#endif
		int cur = fcntl(fd, F_GETFL, 0);
		if (cur < 0) cur = 0;
		fcntl(fd, F_SETFL, cur | O_NONBLOCK);

		// Immediate success
		if (r == 0 || err == EISCONN) {
			auto it = sockets_.find(fd);
			if (it != sockets_.end()) it->second.active = true;
			epoll_event ev{};
			bool paused = (it != sockets_.end()) ? it->second.paused : false;
			ev.events = (paused ? 0u : EPOLLIN) | EPOLLRDHUP;
			ev.data.fd = fd;
			(void)::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
			(void)::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev);
			if (cbs_.on_accept) cbs_.on_accept(fd);
			stats_.connects_ok.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		// Async in-progress
		if (async && (err == EINPROGRESS || err == EALREADY)) {
			auto it = sockets_.find(fd);
			if (it == sockets_.end()) {
				SockState st;
				st.connecting = true;
				sockets_.emplace(fd, std::move(st));
#ifndef NDEBUG
				IO_LOG_DBG("connect EINPROGRESS NEW state fd=%d (no prior add_socket)", (int)fd);
#endif
			} else {
				it->second.connecting = true;
#ifndef NDEBUG
				IO_LOG_DBG("connect EINPROGRESS UPDATE state fd=%d (connecting=1)", (int)fd);
#endif
			}
			epoll_event ev{};
			auto it2 = sockets_.find(fd);
			bool paused = (it2 != sockets_.end()) ? it2->second.paused : false;
			ev.events = EPOLLOUT | (paused ? 0u : EPOLLIN) | EPOLLRDHUP;
			ev.data.fd = fd;
			if (::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) != 0) {
				if (errno == EEXIST) {
					(void)::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
				} else {
					IO_LOG_ERR("epoll_ctl(ADD fd=%d for connect) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno));
				}
			}
			return true;
		}
		// Immediate failure path (not in-progress)
		IO_LOG_ERR("connect(fd=%d) failed, res=%d, errno=%d (%s)", (int)fd, r, err, std::strerror(err));
		stats_.connects_fail.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	bool disconnect(socket_t fd) override {
		if (fd >= 0) {
			// Сначала удалим из epoll и внутреннего состояния, затем закроем fd.
			(void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
			bool dec = false;
			auto it = sockets_.find(fd);
			if (it != sockets_.end()) {
				size_t pend = (it->second.out_queue.size() > it->second.out_head) ? (it->second.out_queue.size() - it->second.out_head) : 0;
				if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
				dec = it->second.server_side;
				sockets_.erase(it);
			}
			auto itT = timers_.find(fd);
			if (itT != timers_.end()) {
				int tfd = itT->second;
				::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr);
				::close(tfd);
				owner_.erase(tfd);
				timers_.erase(itT);
			}
			timeouts_ms_.erase(fd);
			if (dec && cur_conn_ > 0)
				cur_conn_--;
			::shutdown(fd, SHUT_RDWR);
			::close(fd);
			IO_LOG_DBG("close: fd=%d", (int)fd);
			if (cbs_.on_close)
				cbs_.on_close(fd);
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
		epoll_event ev{};
		ev.events = EPOLLIN;
		ev.data.fd = listen_socket;
		if (::epoll_ctl(ep_, EPOLL_CTL_ADD, listen_socket, &ev) != 0) {
			if (errno == EEXIST) {
				(void)::epoll_ctl(ep_, EPOLL_CTL_MOD, listen_socket, &ev);
			} else {
				IO_LOG_ERR("epoll_ctl(ADD listen fd=%d) failed errno=%d (%s)", (int)listen_socket, errno,
						   std::strerror(errno));
				return false;
			}
		}
		return true;
	}
	bool write(socket_t fd, const char *data, size_t data_size) override {
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return false;
		auto &st = it->second;
		// Append to the logical end; if we've consumed a large prefix, compact first to avoid unbounded growth
		if (st.out_head > 0) {
			// Compact if consumed prefix is large (>= 64KB) and >= remaining payload
			size_t remaining = st.out_queue.size() - st.out_head;
			if (st.out_head >= 64 * 1024 && st.out_head >= remaining) {
				// Move remaining bytes to the beginning
				if (remaining > 0)
					std::memmove(st.out_queue.data(), st.out_queue.data() + st.out_head, remaining);
				st.out_queue.resize(remaining);
				st.out_head = 0;
			}
		}
		st.out_queue.insert(st.out_queue.end(), data, data + data_size);
		stats_.send_enqueued_bytes.fetch_add((uint64_t)data_size, std::memory_order_relaxed);
#ifndef NDEBUG
		IO_LOG_DBG("write queued fd=%d, bytes=%zu, want_write=%d (queued_total=%zu, head=%zu)", (int)fd, data_size,
		           st.want_write ? 1 : 0, st.out_queue.size(), st.out_head);
#endif
		if (!st.want_write) {
			epoll_event ev{};
			bool paused = st.paused;
			ev.events = (paused ? 0u : EPOLLIN) | EPOLLOUT | EPOLLRDHUP;
			ev.data.fd = fd;
			int rc = ::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
#ifndef NDEBUG
			if (rc != 0) {
				IO_LOG_ERR("epoll_ctl(MOD fd=%d flags=%s%s%s) failed errno=%d (%s)", (int)fd,
				           (ev.events & EPOLLIN) ? "IN" : "",
				           (ev.events & EPOLLOUT) ? "|OUT" : "",
				           (ev.events & EPOLLRDHUP) ? "|HUP" : "",
				           errno, std::strerror(errno));
			} else {
				IO_LOG_DBG("epoll MOD fd=%d -> %s%s%s", (int)fd,
				           (ev.events & EPOLLIN) ? "IN" : "",
				           (ev.events & EPOLLOUT) ? "|OUT" : "",
				           (ev.events & EPOLLRDHUP) ? "|HUP" : "");
			}
#endif
			st.want_write = true;
		}
		return true;
	}
	bool post(uint32_t val) override {
		// Очередь значений + одиночная будилка при переходе из пустого состояния.
		stats_.user_events.fetch_add(1, std::memory_order_relaxed);
		bool need_wake = false;
		{
			std::lock_guard<std::mutex> lk(user_mtx_);
			need_wake = user_vals_.empty();
			user_vals_.push_back(val);
		}
		if (need_wake && user_pipe_[1] != -1) {
			uint8_t b = 1;
			(void)::write(user_pipe_[1], &b, sizeof(b));
		}
		return true;
	}
	bool set_read_timeout(socket_t socket, uint32_t timeout_ms) override {
		// Create or update a timerfd for this socket
		if (timeout_ms == 0) {
			auto it = timers_.find(socket);
			if (it != timers_.end()) {
				int tfd = it->second;
				::epoll_ctl(ep_, EPOLL_CTL_DEL, tfd, nullptr);
				::close(tfd);
				timers_.erase(it);
			}
			timeouts_ms_.erase(socket);
			return true;
		}
		if (sockets_.find(socket) == sockets_.end())
			return false;
		int tfd = -1;
		{
			auto it = timers_.find(socket);
			if (it == timers_.end()) {
				tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
				if (tfd < 0)
					return false;
				timers_[socket] = tfd;
				owner_[tfd] = socket;
				epoll_event tev{};
				tev.events = EPOLLIN;
				tev.data.fd = tfd;
				::epoll_ctl(ep_, EPOLL_CTL_ADD, tfd, &tev);
			} else {
				tfd = it->second;
			}
			timeouts_ms_[socket] = timeout_ms;
		}
		itimerspec its{};
		its.it_value.tv_sec = timeout_ms / 1000;
		its.it_value.tv_nsec = (timeout_ms % 1000) * 1000000L;
		// One-shot timer; we'll rearm on read events
		if (::timerfd_settime(tfd, 0, &its, nullptr) != 0)
			return false;
		return true;
	}

	bool pause_read(socket_t fd) override {
		epoll_event ev{};
		ev.data.fd = fd;
		ev.events = EPOLLRDHUP;
		{
			auto it = sockets_.find(fd);
			if (it != sockets_.end()) {
				it->second.paused = true;
				if (it->second.want_write)
					ev.events |= EPOLLOUT;
			}
		}
		if (::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev) != 0) {
			IO_LOG_ERR("epoll_ctl(MOD pause fd=%d) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno));
			return false;
		}
	#ifndef NDEBUG
		IO_LOG_DBG("pause_read: fd=%d", (int)fd);
	#endif
		return true;
	}

	bool resume_read(socket_t fd) override {
		epoll_event ev{};
		ev.data.fd = fd;
		ev.events = EPOLLIN | EPOLLRDHUP;
		{
			auto it = sockets_.find(fd);
			if (it != sockets_.end()) {
				it->second.paused = false;
				if (it->second.want_write)
					ev.events |= EPOLLOUT;
			}
		}
		if (::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev) != 0) {
			IO_LOG_ERR("epoll_ctl(MOD resume fd=%d) failed errno=%d (%s)", (int)fd, errno, std::strerror(errno));
			return false;
		}
	#ifndef NDEBUG
		IO_LOG_DBG("resume_read: fd=%d", (int)fd);
	#endif
		return true;
	}

	protected:
		void wake() override {
				// Разбудить epoll_wait: пишем в user pipe
				stats_.wakes_posted.fetch_add(1, std::memory_order_relaxed);
				(void)post(0u);
		}

  private:
	struct SockState {
		char *buf{nullptr};
		size_t buf_size{0};
		ReadCallback read_cb;
		std::vector<char> out_queue; // write buffer (prefix [0..out_head) consumed)
		size_t out_head{0};          // logical start offset into out_queue
		bool want_write{false};
		bool connecting{false};
		bool active{false}; // becomes true when socket is fully connected/usable for I/O
		bool paused{false}; // suppress EPOLLIN while true
		bool server_side{false}; // true for sockets accepted on the server (counted toward max_conn_)
	};
	int ep_{-1};
	NetCallbacks cbs_{};
	int user_pipe_[2]{-1, -1};
	std::mutex user_mtx_;
	std::deque<uint32_t> user_vals_;

	std::unordered_map<socket_t, SockState> sockets_;
	std::unordered_set<socket_t> listeners_;
	// timerfd per socket for read idle timeout
	std::unordered_map<socket_t, int> timers_;			 // socket -> timerfd
	std::unordered_map<int, socket_t> owner_;			 // timerfd -> socket
	std::unordered_map<socket_t, uint32_t> timeouts_ms_; // socket -> timeout ms
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
		std::atomic<uint64_t> gqcs_errors{0}; // not used here
	} stats_{};
    
};

INetEngine *create_engine_epoll() {
	return new EpollEngine();
}

NetStats EpollEngine::get_stats() {
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

void EpollEngine::reset_stats() {
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

}; // namespace io

#endif
