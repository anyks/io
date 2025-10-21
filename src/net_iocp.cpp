// IOCP backend (Windows)
#include "io/net.hpp"
#if defined(IO_ENGINE_IOCP)
#define NOMINMAX
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mswsock.h>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

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

  private:
	struct TimerCtx {
		IocpEngine *self;
		socket_t fd;
	};
	struct ListenerTimerCtx {
		IocpEngine *self;
		socket_t listen_fd;
	};
	struct SockState {
		char *buf{nullptr};
		size_t buf_size{0};
		ReadCallback read_cb{};
		std::vector<char> out_queue;
		bool sending{false};
		bool paused{false};
		uint32_t timeout_ms{0};
		HANDLE timer{nullptr};
		TimerCtx *timer_ctx{nullptr};
		PerIo *pending_read{nullptr};
	};

	void loop();
	void post_read(socket_t fd);
	void post_write_unlocked(socket_t fd, SockState &st);
	void post_accept(socket_t listen_fd);
	static VOID CALLBACK on_socket_timer(PVOID param, BOOLEAN fired);
	static VOID CALLBACK on_autotune_timer(PVOID param, BOOLEAN fired);

	NetCallbacks cbs_{};
	HANDLE iocp_{nullptr};
	std::thread worker_{};
	std::atomic<bool> running_{false};
	std::mutex mtx_;
	std::unordered_map<socket_t, SockState> sockets_;
	std::unordered_set<socket_t> listeners_;
	// Per-listener accept depth management
	std::unordered_map<socket_t, uint32_t> target_accept_depth_;
	std::unordered_map<socket_t, uint32_t> outstanding_accepts_;
	std::unordered_map<socket_t, std::vector<PerIo *>> pending_accepts_;
	// Autotune per-listener state
	std::unordered_map<socket_t, AcceptAutotuneConfig> autotune_cfg_;
	std::unordered_map<socket_t, HANDLE> autotune_timer_;
	std::unordered_map<socket_t, uint32_t> accepted_in_window_;

	std::atomic<uint32_t> max_conn_{0};
	std::atomic<uint32_t> cur_conn_{0};
	LPFN_CONNECTEX ConnectEx_{nullptr};
	LPFN_ACCEPTEX AcceptEx_{nullptr};
	SOCKET accept_listen_{INVALID_SOCKET};
	HANDLE timer_queue_{nullptr};
	bool winsock_inited_{false};
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
		GUID guid = WSAID_CONNECTEX;
		if (WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &ConnectEx_, sizeof(ConnectEx_),
					 &bytes, NULL, NULL) != 0) {
			IO_LOG_ERR("WSAIoctl(GET ConnectEx) failed: WSA=%d", WSAGetLastError());
		}
		GUID guid2 = WSAID_ACCEPTEX;
		if (WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid2, sizeof(guid2), &AcceptEx_, sizeof(AcceptEx_),
					 &bytes, NULL, NULL) != 0) {
			IO_LOG_ERR("WSAIoctl(GET AcceptEx) failed: WSA=%d", WSAGetLastError());
		}
		closesocket(tmp);
	}
	running_ = true;
	worker_ = std::thread(&IocpEngine::loop, this);
	return true;
}

