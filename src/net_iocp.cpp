// IOCP backend (Windows)
#include "io/net.hpp"
#if defined(IO_ENGINE_IOCP)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#ifndef NDEBUG
#define IO_LOG_ERR(fmt, ...) std::fprintf(stderr, "[io/IOCP][ERR] " fmt "\n", ##__VA_ARGS__)
#define IO_LOG_DBG(fmt, ...) std::fprintf(stderr, "[io/IOCP][DBG] " fmt "\n", ##__VA_ARGS__)
#else
#define IO_LOG_ERR(fmt, ...) ((void)0)
#define IO_LOG_DBG(fmt, ...) ((void)0)
#endif

namespace io {

enum class OpType : uint8_t { Accept = 1, Read = 2, Write = 3, Connect = 4, User = 5 };

struct PerIo {
	OVERLAPPED ol{};
	OpType op{OpType::Read};
	socket_t fd{io::kInvalidSocket};
	// For Accept operations, remember the listener this accept belongs to
	socket_t listen_fd{io::kInvalidSocket};
	WSABUF wbuf{};
	std::vector<char> dynbuf; // for accept address storage or write data snapshot
	uint32_t user_val{0};
};

class IocpEngine : public INetEngine {
  public:
	IocpEngine() = default;
	~IocpEngine() override {
		destroy();
	}

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
	bool set_accept_depth(socket_t listen_socket, uint32_t depth) override;
	bool set_accept_depth_ex(socket_t listen_socket, uint32_t depth, bool aggressive_cancel) override;
	bool set_accept_autotune(socket_t listen_socket, const AcceptAutotuneConfig &cfg) override;

	// Метрики
	NetStats get_stats() override;
	void reset_stats() override;

	// Threadless loop integration
	bool loop_once(uint32_t timeout_ms) override;
  protected:
	void wake() override {
		if (iocp_) {
			PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
						stats_.wakes_posted.fetch_add(1, std::memory_order_relaxed);
		}
	}

  private:
		struct StatsAtomic {
				std::atomic<uint64_t> accepts_ok{0};
				std::atomic<uint64_t> accepts_fail{0};
				std::atomic<uint64_t> connects_ok{0};
				std::atomic<uint64_t> connects_fail{0};
				std::atomic<uint64_t> reads{0};
				std::atomic<uint64_t> bytes_read{0};
				std::atomic<uint64_t> writes{0};
				std::atomic<uint64_t> bytes_written{0};
				std::atomic<uint64_t> closes{0};
				std::atomic<uint64_t> timeouts{0};
				std::atomic<uint64_t> gqcs_errors{0};
				std::atomic<uint64_t> user_events{0};
				std::atomic<uint64_t> wakes_posted{0};
				std::atomic<uint64_t> autotune_up{0};
				std::atomic<uint64_t> autotune_down{0};
				std::atomic<uint64_t> peak_connections{0};
				std::atomic<uint64_t> send_enqueued_bytes{0};
				std::atomic<uint64_t> send_dequeued_bytes{0};
				std::atomic<uint64_t> send_dropped_bytes{0};
		};
	struct SockState {
		char *buf{nullptr};
		size_t buf_size{0};
		ReadCallback read_cb{};
		bool sending{false};
		bool paused{false};
		bool connecting{false};
		// Write queues: send_q holds in-flight buffer (must remain stable during WSASend),
		// pend_q accumulates new writes while a send is in progress.
		std::vector<char> send_q;
		std::vector<char> pend_q;
		uint32_t timeout_ms{0};
		uint64_t next_deadline_ms{0}; // absolute deadline (GetTickCount64) for read timeout, 0 if disabled
		PerIo *pending_read{nullptr};
	};

	void post_read(socket_t fd);
	void post_write_unlocked(socket_t fd, SockState &st);
	void post_accept(socket_t listen_fd);
	void process_expired_timeouts(uint64_t now_ms, bool &made_progress);
	void process_autotune(uint64_t now_ms, bool &made_progress);
	static uint64_t now_ms() { return GetTickCount64(); }

	NetCallbacks cbs_{};
	bool winsock_inited_{false};
	HANDLE iocp_{nullptr};

	std::unordered_map<socket_t, SockState> sockets_;
	std::unordered_set<socket_t> listeners_;
	// Per-listener accept depth management
	std::unordered_map<socket_t, uint32_t> target_accept_depth_;
	std::unordered_map<socket_t, uint32_t> outstanding_accepts_;
	std::unordered_map<socket_t, std::vector<PerIo *>> pending_accepts_;
	// Autotune per-listener state (threadless)
	struct AutoState {
		AcceptAutotuneConfig cfg{};
		bool enabled{false};
		uint32_t accepted_in_window{0};
		uint64_t window_deadline_ms{0};
	};
	std::unordered_map<socket_t, AutoState> autotune_;

