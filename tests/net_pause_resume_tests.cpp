#include "io/net.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

// This test verifies per-socket pause_read/resume_read semantics:
// - When paused, inbound data doesn't trigger on_read
// - After resume, buffered data is delivered
TEST(NetPauseResume, PauseSuppressesThenResumeDelivers) {
	using namespace io;
	std::unique_ptr<INetEngine> engine(create_engine());
	ASSERT_NE(engine, nullptr);

	std::atomic<bool> client_connected{false};
	std::atomic<bool> server_connected{false};
	std::atomic<int> client_reads{0};
	std::atomic<int> server_reads{0};

	io::socket_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_NE(listen_fd, io::kInvalidSocket);
	int opt = 1;
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0; // ephemeral
	ASSERT_EQ(::bind(listen_fd, (sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(::listen(listen_fd, 16), 0);
	socklen_t alen = sizeof(addr);
	ASSERT_EQ(::getsockname(listen_fd, (sockaddr *)&addr, &alen), 0);
	uint16_t port = ntohs(addr.sin_port);

	static thread_local char client_buf[4096];
	static thread_local char server_buf[4096];

	io::socket_t client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_NE(client_fd, io::kInvalidSocket);

	std::atomic<int> delivered_bytes{0};
	std::promise<void> got_after_resume;
	auto fut_after_resume = got_after_resume.get_future();

	io::NetCallbacks cbs{};
	cbs.on_accept = [&](io::socket_t fd) {
		if (fd == client_fd) {
			client_connected = true;
		} else if (fd != listen_fd) {
			server_connected = true;
			// server side: read and immediately echo back
			INetEngine *eng = engine.get();
			engine->add_socket(fd, server_buf, sizeof(server_buf),
							   [eng, &server_reads](io::socket_t s, char *b, size_t n) {
								   (void)eng;
								   server_reads++;
								   if (n > 0) {
									   eng->write(s, b, n);
								   }
							   });
		}
	};
	cbs.on_read = [&](io::socket_t s, char *b, size_t n) {
		(void)s;
		(void)b;
		delivered_bytes += (int)n;
		client_reads++;
	};

	ASSERT_TRUE(engine->init(cbs));
	ASSERT_TRUE(engine->accept(listen_fd, true, 8));
	ASSERT_TRUE(engine->add_socket(client_fd, client_buf, sizeof(client_buf), cbs.on_read));
	ASSERT_TRUE(engine->connect(client_fd, "127.0.0.1", port, true));

	auto start = std::chrono::steady_clock::now();
	while (!(client_connected.load() && server_connected.load())) {
		if (std::chrono::steady_clock::now() - start > 2s)
			FAIL() << "Timeout waiting for connections";
		if (!engine->loop_once(5)) std::this_thread::sleep_for(5ms);
	}

	// Pause reads on client socket before sending data
	ASSERT_TRUE(engine->pause_read(client_fd));

	// Send from client to server; server will echo back to client (client paused)
	const std::string payload = std::string(1024, 'A');
	ASSERT_TRUE(engine->write(client_fd, payload.data(), payload.size()));

	// Wait a bit to ensure echo would arrive if not paused
	{
		auto until = std::chrono::steady_clock::now() + 100ms;
		while (std::chrono::steady_clock::now() < until) {
			if (!engine->loop_once(5)) std::this_thread::sleep_for(5ms);
		}
	}
	EXPECT_EQ(client_reads.load(), 0) << "Client should not receive while paused";
	EXPECT_EQ(delivered_bytes.load(), 0);

	// Resume and expect immediate delivery
	ASSERT_TRUE(engine->resume_read(client_fd));

	// Wait for delivery
	auto t0 = std::chrono::steady_clock::now();
	while (delivered_bytes.load() < (int)payload.size()) {
		if (std::chrono::steady_clock::now() - t0 > 2s)
			break;
		if (!engine->loop_once(5)) std::this_thread::sleep_for(5ms);
	}
	EXPECT_EQ(delivered_bytes.load(), (int)payload.size());
	EXPECT_GT(client_reads.load(), 0);

	// Cleanup
	engine->disconnect(client_fd);
	::close(listen_fd);
	engine->destroy();
}