void IocpEngine::destroy() {
	// Phase 1: cancel all outstanding I/O before stopping the worker
	{
		std::lock_guard<std::mutex> lk(mtx_);
		// Cancel all pending accepts (aggressively) and outstanding per-socket I/O
		for (const auto &lfd : listeners_) {
			auto it = pending_accepts_.find(lfd);
			if (it != pending_accepts_.end()) {
				for (PerIo *p : it->second) {
					// cancel specific AcceptEx overlapped on the listening socket
					CancelIoEx((HANDLE)(uintptr_t)lfd, &p->ol);
				}
			}
		}
		for (const auto &kv : sockets_) {
			// cancel all overlapped on this socket (reads/writes)
			CancelIoEx((HANDLE)(uintptr_t)kv.first, nullptr);
			// stop and delete per-socket timers
			if (kv.second.timer) {
				DeleteTimerQueueTimer(timer_queue_, kv.second.timer, INVALID_HANDLE_VALUE);
			}
			if (kv.second.timer_ctx) {
				delete kv.second.timer_ctx;
			}
		}
		// cancel autotune timers now to avoid callbacks during teardown
		for (auto &kv : autotune_timer_) {
			if (kv.second) {
				DeleteTimerQueueTimer(timer_queue_, kv.second, INVALID_HANDLE_VALUE);
			}
		}
	}
	// Phase 2: signal worker to exit after processing queued cancellations
	if (iocp_) {
		PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
	}
	running_ = false;
	if (worker_.joinable())
		worker_.join();
	{
		std::lock_guard<std::mutex> lk(mtx_);
		// Close all active sockets and listeners
		for (auto &kv : sockets_) {
			closesocket((SOCKET)kv.first);
		}
		for (auto lfd : listeners_) {
			closesocket((SOCKET)lfd);
		}
		sockets_.clear();
		listeners_.clear();
		autotune_timer_.clear();
		autotune_cfg_.clear();
		accepted_in_window_.clear();
	}
	if (timer_queue_) {
		DeleteTimerQueueEx(timer_queue_, INVALID_HANDLE_VALUE);
		timer_queue_ = nullptr;
	}
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
	HANDLE h = CreateIoCompletionPort((HANDLE)(uintptr_t)fd, iocp_, (ULONG_PTR)fd, 0);
	if (!h) {
		IO_LOG_ERR("CreateIoCompletionPort(sock=%d) failed: GLE=%lu", (int)fd, GetLastError());
		return false;
	}
	u_long nb = 1;
	ioctlsocket((SOCKET)fd, FIONBIO, &nb);
	std::lock_guard<std::mutex> lk(mtx_);
	SockState st{};
	st.buf = buffer;
	st.buf_size = buffer_size;
	st.read_cb = std::move(cb);
	sockets_[fd] = std::move(st);
	// Defer posting read until connection established or for already-connected sockets it's fine
	post_read(fd);
	return true;
}

bool IocpEngine::delete_socket(socket_t fd) {
	std::lock_guard<std::mutex> lk(mtx_);
	auto it = sockets_.find(fd);
	if (it != sockets_.end()) {
		// best-effort cancel pending I/O operations
		CancelIoEx((HANDLE)(uintptr_t)fd, nullptr);
		if (it->second.timer) {
			DeleteTimerQueueTimer(timer_queue_, it->second.timer, INVALID_HANDLE_VALUE);
			it->second.timer = nullptr;
		}
		if (it->second.timer_ctx) {
			delete it->second.timer_ctx;
			it->second.timer_ctx = nullptr;
		}
		sockets_.erase(it);
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
			if (cbs_.on_accept)
				cbs_.on_accept(fd);
			post_read(fd);
			return true;
		}
		return false;
	}
	sockaddr_in local{};
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = 0;
	::bind((SOCKET)fd, (sockaddr *)&local, sizeof(local));
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
	{
		std::lock_guard<std::mutex> lk(mtx_);
		auto it = sockets_.find(fd);
		if (it != sockets_.end()) {
			CancelIoEx((HANDLE)(uintptr_t)fd, nullptr);
			if (it->second.timer) {
				DeleteTimerQueueTimer(timer_queue_, it->second.timer, INVALID_HANDLE_VALUE);
				it->second.timer = nullptr;
			}
			if (it->second.timer_ctx) {
				delete it->second.timer_ctx;
				it->second.timer_ctx = nullptr;
			}
			sockets_.erase(it);
		}
	}
	shutdown((SOCKET)fd, SD_BOTH);
	closesocket((SOCKET)fd);
	IO_LOG_DBG("close: socket=%p", (void *)(uintptr_t)fd);
	if (cbs_.on_close)
		cbs_.on_close(fd);
	return true;
}

