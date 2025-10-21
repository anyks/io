#include "io/net.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

TEST(NetTimeout, ReadIdleCloseClient) {
	using namespace io;
	std::unique_ptr<INetEngine> engine(create_engine());
	ASSERT_NE(engine, nullptr);

	std::atomic<bool> server_connected{false};
	std::atomic<int> close_count{0};

	io::socket_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_NE(listen_fd, io::kInvalidSocket);
	int opt = 1;
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	ASSERT_EQ(::bind(listen_fd, (sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(::listen(listen_fd, 16), 0);
	socklen_t alen = sizeof(addr);
	ASSERT_EQ(::getsockname(listen_fd, (sockaddr *)&addr, &alen), 0);
	uint16_t port = ntohs(addr.sin_port);

	static thread_local char client_buf[1024];
	static thread_local char server_buf[1024];

	std::atomic<io::socket_t> server_client_fd{io::kInvalidSocket};
	io::socket_t client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_NE(client_fd, io::kInvalidSocket);

	io::NetCallbacks cbs{};
	cbs.on_accept = [&](io::socket_t fd) {
		if (fd != listen_fd) {
			server_connected = true;
			server_client_fd.store(fd, std::memory_order_relaxed);
		}
	};
	cbs.on_close = [&](io::socket_t) { close_count++; };

	ASSERT_TRUE(engine->init(cbs));
	ASSERT_TRUE(engine->accept(listen_fd, true, 8));

	// Client side
	std::promise<void> conn_p;
	auto conn_f = conn_p.get_future();
	io::ReadCallback cr = [&](int, char *, size_t) { /* no reads expected */ };
	ASSERT_TRUE(engine->add_socket(client_fd, client_buf, sizeof(client_buf), cr));
	cbs.on_read = [&](int, char *, size_t) {};
	ASSERT_TRUE(engine->connect(client_fd, "127.0.0.1", port, true));

	// Wait for server accept
	auto start = std::chrono::steady_clock::now();
	while (!server_connected.load()) {
		if (std::chrono::steady_clock::now() - start > 2s) {
			FAIL() << "Timeout waiting for server accept";
			break;
		}
		std::this_thread::sleep_for(10ms);
	}

	// Register server side buffer but don't send any data; set timeout 100ms
	io::socket_t srvfd = server_client_fd.load(std::memory_order_acquire);
	ASSERT_TRUE(engine->add_socket(srvfd, server_buf, sizeof(server_buf), [&](int, char *, size_t) {}));
	ASSERT_TRUE(engine->set_read_timeout(srvfd, 100));

	// Wait for close
	start = std::chrono::steady_clock::now();
	while (close_count.load() == 0) {
		if (std::chrono::steady_clock::now() - start > 2s)
			break;
		std::this_thread::sleep_for(10ms);
	}
	EXPECT_GE(close_count.load(), 1);

	::close(listen_fd);
	engine->destroy();
}

TEST(NetTimeout, DisableTimeout) {
	using namespace io;
	std::unique_ptr<INetEngine> engine(create_engine());
	ASSERT_NE(engine, nullptr);

	io::socket_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_NE(listen_fd, io::kInvalidSocket);
	int opt = 1;
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	ASSERT_EQ(::bind(listen_fd, (sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(::listen(listen_fd, 16), 0);
	socklen_t alen = sizeof(addr);
	ASSERT_EQ(::getsockname(listen_fd, (sockaddr *)&addr, &alen), 0);
	uint16_t port = ntohs(addr.sin_port);

	static thread_local char client_buf[1024];
	static thread_local char server_buf[1024];

	std::atomic<io::socket_t> server_client_fd{io::kInvalidSocket};
	io::socket_t client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_NE(client_fd, io::kInvalidSocket);

	std::atomic<bool> server_connected{false};

	io::NetCallbacks cbs{};
	cbs.on_accept = [&](io::socket_t fd) {
		if (fd != listen_fd) {
			server_connected = true;
			server_client_fd.store(fd, std::memory_order_relaxed);
		}
	};

	ASSERT_TRUE(engine->init(cbs));
	ASSERT_TRUE(engine->accept(listen_fd, true, 8));

	// Client side
	ASSERT_TRUE(engine->add_socket(client_fd, client_buf, sizeof(client_buf), [&](int, char *, size_t) {}));
	ASSERT_TRUE(engine->connect(client_fd, "127.0.0.1", port, true));

	// Wait for server accept
	auto start = std::chrono::steady_clock::now();
	while (!server_connected.load()) {
		if (std::chrono::steady_clock::now() - start > 2s) {
			FAIL() << "Timeout waiting for server accept";
			break;
		}
		std::this_thread::sleep_for(10ms);
	}

	io::socket_t srvfd = server_client_fd.load(std::memory_order_acquire);
	ASSERT_TRUE(engine->add_socket(srvfd, server_buf, sizeof(server_buf), [&](int, char *, size_t) {}));
	// Set then disable
	ASSERT_TRUE(engine->set_read_timeout(srvfd, 200));
	ASSERT_TRUE(engine->set_read_timeout(srvfd, 0));

	// Ensure no close within 500ms
	std::atomic<int> close_count{0};
	cbs.on_close = [&](io::socket_t) { close_count++; };
	std::this_thread::sleep_for(600ms);
	EXPECT_EQ(close_count.load(), 0);

	::close(listen_fd);
	engine->destroy();
}
