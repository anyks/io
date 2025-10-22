#include "io/net.hpp"
#if defined(IO_ENGINE_IOURING)
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <liburing.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef NDEBUG
#define IO_LOG_ERR(fmt, ...) std::fprintf(stderr, "[io/iouring][ERR] " fmt "\n", ##__VA_ARGS__)
#define IO_LOG_DBG(fmt, ...) std::fprintf(stderr, "[io/iouring][DBG] " fmt "\n", ##__VA_ARGS__)
#else
#define IO_LOG_ERR(fmt, ...) ((void)0)
#define IO_LOG_DBG(fmt, ...) ((void)0)
#endif

namespace io {

enum class Op : uint8_t { Accept = 1, Read = 2, Write = 3, User = 4, Connect = 5, Timeout = 6, TimeoutRemove = 7 };

struct UringData {
	Op op{Op::Read};
	socket_t fd{(socket_t)-1};
	uint32_t extra{0};	// 1=poll connect, 2=native connect
	void *ptr{nullptr}; // optional heap memory to free on completion
	uint64_t u64{0};	// generic 64-bit payload (e.g., timeout key)
};

class IouringEngine : public INetEngine {
  public:
	IouringEngine() = default;
	~IouringEngine() override {
		destroy();
	}

	bool init(const NetCallbacks &cbs) override {
		cbs_ = cbs;
		if (io_uring_queue_init(256, &ring_, 0) != 0)
			return false;
		user_efd_ = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
		if (user_efd_ < 0)
			return false;
		// register poll on eventfd via read operation
		submit_read_user();
		running_ = true;
		loop_ = std::thread(&IouringEngine::event_loop, this);
		return true;
	}

	void destroy() override {
		// Stop the event loop thread first
		running_ = false;
		if (user_efd_ != -1) {
			uint64_t one = 1;
			(void)::write(user_efd_, &one, sizeof(one));
		}
		if (loop_.joinable())
			loop_.join();
		// Best-effort cleanup of tracked sockets and listeners
		{
			std::lock_guard<std::mutex> lk(mtx_);
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
		}
		if (user_efd_ != -1) {
			::close(user_efd_);
			user_efd_ = -1;
		}
		io_uring_queue_exit(&ring_);
	}

	bool add_socket(socket_t fd, char *buffer, size_t buffer_size, ReadCallback cb) override {
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0)
			flags = 0;
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		std::lock_guard<std::mutex> lk(mtx_);
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
		std::lock_guard<std::mutex> lk(mtx_);
		auto it = sockets_.find(fd);
		if (it != sockets_.end()) {
			if (it->second.timeout_armed) {
				submit_timeout_remove(fd, it->second.timeout_key);
				it->second.timeout_armed = false;
				it->second.timeout_key = 0;
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
		{
			std::lock_guard<std::mutex> lk(mtx_);
			if (sockets_.find(fd) == sockets_.end())
				return false;
		}
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0)
			flags = 0;
		if (!async) {
			if (flags & O_NONBLOCK)
				fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
			int r = ::connect(fd, (sockaddr *)&addr, sizeof(addr));
			int cur = fcntl(fd, F_GETFL, 0);
			if (cur < 0)
				cur = 0;
			fcntl(fd, F_SETFL, cur | O_NONBLOCK);
			if (r == 0) {
				if (cbs_.on_accept)
					cbs_.on_accept(fd);
				submit_recv(fd);
				return true;
			}
			return false;
		}
		// async path: use native io_uring connect
		submit_connect(fd, addr);
		{
			std::lock_guard<std::mutex> lk(mtx_);
			auto it = sockets_.find(fd);
			if (it != sockets_.end())
				it->second.connecting = true;
		}
		return true;
	}