bool IocpEngine::accept(socket_t listen_socket, bool async, uint32_t max_connections) {
	(void)async;
	max_conn_ = max_connections;
	CreateIoCompletionPort((HANDLE)(uintptr_t)listen_socket, iocp_, (ULONG_PTR)listen_socket, 0);
	{
		std::lock_guard<std::mutex> lk(mtx_);
		listeners_.insert(listen_socket);
		if (target_accept_depth_.find(listen_socket) == target_accept_depth_.end()) {
			target_accept_depth_[listen_socket] = kAcceptDepth;
		}
		outstanding_accepts_[listen_socket] = 0;
		accepted_in_window_[listen_socket] = 0;
	}
	// prime accept pipeline to target depth
	uint32_t depth = kAcceptDepth;
	{
		std::lock_guard<std::mutex> lk(mtx_);
		depth = target_accept_depth_[listen_socket];
	}
	for (uint32_t i = 0; i < depth; ++i)
		post_accept(listen_socket);
	return true;
}

bool IocpEngine::write(socket_t fd, const char *data, size_t data_size) {
	std::lock_guard<std::mutex> lk(mtx_);
	auto it = sockets_.find(fd);
	if (it == sockets_.end())
		return false;
	auto &st = it->second;
	st.out_queue.insert(st.out_queue.end(), data, data + data_size);
	if (!st.sending) {
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
	return true;
}

bool IocpEngine::set_read_timeout(socket_t fd, uint32_t timeout_ms) {
	std::lock_guard<std::mutex> lk(mtx_);
	auto it = sockets_.find(fd);
	if (it == sockets_.end())
		return false;
	it->second.timeout_ms = timeout_ms;
	if (timeout_ms == 0) {
		if (it->second.timer) {
			DeleteTimerQueueTimer(timer_queue_, it->second.timer, INVALID_HANDLE_VALUE);
			it->second.timer = nullptr;
		}
		if (it->second.timer_ctx) {
			delete it->second.timer_ctx;
			it->second.timer_ctx = nullptr;
		}
		return true;
	}
	if (!timer_queue_)
		timer_queue_ = CreateTimerQueue();
	if (it->second.timer) {
		ChangeTimerQueueTimer(timer_queue_, it->second.timer, timeout_ms, 0);
	} else {
		HANDLE hTimer = nullptr;
		auto *ctx = new TimerCtx{this, fd};
		CreateTimerQueueTimer(&hTimer, timer_queue_, &IocpEngine::on_socket_timer, ctx, timeout_ms, 0,
							  WT_EXECUTEDEFAULT);
		it->second.timer = hTimer;
		it->second.timer_ctx = ctx;
	}
	return true;
}

bool IocpEngine::pause_read(socket_t fd) {
	std::lock_guard<std::mutex> lk(mtx_);
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
	{
		std::lock_guard<std::mutex> lk(mtx_);
		auto it = sockets_.find(fd);
		if (it == sockets_.end())
			return false;
		it->second.paused = false;
		if (it->second.pending_read) {
			// Already in-flight
			return true;
		}
	}
	// Post a new read without holding the lock
	post_read(fd);
	return true;
}

void IocpEngine::loop() {
	while (true) {
		DWORD bytes = 0;
		ULONG_PTR key = 0;
		OVERLAPPED *pol = nullptr;
		BOOL ok = GetQueuedCompletionStatus(iocp_, &bytes, &key, &pol, INFINITE);
		// Termination sentinel: null overlapped with zero key/bytes
		if (!pol) {
			if (!ok) {
				IO_LOG_ERR("GetQueuedCompletionStatus error: GLE=%lu", GetLastError());
				continue;
			}
			if (bytes == 0 && key == 0) {
				if (!running_)
					break;
				else
					continue;
			}
		}
		PerIo *p = reinterpret_cast<PerIo *>(pol);
		if (!p)
			continue;
		DWORD err = ok ? 0 : GetLastError();
		switch (p->op) {
		case OpType::User: {
			if (cbs_.on_user)
				cbs_.on_user(p->user_val);
			delete p;
			break;
		}
		case OpType::Accept: {
			socket_t listen_fd = (socket_t)accept_listen_;
			if (!ok) {
				socket_t sock = p->fd;
				{
					std::lock_guard<std::mutex> lk(mtx_);
					auto &vec = pending_accepts_[listen_fd];
					vec.erase(std::remove(vec.begin(), vec.end(), p), vec.end());
					if (outstanding_accepts_[listen_fd] > 0)
						outstanding_accepts_[listen_fd]--;
				}
				if (sock != -1)
					closesocket((SOCKET)sock);
				uint32_t target = 0, out = 0;
				{
					std::lock_guard<std::mutex> lk(mtx_);
					target = target_accept_depth_[listen_fd];
					out = outstanding_accepts_[listen_fd];
				}
				delete p;
				if (running_ && out < target)
					post_accept(listen_fd);
				break;
			}
			socket_t newfd = p->fd; // accepted socket
			setsockopt((SOCKET)newfd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&accept_listen_,
					   sizeof(accept_listen_));
			{
				std::lock_guard<std::mutex> lk(mtx_);
				auto &vec = pending_accepts_[listen_fd];
				vec.erase(std::remove(vec.begin(), vec.end(), p), vec.end());
				accepted_in_window_[listen_fd]++;
				if (max_conn_ == 0 || cur_conn_ < max_conn_) {
					cur_conn_++;
				} else {
					closesocket((SOCKET)newfd);
					newfd = -1;
				}
			}
			if (newfd != (socket_t)INVALID_SOCKET) {
				IO_LOG_DBG("accept: new socket=%p", (void *)(uintptr_t)newfd);
				if (cbs_.on_accept)
					cbs_.on_accept(newfd);
			}
			uint32_t target = 0, out = 0;
			{
				std::lock_guard<std::mutex> lk(mtx_);
				if (outstanding_accepts_[listen_fd] > 0)
					outstanding_accepts_[listen_fd]--;
				target = target_accept_depth_[listen_fd];
				out = outstanding_accepts_[listen_fd];
			}
			if (out < target)
				post_accept(listen_fd);
			delete p;
			break;
		}
		case OpType::Connect: {
			socket_t fd = p->fd;
			if (!ok) {
				DWORD gle = GetLastError();
				IO_LOG_ERR("ConnectEx completion failed socket=%p GLE=%lu", (void *)(uintptr_t)fd, gle);
				delete p;
				if (err == ERROR_OPERATION_ABORTED)
					break;
				if (cbs_.on_close)
					cbs_.on_close(fd);
				shutdown((SOCKET)fd, SD_BOTH);
				closesocket((SOCKET)fd);
				std::lock_guard<std::mutex> lk(mtx_);
				sockets_.erase(fd);
				break;
			}
			delete p;
			setsockopt((SOCKET)fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
			IO_LOG_DBG("connect: established socket=%p", (void *)(uintptr_t)fd);
			if (cbs_.on_accept)
				cbs_.on_accept(fd);
			post_read(fd);
			break;
		}
		case OpType::Read: {
			socket_t fd = p->fd;
			// clear pending_read pointer if it refers to this overlapped
			{
				std::lock_guard<std::mutex> lk(mtx_);
				auto it = sockets_.find(fd);
				if (it != sockets_.end() && it->second.pending_read == p) {
					it->second.pending_read = nullptr;
				}
			}
			if (!ok) {
				delete p;
				if (err == ERROR_OPERATION_ABORTED)
					break;
				if (cbs_.on_close)
					cbs_.on_close(fd);
				closesocket((SOCKET)fd);
				std::lock_guard<std::mutex> lk(mtx_);
				sockets_.erase(fd);
				if (cur_conn_ > 0)
					cur_conn_--;
				break;
			}
			if (bytes == 0) {
				delete p;
				if (cbs_.on_close)
					cbs_.on_close(fd);
				closesocket((SOCKET)fd);
				std::lock_guard<std::mutex> lk(mtx_);
				sockets_.erase(fd);
				if (cur_conn_ > 0)
					cur_conn_--;
				break;
			}
			ReadCallback cb;
			char *buf = nullptr;
			size_t sz = 0;
			uint32_t tms = 0;
			HANDLE timer = nullptr;
			{
				std::lock_guard<std::mutex> lk(mtx_);
				auto it = sockets_.find(fd);
				if (it != sockets_.end()) {
					cb = it->second.read_cb;
					buf = it->second.buf;
					sz = (size_t)bytes;
					tms = it->second.timeout_ms;
					timer = it->second.timer;
				}
			}
			if (cb)
				cb(fd, buf, sz);
			if (tms > 0 && timer) {
				ChangeTimerQueueTimer(timer_queue_, timer, tms, 0);
			}
			post_read(fd);
			delete p;
			break;
		}
		case OpType::Write: {
			socket_t fd = p->fd;
			if (!ok) {
				delete p;
				if (err == ERROR_OPERATION_ABORTED)
					break;
				if (cbs_.on_close)
					cbs_.on_close(fd);
				closesocket((SOCKET)fd);
				std::lock_guard<std::mutex> lk(mtx_);
				sockets_.erase(fd);
				if (cur_conn_ > 0)
					cur_conn_--;
				break;
			}
			size_t wrote = bytes;
			delete p;
			if (cbs_.on_write)
				cbs_.on_write(fd, wrote);
			std::lock_guard<std::mutex> lk(mtx_);
			auto it = sockets_.find(fd);
			if (it != sockets_.end()) {
				if (wrote <= it->second.out_queue.size())
					it->second.out_queue.erase(it->second.out_queue.begin(), it->second.out_queue.begin() + wrote);
				if (!it->second.out_queue.empty()) {
					post_write_unlocked(fd, it->second);
				} else {
					it->second.sending = false;
				}
			}
			break;
		}
		}
	}
}

void IocpEngine::post_read(socket_t fd) {
	std::lock_guard<std::mutex> lk(mtx_);
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
	if (st.out_queue.empty())
		return;
	PerIo *p = new PerIo();
	p->op = OpType::Write;
	p->fd = fd;
	p->dynbuf.assign(st.out_queue.begin(), st.out_queue.end());
	p->wbuf.buf = p->dynbuf.data();
	p->wbuf.len = (ULONG)p->dynbuf.size();
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
	accept_listen_ = (SOCKET)listen_fd;
	{
		std::lock_guard<std::mutex> lk(mtx_);
		outstanding_accepts_[listen_fd]++;
		pending_accepts_[listen_fd].push_back(p);
		IO_LOG_DBG("post_accept: listen=%p outstanding=%u", (void *)(uintptr_t)listen_fd,
				   outstanding_accepts_[listen_fd]);
	}
}

bool IocpEngine::set_accept_depth(socket_t listen_socket, uint32_t depth) {
	if (depth == 0)
		depth = 1;
	uint32_t to_post = 0;
	{
		std::lock_guard<std::mutex> lk(mtx_);
		if (listeners_.find(listen_socket) == listeners_.end()) {
			// listener ещё не активирован через accept(); просто запомним цель
			target_accept_depth_[listen_socket] = depth;
			outstanding_accepts_[listen_socket] = 0;
			return true;
		}
		target_accept_depth_[listen_socket] = depth;
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
	{
		std::lock_guard<std::mutex> lk(mtx_);
		if (listeners_.find(listen_socket) == listeners_.end()) {
			target_accept_depth_[listen_socket] = depth;
			outstanding_accepts_[listen_socket] = 0;
			return true;
		}
		target_accept_depth_[listen_socket] = depth;
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
			CancelIoEx((HANDLE)(uintptr_t)listen_socket, &p->ol);
		}
	}
	return true;
}

bool IocpEngine::set_accept_autotune(socket_t listen_socket, const AcceptAutotuneConfig &cfg) {
	std::lock_guard<std::mutex> lk(mtx_);
	if (listeners_.find(listen_socket) == listeners_.end()) {
		// Разрешим настроить заранее; таймер будет создан при accept()
		autotune_cfg_[listen_socket] = cfg;
		return true;
	}
	autotune_cfg_[listen_socket] = cfg;
	if (!timer_queue_)
		timer_queue_ = CreateTimerQueue();
	auto it = autotune_timer_.find(listen_socket);
	if (!cfg.enabled) {
		if (it != autotune_timer_.end() && it->second) {
			DeleteTimerQueueTimer(timer_queue_, it->second, INVALID_HANDLE_VALUE);
			autotune_timer_.erase(it);
		}
		return true;
	}
	if (it == autotune_timer_.end()) {
		HANDLE hTimer = nullptr;
		auto *ctx = new ListenerTimerCtx{this, listen_socket};
		CreateTimerQueueTimer(&hTimer, timer_queue_, &IocpEngine::on_autotune_timer, ctx, cfg.window_ms, cfg.window_ms,
							  WT_EXECUTEDEFAULT);
		autotune_timer_[listen_socket] = hTimer;
	} else {
		ChangeTimerQueueTimer(timer_queue_, it->second, cfg.window_ms, cfg.window_ms);
	}
	return true;
}

VOID CALLBACK IocpEngine::on_socket_timer(PVOID param, BOOLEAN) {
	TimerCtx *ctx = static_cast<TimerCtx *>(param);
	IocpEngine *self = ctx->self;
	socket_t fd = ctx->fd;
	{
		std::lock_guard<std::mutex> lk(self->mtx_);
		auto it = self->sockets_.find(fd);
		if (it != self->sockets_.end()) {
			it->second.timer = nullptr;
			if (it->second.timer_ctx) {
				delete it->second.timer_ctx;
				it->second.timer_ctx = nullptr;
			}
		}
	}
	if (self->cbs_.on_close)
		self->cbs_.on_close(fd);
	shutdown((SOCKET)fd, SD_BOTH);
	closesocket((SOCKET)fd);
	{
		std::lock_guard<std::mutex> lk2(self->mtx_);
		self->sockets_.erase(fd);
		if (self->cur_conn_ > 0)
			self->cur_conn_--;
	}
}

VOID CALLBACK IocpEngine::on_autotune_timer(PVOID param, BOOLEAN) {
	auto *ctx = static_cast<ListenerTimerCtx *>(param);
	IocpEngine *self = ctx->self;
	socket_t lfd = ctx->listen_fd;
	AcceptAutotuneConfig cfg{};
	uint32_t accepted = 0;
	uint32_t curDepth = 0;
	{
		std::lock_guard<std::mutex> lk(self->mtx_);
		if (self->autotune_cfg_.find(lfd) == self->autotune_cfg_.end())
			return;
		cfg = self->autotune_cfg_[lfd];
		if (!cfg.enabled)
			return;
		if (self->listeners_.find(lfd) == self->listeners_.end())
			return;
		accepted = self->accepted_in_window_[lfd];
		self->accepted_in_window_[lfd] = 0;
		curDepth = self->target_accept_depth_[lfd];
	}
	uint32_t next = curDepth;
	if (accepted >= cfg.high_watermark) {
		next = std::min<uint32_t>(cfg.max_depth, curDepth + cfg.up_step);
	} else if (accepted <= cfg.low_watermark) {
		if (curDepth > cfg.min_depth)
			next = std::max<uint32_t>(cfg.min_depth, curDepth - cfg.down_step);
	}
	if (next != curDepth) {
		bool aggressive = (next < curDepth) && cfg.aggressive_cancel_on_downscale;
		self->set_accept_depth_ex(lfd, next, aggressive);
	}
}

INetEngine *create_engine_iocp() {
	return new IocpEngine();
}

} // namespace io

#endif