	std::atomic<uint32_t> max_conn_{0};
	std::atomic<uint32_t> cur_conn_{0};
	StatsAtomic stats_{};
	LPFN_CONNECTEX ConnectEx_{nullptr};
	LPFN_ACCEPTEX AcceptEx_{nullptr};
	static constexpr int kAcceptDepth =
#ifdef IOCP_ACCEPT_DEPTH
		IOCP_ACCEPT_DEPTH
#else
		4
#endif
		; // default number of concurrent AcceptEx
};

bool IocpEngine::init(const NetCallbacks &cbs) {
	cbs_ = cbs;
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;
	winsock_inited_ = true;
	iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
	if (!iocp_) {
		// roll back WSA if IOCP creation failed
		IO_LOG_ERR("CreateIoCompletionPort failed: GLE=%lu", GetLastError());
		WSACleanup();
		winsock_inited_ = false;
		return false;
	}
	SOCKET tmp = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (tmp != INVALID_SOCKET) {
		DWORD bytes = 0;
		GUID guid1 = WSAID_CONNECTEX;
		WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid1, sizeof(guid1), &ConnectEx_, sizeof(ConnectEx_),
				 &bytes, NULL, NULL);
		GUID guid2 = WSAID_ACCEPTEX;
		WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid2, sizeof(guid2), &AcceptEx_, sizeof(AcceptEx_),
				 &bytes, NULL, NULL);
		closesocket(tmp);
	}
	return true;
}

void IocpEngine::destroy() {
	// Cancel outstanding I/O, close sockets/listeners, free state
	// Cancel pending accepts
	for (const auto &lfd : listeners_) {
		auto it = pending_accepts_.find(lfd);
		if (it != pending_accepts_.end()) {
			for (PerIo *p : it->second) {
				// Cancel on the accept socket handle and close it
				CancelIoEx((HANDLE)(uintptr_t)p->fd, &p->ol);
				closesocket((SOCKET)p->fd);
				delete p;
			}
		}
	}
	for (const auto &kv : sockets_) {
		CancelIoEx((HANDLE)(uintptr_t)kv.first, nullptr);
	}
	// Close and clear
	for (auto &kv : sockets_)
		closesocket((SOCKET)kv.first);
	sockets_.clear();
	for (auto lfd : listeners_)
		closesocket((SOCKET)lfd);
	listeners_.clear();
	autotune_.clear();
	if (iocp_) {
		CloseHandle(iocp_);
		iocp_ = nullptr;
	}
	if (winsock_inited_) {
		WSACleanup();
		winsock_inited_ = false;
	}
}

bool IocpEngine::add_socket(socket_t fd, char *buffer, size_t buffer_size, ReadCallback cb) {
	auto it = sockets_.find(fd);
	if (it == sockets_.end()) {
		HANDLE h = CreateIoCompletionPort((HANDLE)(uintptr_t)fd, iocp_, (ULONG_PTR)fd, 0);
		if (!h) {
			IO_LOG_ERR("CreateIoCompletionPort(sock=%d) failed: GLE=%lu", (int)fd, GetLastError());
			return false;
		}
		u_long nb = 1;
		ioctlsocket((SOCKET)fd, FIONBIO, &nb);
		SockState st{};
		st.buf = buffer;
		st.buf_size = buffer_size;
		st.read_cb = std::move(cb);
		sockets_[fd] = std::move(st);
	} else {
		// Socket was pre-registered (e.g., via AcceptEx path); just attach buffer and callback
		it->second.buf = buffer;
		it->second.buf_size = buffer_size;
		it->second.read_cb = std::move(cb);
		// ensure non-blocking
		u_long nb = 1;
		ioctlsocket((SOCKET)fd, FIONBIO, &nb);
	}
	// Post initial read unless we are in connecting state
	auto sit = sockets_.find(fd);
	if (sit != sockets_.end() && !sit->second.connecting) {
		post_read(fd);
	}
	return true;
}