	bool disconnect(socket_t fd) override {
		if (fd >= 0) {
			// cancel timer if any
			{
				std::lock_guard<std::mutex> lk(mtx_);
				auto it = sockets_.find(fd);
				if (it != sockets_.end() && it->second.timeout_armed) {
					submit_timeout_remove(fd, it->second.timeout_key);
					it->second.timeout_armed = false;
					it->second.timeout_key = 0;
				}
			}
			::shutdown(fd, SHUT_RDWR);
			::close(fd);
			if (cbs_.on_close)
				cbs_.on_close(fd);
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
		{
			std::lock_guard<std::mutex> lk(mtx_);
			listeners_.insert(listen_socket);
		}
		submit_accept(listen_socket);
		return true;
	}

	bool write(socket_t fd, const char *data, size_t data_size) override {
		std::lock_guard<std::mutex> lk(mtx_);
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return false;
		auto &st = it->second;
		st.out_queue.insert(st.out_queue.end(), data, data + data_size);
		if (!st.want_write) {
			st.want_write = true;
			submit_send(fd);
		}
		return true;
	}

	bool post(uint32_t value) override {
		if (user_efd_ == -1)
			return false;
		uint64_t v = ((uint64_t)value == 0 ? 1ull : (uint64_t)value);
		ssize_t n = ::write(user_efd_, &v, sizeof(v));
		return n == (ssize_t)sizeof(v);
	}

	bool set_read_timeout(socket_t socket, uint32_t timeout_ms) override {
		std::lock_guard<std::mutex> lk(mtx_);
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
		uint32_t timeout_ms{0};
		bool timeout_armed{false};
		uint64_t timeout_key{0};
	};

	io_uring ring_{};
	NetCallbacks cbs_{};
	std::thread loop_{};
	std::atomic<bool> running_{false};
	int user_efd_{-1};
	std::mutex mtx_;
	std::unordered_map<socket_t, SockState> sockets_;
	std::unordered_set<socket_t> listeners_;
	// timeout bookkeeping: key -> fd and key -> timeout Ud
	std::unordered_map<uint64_t, socket_t> timeouts_fd_;
	std::unordered_map<uint64_t, UringData *> timeouts_ud_;
	std::atomic<uint32_t> max_conn_{0};
	std::atomic<uint32_t> cur_conn_{0};

	void event_loop() {
		while (running_) {
			io_uring_cqe *cqe = nullptr;
			int ret = io_uring_wait_cqe(&ring_, &cqe);
			if (ret != 0)
				continue;
			UringData *ud = (UringData *)io_uring_cqe_get_data(cqe);
			int res = cqe->res;
			if (ud) {
				switch (ud->op) {
				case Op::User: {
					// drain eventfd and callback with value 1 per trigger
					uint64_t cnt = 0;
					while (::read(user_efd_, &cnt, sizeof(cnt)) == sizeof(cnt)) {
						if (cbs_.on_user)
							cbs_.on_user((uint32_t)cnt);
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
							IO_LOG_DBG("accept: client fd=%d", (int)client);
							// Предрегистрация клиента в sockets_ с пустым буфером до add_socket
							{
								std::lock_guard<std::mutex> lk(mtx_);
								SockState st{};
								sockets_.emplace(client, std::move(st));
							}
							if (cbs_.on_accept)
								cbs_.on_accept(client);
							// auto-recv only after user adds socket with buffer via add_socket
						}
					} else {
						int e = -res;
						if (e < 0)
							e = errno;
						IO_LOG_ERR("accept completion failed listen=%d errno=%d (%s)", (int)listen_fd, e,
								   std::strerror(e));
					}
					submit_accept(listen_fd); // re-arm accept regardless of res
					break;
				}
				case Op::Read: {
					socket_t fd = ud->fd;
					std::unique_lock<std::mutex> lk(mtx_);
					auto it = sockets_.find(fd);
					if (it != sockets_.end()) {
						auto st = it->second; // copy for callbacks
						if (res > 0) {
							auto cb = st.read_cb;
							auto buf = st.buf;
							size_t n = (size_t)res;
							lk.unlock();
							if (cb)
								cb(fd, buf, n);
							lk.lock();
							// submit next read
							submit_recv(fd);
							// re-arm idle timer if configured
							auto it2 = sockets_.find(fd);
							if (it2 != sockets_.end()) {
								auto &st2 = it2->second;
								if (st2.timeout_ms > 0) {
									if (st2.timeout_armed) {
										submit_timeout_remove(fd, st2.timeout_key);
										st2.timeout_armed = false;
										st2.timeout_key = 0;
									}
									submit_timeout(fd, st2.timeout_ms);
								}
							}
						} else {
							// closed or error
							// cancel pending timeout if any
							if (st.timeout_armed) {
								submit_timeout_remove(fd, st.timeout_key);
							}
							lk.unlock();
							IO_LOG_DBG("close: fd=%d", (int)fd);
							if (cbs_.on_close)
								cbs_.on_close(fd);
							std::lock_guard<std::mutex> lk2(mtx_);
							sockets_.erase(fd);
							if (cur_conn_ > 0)
								cur_conn_--;
							::close(fd);
						}
					}
					break;
				}
				case Op::Write: {
					socket_t fd = ud->fd;
					std::unique_lock<std::mutex> lk(mtx_);
					auto it = sockets_.find(fd);
					if (it != sockets_.end()) {
						auto &st = it->second;
						if (res > 0) {
							size_t wrote = (size_t)res;
							if (cbs_.on_write) {
								lk.unlock();
								cbs_.on_write(fd, wrote);
								lk.lock();
							}
							if (wrote <= st.out_queue.size())
								st.out_queue.erase(st.out_queue.begin(), st.out_queue.begin() + wrote);
						}
						if (!st.out_queue.empty()) {
							submit_send(fd);
						} else {
							st.want_write = false;
						}
					}
					break;
				}
				case Op::Connect: {
					socket_t fd = ud->fd;
					std::unique_lock<std::mutex> lk(mtx_);
					auto it = sockets_.find(fd);
					if (it != sockets_.end()) {
						auto &st = it->second;
						st.connecting = false;
						if (ud->extra == 2) {
							// native connect: res==0 success, negative error otherwise
							if (res == 0) {
								lk.unlock();
								IO_LOG_DBG("connect: established fd=%d", (int)fd);
								if (cbs_.on_accept)
									cbs_.on_accept(fd);
								submit_recv(fd);
							} else {
								int e = -res;
								if (e < 0)
									e = errno; // fallback
								IO_LOG_ERR("connect completion fd=%d failed errno=%d (%s)", (int)fd, e,
										   std::strerror(e));
								lk.unlock();
								if (cbs_.on_close)
									cbs_.on_close(fd);
								::close(fd);
								std::lock_guard<std::mutex> lk2(mtx_);
								sockets_.erase(fd);
							}
						} else {
							// poll path: check SO_ERROR
							if (res == 0) {
								int err = 0;
								socklen_t len = sizeof(err);
								int gs = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
								if (gs < 0) {
									int e = errno;
									IO_LOG_ERR("getsockopt(SO_ERROR fd=%d) failed errno=%d (%s)", (int)fd, e,
											   std::strerror(e));
									err = e;
								} else {
									IO_LOG_DBG("connect completion fd=%d, SO_ERROR=%d (%s)", (int)fd, err,
											   std::strerror(err));
								}
								if (err == 0) {
									lk.unlock();
									IO_LOG_DBG("connect: established fd=%d", (int)fd);
									if (cbs_.on_accept)
										cbs_.on_accept(fd);
									submit_recv(fd);
								} else {
									lk.unlock();
									if (cbs_.on_close)
										cbs_.on_close(fd);
									::close(fd);
									std::lock_guard<std::mutex> lk2(mtx_);
									sockets_.erase(fd);
								}
							} else {
								lk.unlock();
								if (cbs_.on_close)
									cbs_.on_close(fd);
								::close(fd);
								std::lock_guard<std::mutex> lk2(mtx_);
								sockets_.erase(fd);
							}
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
						std::lock_guard<std::mutex> lk(mtx_);
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
						::close(fd);
						std::lock_guard<std::mutex> lk2(mtx_);
						sockets_.erase(fd);
						if (cur_conn_ > 0)
							cur_conn_--;
					}
					break;
				}
				case Op::TimeoutRemove: {
					// res >= 0 means removed count; free original timeout ud by key
					uint64_t key = ud->u64;
					UringData *tud = nullptr;
					socket_t fd = (socket_t)-1;
					{
						std::lock_guard<std::mutex> lk(mtx_);
						auto itu = timeouts_ud_.find(key);
						if (itu != timeouts_ud_.end()) {
							tud = itu->second;
							timeouts_ud_.erase(itu);
						}
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
					if (tud)
						delete tud;
					break;
				}
				}
				delete ud;
			}
			io_uring_cqe_seen(&ring_, cqe);
		}
	}
	bool pause_read(socket_t fd) override {
		std::lock_guard<std::mutex> lk(mtx_);
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return false;
		it->second.paused = true;
		return true;
	}
	bool resume_read(socket_t fd) override {
		{
			std::lock_guard<std::mutex> lk(mtx_);
			auto it = sockets_.find(fd);
			if (it == sockets_.end())
				return false;
			it->second.paused = false;
		}
		submit_recv(fd);
		return true;
	}

	void submit_recv(socket_t fd) {
		std::lock_guard<std::mutex> lk(mtx_);
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return;
		auto &st = it->second;
		if (!st.buf || st.buf_size == 0)
			return;
		if (st.paused)
			return; // check if paused
		std::lock_guard<std::mutex> rk(ring_mtx_);
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
		io_uring_prep_recv(sqe, fd, st.buf, st.buf_size, 0);
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(recv fd=%d) failed", (int)fd);
		}
	}

	void submit_send(socket_t fd) {
		std::lock_guard<std::mutex> lk(mtx_);
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return;
		auto &st = it->second;
		if (st.out_queue.empty())
			return;
		std::lock_guard<std::mutex> rk(ring_mtx_);
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
		io_uring_prep_send(sqe, fd, st.out_queue.data(), st.out_queue.size(), 0);
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(send fd=%d) failed", (int)fd);
		}
	}

