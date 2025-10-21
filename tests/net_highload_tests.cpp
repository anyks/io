#include "io/net.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

namespace {
struct ClientState {
	io::socket_t fd{io::kInvalidSocket};
	std::unique_ptr<char[]> buf;
	std::atomic<bool> connected{false};
	std::atomic<bool> echoed{false};
	std::atomic<bool> closed{false};
};
} // namespace

TEST(NetHighload, ManyClientsEchoNoBlock) {
	using namespace io;
	// Read sizes from environment (with safe defaults)
	int N = 200;  // clients
	int M = 5000; // user events
	if (const char *s = std::getenv("IO_STRESS_N")) {
		int v = std::atoi(s);
		if (v > 0)
			N = v;
	}
	if (const char *s = std::getenv("IO_STRESS_M")) {
		int v = std::atoi(s);
		if (v > 0)
			M = v;
	}

	// Bound N by file descriptor soft limit to prevent exhaustion (approx. 2 fds per client + listen)
	rlimit rl{};
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
		long max_fd = (long)rl.rlim_cur;
		if (max_fd <= 128)
			max_fd = 128;
		long budget = max_fd - 64;
		if (budget < 64)
			budget = 64;
		long max_clients = budget / 2;
		if (N > max_clients)
			N = (int)max_clients;
		if (N < 1)
			N = 1;
	}

	// Server listen socket
	io::socket_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_NE(listen_fd, io::kInvalidSocket);
	int opt = 1;
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	ASSERT_EQ(::bind(listen_fd, (sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(::listen(listen_fd, std::max(1024, N * 2)), 0);
	socklen_t alen = sizeof(addr);
	ASSERT_EQ(::getsockname(listen_fd, (sockaddr *)&addr, &alen), 0);
	uint16_t port = ntohs(addr.sin_port);

	// Track server-side buffers to free them on close
	std::mutex srv_mtx;
	std::unordered_map<int, std::unique_ptr<char[]>> server_buffers;

	// Client states
	std::vector<ClientState> clients(N);
	std::unordered_set<io::socket_t> client_fds;
	client_fds.reserve(N);
	std::mutex client_fds_mtx;
	std::unordered_map<io::socket_t, int> fd_to_index;

	std::atomic<int> connected_clients{0};
	std::atomic<int> echoed_clients{0};
	std::atomic<int> server_accepts{0};
	std::atomic<int> close_count{0};
	std::atomic<uint64_t> user_count{0};

	// Place the engine and callbacks into an inner scope so the engine is destroyed
	// before the containers captured by callbacks, even on early EXPECT failures.
	{
		std::unique_ptr<INetEngine> engine(create_engine());
		ASSERT_NE(engine, nullptr);

		io::NetCallbacks cbs{};
		cbs.on_accept = [&](io::socket_t fd) {
			// If this is a server-accepted socket (not in client set and not the listen fd), start echoing
			bool is_client_fd = false;
			{
				std::lock_guard<std::mutex> lk(client_fds_mtx);
				is_client_fd = client_fds.find(fd) != client_fds.end();
			}
			if (fd != listen_fd && !is_client_fd) {
				server_accepts++;
				auto buf = std::make_unique<char[]>(4096);
				char *raw = buf.get();
				{
					std::lock_guard<std::mutex> lk(srv_mtx);
					server_buffers.emplace(fd, std::move(buf));
				}
				engine->add_socket(fd, raw, 4096, [&, fd](int s, char *b, size_t n) {
					(void)s;
					engine->write(fd, b, n);
				});
			} else {
				// client side connected
				connected_clients++;
				int idx = -1;
				{
					std::lock_guard<std::mutex> lk(client_fds_mtx);
					auto it = fd_to_index.find(fd);
					if (it != fd_to_index.end())
						idx = it->second;
				}
				if (idx >= 0 && idx < (int)clients.size()) {
					clients[idx].connected.store(true, std::memory_order_relaxed);
				}
			}
		};
		cbs.on_close = [&](io::socket_t fd) {
			close_count++;
			// Track client-side closes to recover failed in-flight connects
			{
				std::lock_guard<std::mutex> lk(client_fds_mtx);
				auto it = fd_to_index.find(fd);
				if (it != fd_to_index.end()) {
					int idx = it->second;
					if (idx >= 0 && idx < (int)clients.size()) {
						clients[idx].closed.store(true, std::memory_order_relaxed);
					}
				}
			}
			std::lock_guard<std::mutex> lk2(srv_mtx);
			server_buffers.erase(fd);
		};
		cbs.on_user = [&](uint32_t) { user_count++; };

		ASSERT_TRUE(engine->init(cbs));
		ASSERT_TRUE(engine->accept(listen_fd, true, 2000));

		// Create clients
		for (int i = 0; i < N; ++i) {
			io::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
			ASSERT_NE(fd, io::kInvalidSocket);
			clients[i].fd = fd;
			clients[i].buf = std::make_unique<char[]>(2048);
			{
				std::lock_guard<std::mutex> lk(client_fds_mtx);
				client_fds.insert(fd);
				fd_to_index.emplace(fd, i);
			}
			ASSERT_TRUE(engine->add_socket(fd, clients[i].buf.get(), 2048, [&, i](io::socket_t s, char *b, size_t n) {
				(void)s;
				(void)b;
				(void)n;
				if (!clients[i].echoed.exchange(true))
					echoed_clients++;
			}));
		}

		// Connect all clients async
		for (int i = 0; i < N; ++i) {
			bool ok = false;
			int attempts = 0;
			// Allow a bit more time on macOS to avoid transient EADDRNOTAVAIL spikes on loopback
			auto deadline = std::chrono::steady_clock::now() + 3000ms;
			while (std::chrono::steady_clock::now() < deadline && !ok) {
				clients[i].closed.store(false, std::memory_order_relaxed);
				ok = engine->connect(clients[i].fd, "127.0.0.1", port, true);
				if (!ok) {
					attempts++;
					// Backoff slightly to give kernel time to allocate an ephemeral port
					std::this_thread::sleep_for(5ms);
					// Only recreate the socket occasionally; otherwise retry on the same fd
					if (attempts % 5 == 0) {
						// Try to recover: remove and recreate the socket, update mappings.
						io::socket_t badfd = clients[i].fd;
						engine->delete_socket(badfd);
						::close(badfd);
						io::socket_t newfd = ::socket(AF_INET, SOCK_STREAM, 0);
						if (newfd == io::kInvalidSocket) {
							std::this_thread::sleep_for(5ms);
							continue;
						}
						clients[i].fd = newfd;
						clients[i].buf = std::make_unique<char[]>(2048);
						{
							std::lock_guard<std::mutex> lk(client_fds_mtx);
							// Remove stale entries for the old fd to prevent misclassification
							client_fds.erase(badfd);
							fd_to_index.erase(badfd);
							client_fds.insert(newfd);
							fd_to_index.emplace(newfd, i);
						}
						engine->add_socket(newfd, clients[i].buf.get(), 2048,
										   [&, i](io::socket_t s, char *b, size_t n) {
											   (void)s;
											   (void)b;
											   (void)n;
											   if (!clients[i].echoed.exchange(true))
												   echoed_clients++;
										   });
					}
				} else {
					// Wait a bit for the async connect to complete (on_accept) or fail (on_close)
					auto wait_until = std::chrono::steady_clock::now() + 200ms;
					while (std::chrono::steady_clock::now() < wait_until &&
						   !clients[i].connected.load(std::memory_order_relaxed) &&
						   !clients[i].closed.load(std::memory_order_relaxed)) {
						std::this_thread::sleep_for(2ms);
					}
					if (!clients[i].connected.load(std::memory_order_relaxed)) {
						// Treat as failed attempt if it closed or still not connected
						ok = false;
					}
				}
			}
			EXPECT_TRUE(ok) << "connect failed for fd=" << clients[i].fd;
			// Stagger clients slightly to avoid bursty connect storms
			std::this_thread::sleep_for(1ms);
		}

		// Wait for all clients connected
		auto t0 = std::chrono::steady_clock::now();
		while (connected_clients.load() < N) {
			if (std::chrono::steady_clock::now() - t0 > 10s) {
				FAIL() << "Timeout waiting for clients to connect: " << connected_clients.load() << "/" << N;
				break;
			}
			std::this_thread::sleep_for(5ms);
		}

		// Flood a lot of user events (to test post path under pressure)
		for (int i = 0; i < M; ++i)
			engine->post((uint32_t)(i + 1));

		// Send data from each client
		const char *payload = "x";
		for (int i = 0; i < N; ++i)
			ASSERT_TRUE(engine->write(clients[i].fd, payload, 1));

		// Wait for all echoes and user events to be delivered
		auto t1 = std::chrono::steady_clock::now();
		while (echoed_clients.load() < N || user_count.load() < (uint64_t)M) {
			if (std::chrono::steady_clock::now() - t1 > 15s) {
				ADD_FAILURE() << "Timeout: echoed=" << echoed_clients.load() << "/" << N
							  << ", user_count=" << user_count.load() << "/" << M;
				break;
			}
			std::this_thread::sleep_for(5ms);
		}

		EXPECT_EQ(echoed_clients.load(), N);
		EXPECT_EQ(user_count.load(), (uint64_t)M);
		EXPECT_GE(server_accepts.load(), N); // at least N accepts

		// Cleanup: remove client sockets from the engine first so the engine won't
		// call read callbacks that capture test-local containers while those
		// containers are being destroyed. Then close fds and destroy the engine.
		for (int i = 0; i < N; ++i)
			engine->delete_socket(clients[i].fd);
		for (int i = 0; i < N; ++i)
			engine->disconnect(clients[i].fd);
		std::this_thread::sleep_for(50ms);
		::close(listen_fd);
		engine->destroy();

		// Now that the engine thread has been joined, clear mappings under lock
		// to avoid races between callbacks (which lock the same mutex) and
		// destruction of these containers.
		{
			std::lock_guard<std::mutex> lk(client_fds_mtx);
			client_fds.clear();
			fd_to_index.clear();
		}
	} // engine scope ends before containers are destroyed

	// Server-side buffers map should be empty (no leaks tracked here)
	{
		std::lock_guard<std::mutex> lk(srv_mtx);
		EXPECT_TRUE(server_buffers.empty());
	}
}