bool IocpEngine::delete_socket(socket_t fd) {
	auto it = sockets_.find(fd);
	if (it != sockets_.end()) {
		// best-effort cancel pending I/O operations
		CancelIoEx((HANDLE)(uintptr_t)fd, nullptr);
		// считаем несданные байты как дропнутые, раз сокет больше не управляется движком
		auto &st = it->second;
		uint64_t drop = (uint64_t)st.send_q.size() + (uint64_t)st.pend_q.size();
		if (drop) stats_.send_dropped_bytes.fetch_add(drop, std::memory_order_relaxed);
		sockets_.erase(it);
		if (cur_conn_ > 0)
			cur_conn_--;
	}
	return true;
}

bool IocpEngine::connect(socket_t fd, const char *host, uint16_t port, bool async) {
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (InetPtonA(AF_INET, host, &addr.sin_addr) != 1)
		return false;
	if (!async || !ConnectEx_) {
		u_long nb0 = 0;
		ioctlsocket((SOCKET)fd, FIONBIO, &nb0);
		int r = ::connect((SOCKET)fd, (sockaddr *)&addr, sizeof(addr));
		u_long nb1 = 1;
		ioctlsocket((SOCKET)fd, FIONBIO, &nb1);
		if (r == 0) {
			stats_.connects_ok.fetch_add(1, std::memory_order_relaxed);
			if (cbs_.on_accept)
				cbs_.on_accept(fd);
			post_read(fd);
			return true;
		}
		stats_.connects_fail.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	sockaddr_in local{};
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = 0;
	::bind((SOCKET)fd, (sockaddr *)&local, sizeof(local));
	// mark as connecting and cancel any early posted read
	if (auto it = sockets_.find(fd); it != sockets_.end()) {
		it->second.connecting = true;
		if (it->second.pending_read) {
			CancelIoEx((HANDLE)(uintptr_t)fd, &it->second.pending_read->ol);
			it->second.pending_read = nullptr;
		}
	}
	PerIo *p = new PerIo();
	p->op = OpType::Connect;
	p->fd = fd;
	BOOL ok = ConnectEx_((SOCKET)fd, (sockaddr *)&addr, sizeof(addr), NULL, 0, NULL, &p->ol);
	if (!ok) {
		int e = WSAGetLastError();
		if (e != WSA_IO_PENDING) {
			IO_LOG_ERR("ConnectEx(fd=%d) failed immediately: WSA=%d", (int)fd, e);
			delete p;
			return false;
		}
	}
	return true;
}

bool IocpEngine::disconnect(socket_t fd) {
	if (auto it = sockets_.find(fd); it != sockets_.end()) {
		CancelIoEx((HANDLE)(uintptr_t)fd, nullptr);
		// учтем несданные байты перед закрытием
		auto &st = it->second;
		uint64_t drop = (uint64_t)st.send_q.size() + (uint64_t)st.pend_q.size();
		if (drop) stats_.send_dropped_bytes.fetch_add(drop, std::memory_order_relaxed);
		sockets_.erase(it);
	}
	shutdown((SOCKET)fd, SD_BOTH);
	closesocket((SOCKET)fd);
	IO_LOG_DBG("close: socket=%p", (void *)(uintptr_t)fd);
	if (cbs_.on_close)
		cbs_.on_close(fd);
	stats_.closes.fetch_add(1, std::memory_order_relaxed);
	return true;
}

bool IocpEngine::accept(socket_t listen_socket, bool async, uint32_t max_connections) {
	(void)async;
	max_conn_ = max_connections;
	CreateIoCompletionPort((HANDLE)(uintptr_t)listen_socket, iocp_, (ULONG_PTR)listen_socket, 0);
	listeners_.insert(listen_socket);
	if (target_accept_depth_.find(listen_socket) == target_accept_depth_.end())
		target_accept_depth_[listen_socket] = kAcceptDepth;
	outstanding_accepts_[listen_socket] = 0;
	if (autotune_.find(listen_socket) != autotune_.end()) {
		auto &as = autotune_[listen_socket];
		if (as.enabled)
			as.window_deadline_ms = now_ms() + as.cfg.window_ms;
		as.accepted_in_window = 0;
	}
	// prime accept pipeline to target depth
	uint32_t depth = kAcceptDepth;
	depth = target_accept_depth_[listen_socket];
	for (uint32_t i = 0; i < depth; ++i)
		post_accept(listen_socket);
	return true;
}

bool IocpEngine::write(socket_t fd, const char *data, size_t data_size) {
	auto it = sockets_.find(fd);
	if (it == sockets_.end())
		return false;
	auto &st = it->second;
	if (data_size == 0)
		return true;
	stats_.send_enqueued_bytes.fetch_add((uint64_t)data_size, std::memory_order_relaxed);
	if (st.sending) {
		st.pend_q.insert(st.pend_q.end(), data, data + data_size);
	} else {
		st.send_q.insert(st.send_q.end(), data, data + data_size);
		st.sending = true;
		post_write_unlocked(fd, st);
	}
	return true;
}

bool IocpEngine::post(uint32_t val) {
	PerIo *p = new PerIo();
	p->op = OpType::User;
	p->user_val = val;
	BOOL ok = PostQueuedCompletionStatus(iocp_, val, 0, &p->ol);
	if (!ok) {
		IO_LOG_ERR("PostQueuedCompletionStatus(user=%u) failed: GLE=%lu", val, GetLastError());
		delete p;
		return false;
	}
	stats_.user_events.fetch_add(1, std::memory_order_relaxed);
	return true;
}

bool IocpEngine::set_read_timeout(socket_t fd, uint32_t timeout_ms) {
	auto it = sockets_.find(fd);
	if (it == sockets_.end())
		return false;
	it->second.timeout_ms = timeout_ms;
	if (timeout_ms == 0) {
		it->second.next_deadline_ms = 0;
		return true;
	}
	it->second.next_deadline_ms = now_ms() + timeout_ms;
	return true;
}

bool IocpEngine::pause_read(socket_t fd) {
	auto it = sockets_.find(fd);
	if (it == sockets_.end())
		return false;
	it->second.paused = true;
	if (it->second.pending_read) {
		// Cancel only the outstanding read; completion will clear pending_read
		CancelIoEx((HANDLE)(uintptr_t)fd, &it->second.pending_read->ol);
	}
	return true;
}

bool IocpEngine::resume_read(socket_t fd) {
	auto it = sockets_.find(fd);
	if (it == sockets_.end())
		return false;
	it->second.paused = false;
	if (it->second.pending_read)
		return true;
	// Post a new read without holding the lock
	post_read(fd);
	return true;
}

bool IocpEngine::loop_once(uint32_t timeout_ms) {
	// Determine next deadline among socket timeouts and autotune windows
	uint64_t now = now_ms();
	uint64_t earliest = 0; // 0 means none
	for (auto &kv : sockets_) {
		const auto &st = kv.second;
		if (st.next_deadline_ms) {
			uint64_t d = st.next_deadline_ms;
			if (earliest == 0 || d < earliest)
				earliest = d;
		}
	}
	for (auto &kv : autotune_) {
		const auto &as = kv.second;
		if (as.enabled && as.window_deadline_ms) {
			uint64_t d = as.window_deadline_ms;
			if (earliest == 0 || d < earliest)
				earliest = d;
		}
	}
	uint32_t wait = timeout_ms;
	if (earliest) {
		uint64_t delta = (now < earliest) ? (earliest - now) : 0;
		if (timeout_ms == 0xFFFFFFFFu) {
			wait = static_cast<uint32_t>(std::min<uint64_t>(delta, 0xFFFFFFFFull));
		} else {
			wait = std::min<uint32_t>(wait, static_cast<uint32_t>(std::min<uint64_t>(delta, 0xFFFFFFFFull)));
		}
	}

	DWORD bytes = 0;
	ULONG_PTR key = 0;
	OVERLAPPED *pol = nullptr;
	BOOL ok = GetQueuedCompletionStatus(iocp_, &bytes, &key, &pol, wait);
	bool made_progress = false;
	if (!pol) {
		if (!ok) {
			DWORD gle = GetLastError();
			if (gle != WAIT_TIMEOUT) {
				IO_LOG_ERR("GetQueuedCompletionStatus error: GLE=%lu", gle);
				stats_.gqcs_errors.fetch_add(1, std::memory_order_relaxed);
			}
		}
		// process timers regardless (timeout or spurious wake)
		now = now_ms();
		process_expired_timeouts(now, made_progress);
		process_autotune(now, made_progress);
		return made_progress;
	}
		PerIo *p = reinterpret_cast<PerIo *>(pol);
		if (!p)
			return false;
		DWORD err = ok ? 0 : GetLastError();
		switch (p->op) {
		case OpType::User: {
			if (cbs_.on_user)
				cbs_.on_user(p->user_val);
			delete p;
			return true;
		}
		case OpType::Accept: {
			socket_t listen_fd = p->listen_fd;
			if (!ok) {
				socket_t sock = p->fd;
				auto &vec = pending_accepts_[listen_fd];
				vec.erase(std::remove(vec.begin(), vec.end(), p), vec.end());
				if (outstanding_accepts_[listen_fd] > 0)
					outstanding_accepts_[listen_fd]--;
				if (sock != kInvalidSocket)
					closesocket((SOCKET)sock);
				stats_.accepts_fail.fetch_add(1, std::memory_order_relaxed);
				uint32_t target = 0, out = 0;
				target = target_accept_depth_[listen_fd];
				out = outstanding_accepts_[listen_fd];
				delete p;
				if (out < target)
					post_accept(listen_fd);
				return true;
			}
			socket_t newfd = p->fd; // accepted socket
			setsockopt((SOCKET)newfd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&listen_fd,
					   sizeof(listen_fd));
			{
				auto &vec = pending_accepts_[listen_fd];
				vec.erase(std::remove(vec.begin(), vec.end(), p), vec.end());
				if (auto it = autotune_.find(listen_fd); it != autotune_.end())
					it->second.accepted_in_window++;
				if (max_conn_ == 0 || cur_conn_ < max_conn_) {
					cur_conn_++;
					// обновляем пиковое число подключений
					auto cur = (uint64_t)cur_conn_.load(std::memory_order_relaxed);
					uint64_t prev = stats_.peak_connections.load(std::memory_order_relaxed);
					while (cur > prev && !stats_.peak_connections.compare_exchange_weak(prev, cur, std::memory_order_relaxed)) {
						// retry with updated prev
					}
				} else {
					closesocket((SOCKET)newfd);
					newfd = -1;
				}
			}
			if (newfd != (socket_t)INVALID_SOCKET) {
				stats_.accepts_ok.fetch_add(1, std::memory_order_relaxed);
				// Предрегистрация клиента в sockets_ с пустым буфером до add_socket
				{
					SockState st{}; // defaults are fine (no pending read yet)
					sockets_.emplace(newfd, std::move(st));
				}
				IO_LOG_DBG("accept: new socket=%p", (void *)(uintptr_t)newfd);
				if (cbs_.on_accept)
					cbs_.on_accept(newfd);
			}
			uint32_t target = 0, out = 0;
			if (outstanding_accepts_[listen_fd] > 0)
				outstanding_accepts_[listen_fd]--;
			target = target_accept_depth_[listen_fd];
			out = outstanding_accepts_[listen_fd];
			if (out < target)
				post_accept(listen_fd);
			delete p;
			return true;
		}
		case OpType::Connect: {
			socket_t fd = p->fd;
			if (!ok) {
				DWORD gle = GetLastError();
				IO_LOG_ERR("ConnectEx completion failed socket=%p GLE=%lu", (void *)(uintptr_t)fd, gle);
				delete p;
				if (err == ERROR_OPERATION_ABORTED)
					return true;
				stats_.connects_fail.fetch_add(1, std::memory_order_relaxed);
				if (cbs_.on_close)
					cbs_.on_close(fd);
				shutdown((SOCKET)fd, SD_BOTH);
				closesocket((SOCKET)fd);
				sockets_.erase(fd);
				stats_.closes.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			delete p;
			setsockopt((SOCKET)fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
			IO_LOG_DBG("connect: established socket=%p", (void *)(uintptr_t)fd);
			stats_.connects_ok.fetch_add(1, std::memory_order_relaxed);
			if (cbs_.on_accept)
				cbs_.on_accept(fd);
			// clear connecting flag and post initial read now
			if (auto it = sockets_.find(fd); it != sockets_.end()) {
				it->second.connecting = false;
			}
			post_read(fd);
			return true;
		}
		case OpType::Read: {
			socket_t fd = p->fd;
			// clear pending_read pointer if it refers to this overlapped
			if (auto it = sockets_.find(fd); it != sockets_.end() && it->second.pending_read == p)
				it->second.pending_read = nullptr;
			if (!ok) {
				delete p;
				if (err == ERROR_OPERATION_ABORTED)
					return true;
				if (cbs_.on_close)
					cbs_.on_close(fd);
				// учтем дропающиеся несданные байты
				if (auto it2 = sockets_.find(fd); it2 != sockets_.end()) {
					auto &st = it2->second;
					uint64_t drop = (uint64_t)st.send_q.size() + (uint64_t)st.pend_q.size();
					if (drop) stats_.send_dropped_bytes.fetch_add(drop, std::memory_order_relaxed);
				}
				closesocket((SOCKET)fd);
				sockets_.erase(fd);
				if (cur_conn_ > 0)
					cur_conn_--;
				stats_.closes.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			if (bytes == 0) {
				delete p;
				if (cbs_.on_close)
					cbs_.on_close(fd);
				if (auto it2 = sockets_.find(fd); it2 != sockets_.end()) {
					auto &st = it2->second;
					uint64_t drop = (uint64_t)st.send_q.size() + (uint64_t)st.pend_q.size();
					if (drop) stats_.send_dropped_bytes.fetch_add(drop, std::memory_order_relaxed);
				}
				closesocket((SOCKET)fd);
				sockets_.erase(fd);
				if (cur_conn_ > 0)
					cur_conn_--;
				stats_.closes.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			ReadCallback cb;
			char *buf = nullptr;
			size_t sz = 0;
			uint32_t tms = 0;
			if (auto it = sockets_.find(fd); it != sockets_.end()) {
				cb = it->second.read_cb;
				buf = it->second.buf;
				sz = (size_t)bytes;
				tms = it->second.timeout_ms;
			}
			stats_.reads.fetch_add(1, std::memory_order_relaxed);
			stats_.bytes_read.fetch_add((uint64_t)bytes, std::memory_order_relaxed);
			if (cb)
				cb(fd, buf, sz);
			if (tms > 0) {
				if (auto it = sockets_.find(fd); it != sockets_.end())
					it->second.next_deadline_ms = now_ms() + tms;
			}
			post_read(fd);
			delete p;
			return true;
		}
		case OpType::Write: {
			socket_t fd = p->fd;
			if (!ok) {
				delete p;
				if (err == ERROR_OPERATION_ABORTED)
					return true;
				if (cbs_.on_close)
					cbs_.on_close(fd);
				if (auto it2 = sockets_.find(fd); it2 != sockets_.end()) {
					auto &st = it2->second;
					uint64_t drop = (uint64_t)st.send_q.size() + (uint64_t)st.pend_q.size();
					if (drop) stats_.send_dropped_bytes.fetch_add(drop, std::memory_order_relaxed);
				}
				closesocket((SOCKET)fd);
				sockets_.erase(fd);
				if (cur_conn_ > 0)
					cur_conn_--;
				stats_.closes.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			size_t wrote = bytes;
			delete p;
			stats_.writes.fetch_add(1, std::memory_order_relaxed);
			stats_.bytes_written.fetch_add((uint64_t)wrote, std::memory_order_relaxed);
			stats_.send_dequeued_bytes.fetch_add((uint64_t)wrote, std::memory_order_relaxed);
			if (cbs_.on_write)
				cbs_.on_write(fd, wrote);
			auto it = sockets_.find(fd);
			if (it != sockets_.end()) {
				auto &st = it->second;
				if (wrote > 0) {
					if (wrote >= st.send_q.size()) {
						st.send_q.clear();
					} else {
						st.send_q.erase(st.send_q.begin(), st.send_q.begin() + wrote);
					}
				}
				if (!st.send_q.empty()) {
					post_write_unlocked(fd, st);
				} else if (!st.pend_q.empty()) {
					st.send_q.swap(st.pend_q);
					post_write_unlocked(fd, st);
				} else {
					st.sending = false;
				}
			}
			return true;
		}
		}
	return false;
}

void IocpEngine::post_read(socket_t fd) {
	auto it = sockets_.find(fd);
	if (it == sockets_.end())
		return;
	auto &st = it->second;
	if (st.paused) {
		return;
	}
	if (st.pending_read) {
		return;
	}
	PerIo *p = new PerIo();
	p->op = OpType::Read;
	p->fd = fd;
	p->wbuf.buf = st.buf;
	p->wbuf.len = (ULONG)st.buf_size;
	DWORD flags = 0;
	DWORD recvd = 0;
	st.pending_read = p;
	int r = WSARecv((SOCKET)fd, &p->wbuf, 1, &recvd, &flags, &p->ol, NULL);
	if (r != 0) {
		int e = WSAGetLastError();
		if (e != WSA_IO_PENDING) {
			IO_LOG_ERR("WSARecv(fd=%d) failed: WSA=%d", (int)fd, e);
			st.pending_read = nullptr;
			delete p;
		}
	}
}

void IocpEngine::post_write_unlocked(socket_t fd, SockState &st) {
	if (st.send_q.empty())
		return;
	PerIo *p = new PerIo();
	p->op = OpType::Write;
	p->fd = fd;
	p->wbuf.buf = st.send_q.data();
	p->wbuf.len = (ULONG)st.send_q.size();
	DWORD sent = 0;
	int r = WSASend((SOCKET)fd, &p->wbuf, 1, &sent, 0, &p->ol, NULL);
	if (r != 0) {
		int e = WSAGetLastError();
		if (e != WSA_IO_PENDING) {
			IO_LOG_ERR("WSASend(fd=%d) failed: WSA=%d", (int)fd, e);
			delete p;
			st.sending = false;
		}
	}
}

void IocpEngine::post_accept(socket_t listen_fd) {
	SOCKET as = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (as == INVALID_SOCKET)
		return;
	CreateIoCompletionPort((HANDLE)as, iocp_, (ULONG_PTR)as, 0);
	PerIo *p = new PerIo();
	p->op = OpType::Accept;
	p->fd = (socket_t)as;
	p->listen_fd = listen_fd;
	p->dynbuf.resize(2 * (sizeof(sockaddr_storage) + 16));
	DWORD bytes = 0;
	BOOL ok = FALSE;
	if (AcceptEx_) {
		ok = AcceptEx_((SOCKET)listen_fd, as, p->dynbuf.data(), 0, (DWORD)(sizeof(sockaddr_storage) + 16),
					   (DWORD)(sizeof(sockaddr_storage) + 16), &bytes, &p->ol);
	}
	if (!ok) {
		int e = WSAGetLastError();
		if (e != WSA_IO_PENDING) {
			IO_LOG_ERR("AcceptEx(listen=%d) failed: WSA=%d", (int)listen_fd, e);
			delete p;
			closesocket(as);
			return;
		}
	}
	outstanding_accepts_[listen_fd]++;
	pending_accepts_[listen_fd].push_back(p);
	IO_LOG_DBG("post_accept: listen=%p outstanding=%u", (void *)(uintptr_t)listen_fd,
			   outstanding_accepts_[listen_fd]);
}

bool IocpEngine::set_accept_depth(socket_t listen_socket, uint32_t depth) {
	if (depth == 0)
		depth = 1;
	uint32_t to_post = 0;
	if (listeners_.find(listen_socket) == listeners_.end()) {
		// listener ещё не активирован через accept(); просто запомним цель
		target_accept_depth_[listen_socket] = depth;
		outstanding_accepts_[listen_socket] = 0;
		return true;
	}
	target_accept_depth_[listen_socket] = depth;
	{
		uint32_t out = outstanding_accepts_[listen_socket];
		to_post = (depth > out) ? (depth - out) : 0;
	}
	// Дозакладываем недостающие AcceptEx вне лока
	for (uint32_t i = 0; i < to_post; ++i)
		post_accept(listen_socket);
	return true;
}

bool IocpEngine::set_accept_depth_ex(socket_t listen_socket, uint32_t depth, bool aggressive_cancel) {
	if (depth == 0)
		depth = 1;
	uint32_t to_post = 0;
	std::vector<PerIo *> to_cancel;
	if (listeners_.find(listen_socket) == listeners_.end()) {
		target_accept_depth_[listen_socket] = depth;
		outstanding_accepts_[listen_socket] = 0;
		return true;
	}
	target_accept_depth_[listen_socket] = depth;
	{
		uint32_t out = outstanding_accepts_[listen_socket];
		if (depth > out) {
			to_post = depth - out;
		} else if (aggressive_cancel && out > depth) {
			auto &vec = pending_accepts_[listen_socket];
			size_t need_cancel = static_cast<size_t>(out - depth);
			for (size_t i = 0; i < vec.size() && to_cancel.size() < need_cancel; ++i) {
				to_cancel.push_back(vec[i]);
			}
		}
	}
	for (uint32_t i = 0; i < to_post; ++i)
		post_accept(listen_socket);
	if (aggressive_cancel) {
		for (auto *p : to_cancel) {
			// Cancel on the accept socket itself
			CancelIoEx((HANDLE)(uintptr_t)p->fd, &p->ol);
		}
	}
	return true;
}

bool IocpEngine::set_accept_autotune(socket_t listen_socket, const AcceptAutotuneConfig &cfg) {
	auto &as = autotune_[listen_socket];
	as.cfg = cfg;
	as.enabled = cfg.enabled;
	as.accepted_in_window = 0;
	as.window_deadline_ms = cfg.enabled ? (now_ms() + cfg.window_ms) : 0;
	return true;
}

void IocpEngine::process_expired_timeouts(uint64_t now, bool &made_progress) {
	std::vector<socket_t> to_close;
	for (auto &kv : sockets_) {
		auto fd = kv.first;
		auto &st = kv.second;
		if (st.next_deadline_ms && now >= st.next_deadline_ms) {
			to_close.push_back(fd);
		}
	}
	for (auto fd : to_close) {
		if (cbs_.on_close)
			cbs_.on_close(fd);
		if (auto it = sockets_.find(fd); it != sockets_.end()) {
			auto &st = it->second;
			uint64_t drop = (uint64_t)st.send_q.size() + (uint64_t)st.pend_q.size();
			if (drop) stats_.send_dropped_bytes.fetch_add(drop, std::memory_order_relaxed);
		}
		shutdown((SOCKET)fd, SD_BOTH);
		closesocket((SOCKET)fd);
		sockets_.erase(fd);
		if (cur_conn_ > 0)
			cur_conn_--;
		stats_.closes.fetch_add(1, std::memory_order_relaxed);
		stats_.timeouts.fetch_add(1, std::memory_order_relaxed);
		made_progress = true;
	}
}

void IocpEngine::process_autotune(uint64_t now, bool &made_progress) {
	for (auto &kv : autotune_) {
		socket_t lfd = kv.first;
		auto &as = kv.second;
		if (!as.enabled || as.window_deadline_ms == 0)
			continue;
		if (now < as.window_deadline_ms)
			continue;
		// window elapsed
		auto cfg = as.cfg;
		uint32_t accepted = as.accepted_in_window;
		as.accepted_in_window = 0;
		as.window_deadline_ms = now + cfg.window_ms;
		uint32_t curDepth = target_accept_depth_[lfd];
		uint32_t next = curDepth;
		if (accepted >= cfg.high_watermark) {
			next = std::min<uint32_t>(cfg.max_depth, curDepth + cfg.up_step);
		} else if (accepted <= cfg.low_watermark) {
			if (curDepth > cfg.min_depth)
				next = std::max<uint32_t>(cfg.min_depth, curDepth - cfg.down_step);
		}
		if (next != curDepth) {
			bool aggressive = (next < curDepth) && cfg.aggressive_cancel_on_downscale;
			set_accept_depth_ex(lfd, next, aggressive);
			if (next > curDepth) stats_.autotune_up.fetch_add(1, std::memory_order_relaxed);
			else stats_.autotune_down.fetch_add(1, std::memory_order_relaxed);
			made_progress = true;
		}
	}
}

NetStats IocpEngine::get_stats() {
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
	s.gqcs_errors = stats_.gqcs_errors.load(std::memory_order_relaxed);
	s.user_events = stats_.user_events.load(std::memory_order_relaxed);
	s.wakes_posted = stats_.wakes_posted.load(std::memory_order_relaxed);
	s.autotune_up = stats_.autotune_up.load(std::memory_order_relaxed);
	s.autotune_down = stats_.autotune_down.load(std::memory_order_relaxed);
	s.current_connections = (uint64_t)cur_conn_.load(std::memory_order_relaxed);
	s.peak_connections = stats_.peak_connections.load(std::memory_order_relaxed);
	// Sum outstanding accepts across all listeners
	uint64_t out_acc = 0;
	for (auto &kv : outstanding_accepts_) out_acc += kv.second;
	s.outstanding_accepts = out_acc;
	s.send_enqueued_bytes = stats_.send_enqueued_bytes.load(std::memory_order_relaxed);
	s.send_dequeued_bytes = stats_.send_dequeued_bytes.load(std::memory_order_relaxed);
	uint64_t deq = s.send_dequeued_bytes;
	s.send_backlog_bytes = (s.send_enqueued_bytes >= deq) ? (s.send_enqueued_bytes - deq) : 0;
	s.send_dropped_bytes = stats_.send_dropped_bytes.load(std::memory_order_relaxed);
	return s;
}

void IocpEngine::reset_stats() {
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
	stats_.gqcs_errors.store(0, std::memory_order_relaxed);
	stats_.user_events.store(0, std::memory_order_relaxed);
	stats_.wakes_posted.store(0, std::memory_order_relaxed);
	stats_.autotune_up.store(0, std::memory_order_relaxed);
	stats_.autotune_down.store(0, std::memory_order_relaxed);
	// пиковое значение имеет смысл сбросить до текущего количества соединений
	stats_.peak_connections.store((uint64_t)cur_conn_.load(std::memory_order_relaxed), std::memory_order_relaxed);
	stats_.send_enqueued_bytes.store(0, std::memory_order_relaxed);
	stats_.send_dequeued_bytes.store(0, std::memory_order_relaxed);
	stats_.send_dropped_bytes.store(0, std::memory_order_relaxed);
}

INetEngine *create_engine_iocp() {
	return new IocpEngine();
}

} // namespace io

#endif