	void submit_accept(socket_t listen_fd) {
		std::lock_guard<std::mutex> rk(ring_mtx_);
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
		std::lock_guard<std::mutex> rk(ring_mtx_);
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
		io_uring_prep_poll_add(sqe, fd, POLLOUT);
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(pollout fd=%d) failed", (int)fd);
		}
	}

	void submit_connect(socket_t fd, const sockaddr_in &addr) {
		std::lock_guard<std::mutex> rk(ring_mtx_);
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
		io_uring_prep_connect(sqe, fd, (sockaddr *)heap_addr, sizeof(*heap_addr));
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(connect fd=%d) failed", (int)fd);
		}
	}

	void submit_read_user() {
		std::lock_guard<std::mutex> rk(ring_mtx_);
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
		// Using read on eventfd; it will complete when counter > 0
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
		std::lock_guard<std::mutex> rk(ring_mtx_);
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
		// use ud pointer value as timeout key
		uint64_t key = (uint64_t)ud;
		{
			std::lock_guard<std::mutex> lk(mtx_);
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
		std::lock_guard<std::mutex> rk(ring_mtx_);
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
		ud->u64 = key;
		io_uring_prep_timeout_remove(sqe, key, 0);
		io_uring_sqe_set_data(sqe, ud);
		if (io_uring_submit(&ring_) < 0) {
			IO_LOG_ERR("io_uring_submit(timeout_remove key=%llu) failed", (unsigned long long)key);
		}
	}

	uint64_t user_buf_{0};
	std::mutex ring_mtx_;
};

INetEngine *create_engine_iouring() {
	return new IouringEngine();
}

} // namespace io

#endif
