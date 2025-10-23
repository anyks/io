#include "io/net.hpp"
#if defined(IO_ENGINE_IOURING)
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <liburing.h>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Logging macros: errors in Debug, debug gated by IO_ENABLE_IOURING_VERBOSE
#ifndef NDEBUG
#define IO_LOG_ERR(...)                                                                                                 \
	do {                                                                                                                \
		std::fprintf(stderr, "[io/iouring][ERR] ");                                                                    \
		std::fprintf(stderr, __VA_ARGS__);                                                                              \
		std::fprintf(stderr, "\n");                                                                                     \
	} while (0)
#else
#define IO_LOG_ERR(...) ((void)0)
#endif

#ifdef IO_ENABLE_IOURING_VERBOSE
#define IO_LOG_DBG(...)                                                                                                 \
	do {                                                                                                                \
		std::fprintf(stderr, "[io/iouring][DBG] ");                                                                    \
		std::fprintf(stderr, __VA_ARGS__);                                                                              \
		std::fprintf(stderr, "\n");                                                                                     \
	} while (0)
#else
#define IO_LOG_DBG(...) ((void)0)
#endif

namespace io {

enum class Op : uint8_t { Accept = 1, Read = 2, Write = 3, User = 4, Connect = 5, Timeout = 6, TimeoutRemove = 7, Cancel = 8 };

struct UringData {
	Op op{Op::Read};
	socket_t fd{(socket_t)-1};
	uint32_t extra{0};     // 1=poll connect, 2=native connect
	void *ptr{nullptr};    // optional heap memory to free on completion
	uint64_t u64{0};       // generic 64-bit payload (e.g., timeout key)
};

class IouringEngine : public INetEngine {
  public:
	IouringEngine() = default;
	~IouringEngine() override {
		destroy();
	}

	// Метрики
	NetStats get_stats() override;
	void reset_stats() override;

	bool loop_once(uint32_t timeout_ms) override {
		io_uring_cqe *cqe = nullptr;
		int rc = 0;
		if (timeout_ms == 0xFFFFFFFFu) {
			rc = io_uring_wait_cqe(&ring_, &cqe);
		} else if (timeout_ms == 0) {
			rc = io_uring_peek_cqe(&ring_, &cqe);
			if (rc == -EAGAIN) return false;
		} else {
			__kernel_timespec ts{}; ts.tv_sec = timeout_ms/1000; ts.tv_nsec = (timeout_ms%1000)*1000000L;
			rc = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
		}
		if (rc != 0 || !cqe) return false;
		// Process one and drain remaining without blocking
		handle_cqe(cqe);
		while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
			handle_cqe(cqe);
		}
		return true;
	}

	bool init(const NetCallbacks &cbs) override {
		cbs_ = cbs;
		io::suppress_sigpipe_once();
		if (io_uring_queue_init(256, &ring_, 0) != 0)
			return false;
		ring_inited_ = true;
		// Detect kernel support for IORING_OP_CONNECT once
		probe_ = io_uring_get_probe_ring(&ring_);
		connect_supported_ = (probe_ && io_uring_opcode_supported(probe_, IORING_OP_CONNECT));
		// user eventfd for wake()/post(): efficient counter
		user_efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (user_efd_ < 0) return false;
		// register read on eventfd via io_uring
		submit_read_user();
		return true;
	}

	

	// Основной цикл обработки: блокируется на io_uring и проверяет run_flag
	void event_loop(std::atomic<bool> &run_flag, int32_t wait_ms) override {
		io_uring_cqe *cqe = nullptr;
		if (wait_ms < 0) {
			// Бесконечное блокирующее ожидание
			while (run_flag.load(std::memory_order_relaxed)) {
				int wr = io_uring_wait_cqe(&ring_, &cqe);
				if (wr == 0 && cqe) {
					handle_cqe(cqe);
					// дренируем очередь без блокировок
					while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
						handle_cqe(cqe);
					}
				}
			}
			return;
		}
		if (wait_ms == 0) {
			// Неблокирующий опрос (busy-poll)
			while (run_flag.load(std::memory_order_relaxed)) {
				int pr = io_uring_peek_cqe(&ring_, &cqe);
				if (pr == 0 && cqe) {
					handle_cqe(cqe);
					// дренируем остаток без блокировок
					while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
						handle_cqe(cqe);
					}
				}
			}
			return;
		}
		// Положительный таймаут: блокируемся до указанного времени
		__kernel_timespec ts{};
		ts.tv_sec = wait_ms / 1000;
		ts.tv_nsec = (wait_ms % 1000) * 1000000L;
		while (run_flag.load(std::memory_order_relaxed)) {
			int wr = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
			if (wr == 0 && cqe) {
				handle_cqe(cqe);
				// дренируем остаток без блокировок
				while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
					handle_cqe(cqe);
				}
			}
		}
	}

	void destroy() override {
		// Best-effort cleanup of tracked sockets and listeners
		for (auto &kv : sockets_) {
			socket_t fd = kv.first;
			// invoke on_close before closing
			if (cbs_.on_close)
				cbs_.on_close(fd);
			::shutdown(fd, SHUT_RDWR);
			::close(fd);
		}
		for (auto lfd : listeners_) {
			::close(lfd);
		}
		sockets_.clear();
		listeners_.clear();
		timeouts_fd_.clear();
		timeouts_ud_.clear();
		// Free any pending UDs that may not complete after ring exit
		for (auto *p : pending_ud_) { delete p; }
		pending_ud_.clear();
		if (user_efd_ != -1) { ::close(user_efd_); user_efd_ = -1; }
		if (ring_inited_) {
			io_uring_queue_exit(&ring_);
			ring_inited_ = false;
		}
		if (probe_) {
			io_uring_free_probe(probe_);
			probe_ = nullptr;
		}
	}

	bool add_socket(socket_t fd, char *buffer, size_t buffer_size, ReadCallback cb) override {
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0)
			flags = 0;
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		sockets_[fd] = SockState{buffer, buffer_size, std::move(cb), {}, false, false};
		// Avoid flooding SQ with recv before a connection is actually established.
		// If the socket is connected (e.g., server-accepted), start reading now; for
		// client sockets, we'll submit recv after connect completion.
		sockaddr_storage ss{};
		socklen_t slen = sizeof(ss);
		if (::getpeername(fd, (sockaddr *)&ss, &slen) == 0) {
			submit_recv(fd);
		}
		return true;
	}

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

	bool delete_socket(socket_t fd) override {
		auto it = sockets_.find(fd);
		if (it != sockets_.end()) {
			if (it->second.timeout_armed) {
				submit_timeout_remove(fd, it->second.timeout_key);
				it->second.timeout_armed = false;
				it->second.timeout_key = 0;
			}
			if (!it->second.out_queue.empty()) {
				stats_.send_dropped_bytes.fetch_add((uint64_t)it->second.out_queue.size(), std::memory_order_relaxed);
			}
			sockets_.erase(it);
		}
		return true;
	}

	bool connect(socket_t fd, const char *host, uint16_t port, bool async) override {
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1)
			return false;
		// require socket to be tracked
		if (sockets_.find(fd) == sockets_.end())
			return false;
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0) flags = 0;
		if (!async) {
			if (flags & O_NONBLOCK)
				fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
			int r = ::connect(fd, (sockaddr *)&addr, sizeof(addr));
			int cur = fcntl(fd, F_GETFL, 0);
			if (cur < 0) cur = 0;
			fcntl(fd, F_SETFL, cur | O_NONBLOCK);
			if (r == 0) {
				// Синхронный путь: для согласованности доставим on_accept через loop-поток
				submit_nop_connect(fd);
				submit_recv(fd);
				return true;
			}
			return false;
		}
		// Асинхронный путь: без доп. потоков. Предпочитаем IORING_OP_CONNECT, иначе POLL_ADD.
		int cur = fcntl(fd, F_GETFL, 0);
		if (cur < 0) cur = 0;
		fcntl(fd, F_SETFL, cur | O_NONBLOCK);
		if (connect_supported_) {
			auto it = sockets_.find(fd);
			if (it != sockets_.end()) it->second.connecting = true;
			submit_connect(fd, addr);
			return true;
		}
		// Fallback: неблокирующий connect + IORING_OP_POLL_ADD на POLLOUT
		int r = ::connect(fd, (sockaddr *)&addr, sizeof(addr));
		if (r == 0) {
			// Немедленный success — доставим on_accept через CQE nop
			submit_nop_connect(fd);
			submit_recv(fd);
			return true;
		}
		int e = errno;
		if (e != EINPROGRESS)
			return false;
		auto it = sockets_.find(fd);
		if (it != sockets_.end()) it->second.connecting = true;
		submit_pollout(fd);
		return true;
	}

	bool disconnect(socket_t fd) override {
		if (fd >= 0) {
			// cancel timer if any
			auto it = sockets_.find(fd);
			if (it != sockets_.end() && it->second.timeout_armed) {
				submit_timeout_remove(fd, it->second.timeout_key);
				it->second.timeout_armed = false;
				it->second.timeout_key = 0;
			}
			// account dropped pending send bytes and erase from map
			bool dec = false; size_t pend = 0;
			{
				auto itS = sockets_.find(fd);
				if (itS != sockets_.end()) {
					pend = itS->second.out_queue.size();
					dec = itS->second.server_side;
					sockets_.erase(itS);
				}
			}
			if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
			if (dec && cur_conn_>0) cur_conn_--;
			::shutdown(fd, SHUT_RDWR);
			::close(fd);
			if (cbs_.on_close)
				cbs_.on_close(fd);
			stats_.closes.fetch_add(1, std::memory_order_relaxed);
		}
		return true;
	}

	bool accept(socket_t listen_socket, bool async, uint32_t max_connections) override {
		if (async) {
			int flags = fcntl(listen_socket, F_GETFL, 0);
			if (flags < 0)
				flags = 0;
			fcntl(listen_socket, F_SETFL, flags | O_NONBLOCK);
		}
		max_conn_ = max_connections;
		listeners_.insert(listen_socket);
		submit_accept(listen_socket);
		return true;
	}

	bool write(socket_t fd, const char *data, size_t data_size) override {
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return false;
		auto &st = it->second;
		st.out_queue.insert(st.out_queue.end(), data, data + data_size);
		stats_.send_enqueued_bytes.fetch_add((uint64_t)data_size, std::memory_order_relaxed);
		if (!st.want_write) {
			st.want_write = true;
			submit_send(fd);
		}
		return true;
	}

	bool post(uint32_t value) override {
		if (user_efd_ == -1)
			return false;
		uint32_t v = (value == 0 ? 1u : value);
		{
			std::lock_guard<std::mutex> lk(user_mtx_);
			user_queue_.push_back(v);
		}
		// Signal exactly one pending event via eventfd
		uint64_t one = 1ull;
		ssize_t n = ::write(user_efd_, &one, sizeof(one));
		if (n == (ssize_t)sizeof(one)) { stats_.user_events.fetch_add(1, std::memory_order_relaxed); return true; }
		return false;
	}

	bool set_read_timeout(socket_t socket, uint32_t timeout_ms) override {
		auto it = sockets_.find(socket);
		if (it == sockets_.end())
			return false;
		auto &st = it->second;
		st.timeout_ms = timeout_ms;
		if (timeout_ms == 0) {
			if (st.timeout_armed) {
				submit_timeout_remove(socket, st.timeout_key);
				st.timeout_armed = false;
				st.timeout_key = 0;
			}
			return true;
		}
		// arm or re-arm
		if (st.timeout_armed) {
			submit_timeout_remove(socket, st.timeout_key);
			st.timeout_armed = false;
			st.timeout_key = 0;
		}
		submit_timeout(socket, timeout_ms);
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
		bool paused{false}; // added paused flag
		bool server_side{false}; // true for sockets accepted on the server (counted toward max_conn_)
		uint32_t timeout_ms{0};
		bool timeout_armed{false};
		uint64_t timeout_key{0};
		UringData *recv_ud{nullptr};
	};

	io_uring ring_{};
	io_uring_probe *probe_{nullptr};
	NetCallbacks cbs_{};
	bool ring_inited_{false};

	int user_efd_{-1};
	std::mutex user_mtx_;
	std::deque<uint32_t> user_queue_;
	std::unordered_map<socket_t, SockState> sockets_;
	std::unordered_set<socket_t> listeners_;
	// timeout bookkeeping: key -> fd and key -> timeout Ud
	std::unordered_map<uint64_t, socket_t> timeouts_fd_;
	std::unordered_map<uint64_t, UringData *> timeouts_ud_;
	std::unordered_set<UringData *> pending_ud_;
	std::atomic<uint32_t> max_conn_{0};
	std::atomic<uint32_t> cur_conn_{0};
	bool connect_supported_{false};
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

	void handle_cqe(io_uring_cqe *cqe) {
		UringData *ud = (UringData *)io_uring_cqe_get_data(cqe);
		int res = cqe->res;
		if (ud) {
			switch (ud->op) {
				case Op::Cancel: {
                    // ignore cancel completion; original op will complete with -ECANCELED
					break;
                }
				case Op::User: {
					// Deliver exactly 'cnt' queued user events preserving order
					if (res == (int)sizeof(uint64_t)) {
						uint64_t cnt = user_buf_;
						while (cnt-- > 0) {
							uint32_t v = 1;
							{
								std::lock_guard<std::mutex> lk(user_mtx_);
								if (!user_queue_.empty()) { v = user_queue_.front(); user_queue_.pop_front(); }
							}
							if (cbs_.on_user) cbs_.on_user(v);
						}
					}
					submit_read_user();
					break;
				}
				case Op::Accept: {
					socket_t listen_fd = ud->fd;
					if (res >= 0) {
						socket_t client = res;
						if (max_conn_ > 0 && cur_conn_.load() >= max_conn_) {
							::close(client);
						} else {
							int fl = fcntl(client, F_GETFL, 0);
							if (fl < 0)
								fl = 0;
							fcntl(client, F_SETFL, fl | O_NONBLOCK);
							cur_conn_++;
							// peak update and accepts_ok
							{
								auto cur = (uint64_t)cur_conn_.load(std::memory_order_relaxed);
								uint64_t prev = stats_.peak_connections.load(std::memory_order_relaxed);
								while (cur > prev && !stats_.peak_connections.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {}
								stats_.accepts_ok.fetch_add(1, std::memory_order_relaxed);
							}
							IO_LOG_DBG("accept: client fd=%d", (int)client);
							// Предрегистрация клиента в sockets_ с пустым буфером до add_socket
							{
								SockState st{};
								st.server_side = true;
								sockets_.emplace(client, std::move(st));
							}
							if (cbs_.on_accept) {
								IO_LOG_DBG("invoke on_accept(server) fd=%d", (int)client);
								cbs_.on_accept(client);
							}
							// auto-recv only after user adds socket with buffer via add_socket
						}
					} else {
						int e = -res;
						if (e < 0)
							e = errno;
						IO_LOG_ERR("accept completion failed listen=%d errno=%d (%s)", (int)listen_fd, e,
								   std::strerror(e));
						stats_.accepts_fail.fetch_add(1, std::memory_order_relaxed);
					}
					submit_accept(listen_fd); // re-arm accept regardless of res
					break;
				}
				case Op::Read: {
					socket_t fd = ud->fd;
					auto it = sockets_.find(fd);
					if (it != sockets_.end()) {
						auto &st = it->second; // reference to mutate state
						// mark no longer in-flight
						if (st.recv_ud == ud) st.recv_ud = nullptr;
						if (res > 0) {
							// if paused, do not deliver; re-arm after resume
							if (!st.paused) {
								auto cb = st.read_cb;
								auto buf = st.buf;
								size_t n = (size_t)res;
								if (cb)
									cb(fd, buf, n);
								stats_.reads.fetch_add(1, std::memory_order_relaxed);
								stats_.bytes_read.fetch_add((uint64_t)n, std::memory_order_relaxed);
							}
							// decide re-arms without holding mtx_
							uint32_t ms = 0; bool had_timeout=false; uint64_t key=0;
							auto it2 = sockets_.find(fd);
							if (it2 != sockets_.end()) {
								auto &st2 = it2->second;
								ms = st2.timeout_ms;
								had_timeout = st2.timeout_armed;
								key = st2.timeout_key;
							}
							// submit next read and handle timeout rearm outside of mtx_
							submit_recv(fd);
							if (had_timeout) submit_timeout_remove(fd, key);
							if (ms > 0) submit_timeout(fd, ms);
						} else {
							// treat canceled read specially: do not close
							if (res == -ECANCELED) {
								break;
							}
							// closed or error
							// cancel pending timeout if any
							if (st.timeout_armed) {
								submit_timeout_remove(fd, st.timeout_key);
							}
							IO_LOG_DBG("close: fd=%d", (int)fd);
							if (cbs_.on_close)
								cbs_.on_close(fd);
							// account dropped pending send bytes
							size_t pend = st.out_queue.size();
							if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
							stats_.closes.fetch_add(1, std::memory_order_relaxed);
							bool dec=false; {
								auto itS = sockets_.find(fd);
								if (itS != sockets_.end()) { dec = itS->second.server_side; sockets_.erase(itS);} }
							if (dec && cur_conn_>0) cur_conn_--;
							::close(fd);
						}
					}
					break;
				}
				case Op::Write: {
					socket_t fd = ud->fd;
					auto it = sockets_.find(fd);
					if (it != sockets_.end()) {
						auto &st = it->second;
						if (res > 0) {
							size_t wrote = (size_t)res;
							if (cbs_.on_write) {
								cbs_.on_write(fd, wrote);
							}
							if (wrote <= st.out_queue.size())
								st.out_queue.erase(st.out_queue.begin(), st.out_queue.begin() + wrote);
							stats_.writes.fetch_add(1, std::memory_order_relaxed);
							stats_.bytes_written.fetch_add((uint64_t)wrote, std::memory_order_relaxed);
							stats_.send_dequeued_bytes.fetch_add((uint64_t)wrote, std::memory_order_relaxed);
						}
						if (res < 0) {
							int e = -res;
							if (e < 0) e = errno;
							if (e == EPIPE || e == ECONNRESET) {
								// cancel pending timeout if any to avoid later close on timed-out CQE
								if (st.timeout_armed) {
									submit_timeout_remove(fd, st.timeout_key);
									st.timeout_armed = false;
									st.timeout_key = 0;
								}
								// account dropped pending bytes on abrupt close
								size_t pend = st.out_queue.size();
								if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
								if (cbs_.on_close) cbs_.on_close(fd);
								::close(fd);
								bool dec=false; { auto itS=sockets_.find(fd); if (itS!=sockets_.end()){ dec=itS->second.server_side; sockets_.erase(itS);} }
								if (dec && cur_conn_>0) cur_conn_--;
								stats_.closes.fetch_add(1, std::memory_order_relaxed);
								break;
							}
						}
						bool need_more = !st.out_queue.empty();
						st.want_write = need_more;
						if (need_more) submit_send(fd);
					}
					break;
				}
				case Op::Connect: {
					socket_t fd = ud->fd;
					IO_LOG_DBG("connect CQE fd=%d extra=%u res=%d", (int)fd, (unsigned)ud->extra, res);
					auto it = sockets_.find(fd);
					if (it != sockets_.end()) {
						auto &st = it->second;
						st.connecting = false;
						if (ud->extra == 2) {
							// native connect: res==0 success, negative error otherwise
							if (res == 0) {
								IO_LOG_DBG("connect: established fd=%d", (int)fd);
								stats_.connects_ok.fetch_add(1, std::memory_order_relaxed);
								if (cbs_.on_accept) { IO_LOG_DBG("invoke on_accept(client) fd=%d", (int)fd); cbs_.on_accept(fd);} 
								submit_recv(fd);
							} else {
								int e = -res;
								if (e < 0)
									e = errno; // fallback
								IO_LOG_ERR("connect completion fd=%d failed errno=%d (%s)", (int)fd, e,
										   std::strerror(e));
								if (cbs_.on_close)
									cbs_.on_close(fd);
								::close(fd);
								sockets_.erase(fd);
								stats_.connects_fail.fetch_add(1, std::memory_order_relaxed);
							}
						} else if (ud->extra == 1) {
							// poll path: res is revents bitmask (>=0). Check SO_ERROR to determine status.
							if (res >= 0) {
								if (err == 0) {
								socklen_t len = sizeof(err);
									stats_.connects_ok.fetch_add(1, std::memory_order_relaxed);
								int gs = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
								if (gs < 0) {
									int e = errno;
									IO_LOG_ERR("getsockopt(SO_ERROR fd=%d) failed errno=%d (%s)", (int)fd, e,
										   std::strerror(e));
									err = e;
								} else {
									stats_.connects_fail.fetch_add(1, std::memory_order_relaxed);
									IO_LOG_DBG("connect completion fd=%d, SO_ERROR=%d (%s)", (int)fd, err,
										   std::strerror(err));
								}
								if (err == 0) {
								stats_.connects_ok.fetch_add(1, std::memory_order_relaxed);
									IO_LOG_DBG("connect: established fd=%d", (int)fd);
									if (cbs_.on_accept) { IO_LOG_DBG("invoke on_accept(client) fd=%d", (int)fd); cbs_.on_accept(fd);} 
									submit_recv(fd);
								} else {
									if (cbs_.on_close)
										cbs_.on_close(fd);
									::close(fd);
									sockets_.erase(fd);
								}
							} else {
								if (cbs_.on_close)
									cbs_.on_close(fd);
								::close(fd);
								sockets_.erase(fd);
							}
						} else {
							// extra==0: synthetic success via NOP to deliver on_accept consistently
							IO_LOG_DBG("connect: established(fd=%d) via NOP", (int)fd);
							if (cbs_.on_accept) { IO_LOG_DBG("invoke on_accept(client) fd=%d", (int)fd); cbs_.on_accept(fd);} 
							submit_recv(fd);
						}
					}
					if (ud->ptr) {
						delete static_cast<sockaddr_in *>(ud->ptr);
					}
					break;
				}
				case Op::Timeout: {
					// user_data is pointer to this ud; use it as key
					uint64_t key = (uint64_t)ud;
					socket_t fd = (socket_t)-1;
					{
						auto itf = timeouts_fd_.find(key);
						if (itf != timeouts_fd_.end()) {
							fd = itf->second;
							timeouts_fd_.erase(itf);
						}
						timeouts_ud_.erase(key);
						auto itS = sockets_.find(fd);
						if (itS != sockets_.end()) {
							itS->second.timeout_armed = false;
							itS->second.timeout_key = 0;
						}
					}
					if (res == -ETIME && fd != -1) {
						if (cbs_.on_close)
							cbs_.on_close(fd);
						// account dropped bytes if any
						size_t pend = 0; { auto itS=sockets_.find(fd); if (itS!=sockets_.end()){ pend = itS->second.out_queue.size(); }}
						if (pend) stats_.send_dropped_bytes.fetch_add((uint64_t)pend, std::memory_order_relaxed);
						::close(fd);
						bool dec=false; { auto itS=sockets_.find(fd); if (itS!=sockets_.end()){ dec=itS->second.server_side; sockets_.erase(itS);} }
						if (dec && cur_conn_>0) cur_conn_--;
						stats_.timeouts.fetch_add(1, std::memory_order_relaxed);
						stats_.closes.fetch_add(1, std::memory_order_relaxed);
					}
					break;
				}
				case Op::TimeoutRemove: {
					// res >= 0 means removed count; free original timeout ud by key
					uint64_t key = ud->u64;
					socket_t fd = (socket_t)-1;
					{
						// Do not delete the original timeout UD here to avoid UAF if a canceled timeout CQE arrives later.
						// Leave entry in timeouts_ud_ map; original UD will be freed when its CQE (likely -ECANCELED) is handled.
						auto itf = timeouts_fd_.find(key);
						if (itf != timeouts_fd_.end()) {
							fd = itf->second;
							timeouts_fd_.erase(itf);
						}
						if (fd != -1) {
							auto itS = sockets_.find(fd);
							if (itS != sockets_.end()) {
								itS->second.timeout_armed = false;
								itS->second.timeout_key = 0;
							}
						}
					}
					// Don't delete tud here; it will be deleted when the Timeout CQE is processed (possibly with -ECANCELED).
					break;
				}
			}
			pending_ud_.erase(ud);
			delete ud;
			io_uring_cqe_seen(&ring_, cqe);
		}
	}
	bool pause_read(socket_t fd) override {
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return false;
		auto &st = it->second;
		st.paused = true;
		// Cancel in-flight recv if any
		if (st.recv_ud) {
			io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
			if (!sqe) { (void)io_uring_submit(&ring_); sqe = io_uring_get_sqe(&ring_); }
			if (sqe) {
				UringData *cud = new UringData{Op::Cancel, -1, 0};
				io_uring_prep_cancel(sqe, st.recv_ud, 0);
				io_uring_sqe_set_data(sqe, cud);
				(void)io_uring_submit(&ring_);
			}
			st.recv_ud = nullptr;
		}
		return true;
	}
	bool resume_read(socket_t fd) override {
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return false;
		it->second.paused = false;
		submit_recv(fd);
		return true;
	}

	void submit_recv(socket_t fd) {
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return;
		auto &st = it->second;
		if (!st.buf || st.buf_size == 0)
			return;
		if (st.paused)
			return; // check if paused
		io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
		if (!sqe) {
			(void)io_uring_submit(&ring_);
			sqe = io_uring_get_sqe(&ring_);
			if (!sqe) {
				IO_LOG_ERR("no SQE available for recv fd=%d", (int)fd);
				return;
			}
		}
		UringData *ud = new UringData{Op::Read, fd, 0};
		pending_ud_.insert(ud);
		io_uring_prep_recv(sqe, fd, st.buf, st.buf_size, 0);
		io_uring_sqe_set_data(sqe, ud);
		st.recv_ud = ud;
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(recv fd=%d) failed", (int)fd);
		}
	}

	void submit_send(socket_t fd) {
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return;
		auto &st = it->second;
		if (st.out_queue.empty())
			return;
		io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
		if (!sqe) {
			(void)io_uring_submit(&ring_);
			sqe = io_uring_get_sqe(&ring_);
			if (!sqe) {
				IO_LOG_ERR("no SQE available for send fd=%d", (int)fd);
				return;
			}
		}
		UringData *ud = new UringData{Op::Write, fd, 0};
		pending_ud_.insert(ud);
		io_uring_prep_send(sqe, fd, st.out_queue.data(), st.out_queue.size(), 0);
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(send fd=%d) failed", (int)fd);
		}
	}

	void submit_accept(socket_t listen_fd) {
		io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
		if (!sqe) {
			(void)io_uring_submit(&ring_);
			sqe = io_uring_get_sqe(&ring_);
			if (!sqe) {
				IO_LOG_ERR("no SQE available for accept listen=%d", (int)listen_fd);
				return;
			}
		}
		UringData *ud = new UringData{Op::Accept, listen_fd, 0};
		pending_ud_.insert(ud);
		// Prepare sockaddr and reset len for each submit; avoid SOCK_NONBLOCK for wider kernel compatibility
		static sockaddr_storage ss;
		static socklen_t slen;
		slen = (socklen_t)sizeof(ss);
		io_uring_prep_accept(sqe, listen_fd, (sockaddr *)&ss, &slen, 0);
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(accept listen=%d) failed", (int)listen_fd);
		}
	}

	void submit_pollout(socket_t fd) {
		io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
		if (!sqe) {
			(void)io_uring_submit(&ring_);
			sqe = io_uring_get_sqe(&ring_);
			if (!sqe) {
				IO_LOG_ERR("no SQE available for pollout fd=%d", (int)fd);
				return;
			}
		}
		UringData *ud = new UringData{Op::Connect, fd, 1, nullptr};
		pending_ud_.insert(ud);
		short mask = (short)(POLLOUT | POLLIN | POLLERR | POLLHUP);
		io_uring_prep_poll_add(sqe, fd, mask);
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(pollout fd=%d) failed", (int)fd);
		}
	}

	void submit_nop_connect(socket_t fd) {
		io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
		if (!sqe) {
			(void)io_uring_submit(&ring_);
			sqe = io_uring_get_sqe(&ring_);
			if (!sqe) {
				IO_LOG_ERR("no SQE available for nop-connect fd=%d", (int)fd);
				return;
			}
		}
		UringData *ud = new UringData{Op::Connect, fd, 0, nullptr};
		pending_ud_.insert(ud);
		io_uring_prep_nop(sqe);
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(nop-connect fd=%d) failed", (int)fd);
		}
	}

	void submit_connect(socket_t fd, const sockaddr_in &addr) {
		io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
		if (!sqe) {
			(void)io_uring_submit(&ring_);
			sqe = io_uring_get_sqe(&ring_);
			if (!sqe) {
				IO_LOG_ERR("no SQE available for connect fd=%d", (int)fd);
				return;
			}
		}
		// allocate a copy of address to ensure lifetime until completion
		auto *heap_addr = new sockaddr_in(addr);
		UringData *ud = new UringData{Op::Connect, fd, 2, heap_addr};
		pending_ud_.insert(ud);
		io_uring_prep_connect(sqe, fd, (sockaddr *)heap_addr, sizeof(*heap_addr));
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(connect fd=%d) failed", (int)fd);
		}
	}

	void submit_read_user() {
		io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
		if (!sqe) {
			(void)io_uring_submit(&ring_);
			sqe = io_uring_get_sqe(&ring_);
			if (!sqe) {
				IO_LOG_ERR("no SQE available for user read");
				return;
			}
		}
		UringData *ud = new UringData{Op::User, user_efd_, 0};
		pending_ud_.insert(ud);
		// Read one 64-bit counter from eventfd; we'll pop per-value items from user_queue_
		io_uring_prep_read(sqe, user_efd_, &user_buf_, sizeof(user_buf_), 0);
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(user read) failed");
		}
	}

	void submit_timeout(socket_t fd, uint32_t ms) {
		// prepare relative timeout
		__kernel_timespec ts{};
		ts.tv_sec = ms / 1000;
		ts.tv_nsec = (ms % 1000) * 1000000L;
		io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
		if (!sqe) {
			(void)io_uring_submit(&ring_);
			sqe = io_uring_get_sqe(&ring_);
			if (!sqe) {
				IO_LOG_ERR("no SQE available for timeout fd=%d", (int)fd);
				return;
			}
		}
		UringData *ud = new UringData{Op::Timeout, fd};
		pending_ud_.insert(ud);
		// use ud pointer value as timeout key
		uint64_t key = (uint64_t)ud;
		{
			auto it = sockets_.find(fd);
			if (it == sockets_.end()) {
				delete ud;
				return;
			}
			it->second.timeout_armed = true;
			it->second.timeout_key = key;
			timeouts_ud_[key] = ud;
			timeouts_fd_[key] = fd;
		}
		io_uring_prep_timeout(sqe, &ts, 0, 0);
		// set user_data to ud pointer (which equals key) so removal can find it
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(timeout fd=%d) failed", (int)fd);
		}
	}

	void submit_timeout_remove(socket_t /*fd*/, uint64_t key) {
		io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
		if (!sqe) {
			(void)io_uring_submit(&ring_);
			sqe = io_uring_get_sqe(&ring_);
			if (!sqe) {
				IO_LOG_ERR("no SQE available for timeout_remove key=%llu", (unsigned long long)key);
				return;
			}
		}
		UringData *ud = new UringData{Op::TimeoutRemove, -1};
		pending_ud_.insert(ud);
		ud->u64 = key;
		io_uring_prep_timeout_remove(sqe, key, 0);
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(timeout_remove key=%llu) failed", (unsigned long long)key);
		}
	}

	uint64_t user_buf_{0};

	void wake() override {
		// Постим пользовательское событие, чтобы разбудить wait_cqe/timeout
		stats_.wakes_posted.fetch_add(1, std::memory_order_relaxed);
		(void)post(1u);
	}
};

INetEngine *create_engine_iouring() {
	return new IouringEngine();
}

} // namespace io

NetStats IouringEngine::get_stats() {
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
	s.outstanding_accepts = 0; // we don't track depth here
	s.send_enqueued_bytes = stats_.send_enqueued_bytes.load(std::memory_order_relaxed);
	s.send_dequeued_bytes = stats_.send_dequeued_bytes.load(std::memory_order_relaxed);
	s.send_backlog_bytes = (s.send_enqueued_bytes >= s.send_dequeued_bytes)
						    ? (s.send_enqueued_bytes - s.send_dequeued_bytes)
						    : 0;
	s.send_dropped_bytes = stats_.send_dropped_bytes.load(std::memory_order_relaxed);
	return s;
}

void IouringEngine::reset_stats() {
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

#endif
