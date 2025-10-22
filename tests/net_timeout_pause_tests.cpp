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

// Semantics under test:
// - If paused and resume occurs before read-timeout, socket should NOT be closed due to timeout.
// - If paused and timeout elapses without any data read, on_close should be invoked.

namespace {
struct TestRig {
	std::unique_ptr<io::INetEngine> engine;
	io::socket_t listen_fd{io::kInvalidSocket};
	io::socket_t client_fd{io::kInvalidSocket};
	std::atomic<bool> client_connected{false};
	std::atomic<bool> server_connected{false};
	std::atomic<int> client_reads{0};
	std::atomic<int> server_reads{0};
	std::atomic<int> closes{0};
	char client_buf[1024]{};
	char server_buf[1024]{};

	bool setup(io::NetCallbacks &cbs, uint16_t &out_port) {
		engine.reset(io::create_engine());
		if (!engine)
			return false;
		listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (listen_fd == io::kInvalidSocket)
			return false;
		int opt = 1;
		::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (::bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) != 0)
			return false;
		if (::listen(listen_fd, 16) != 0)
			return false;
		socklen_t alen = sizeof(addr);
		if (::getsockname(listen_fd, (sockaddr *)&addr, &alen) != 0)
			return false;
		out_port = ntohs(addr.sin_port);

		client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (client_fd == io::kInvalidSocket)
			return false;

		cbs.on_accept = [&](io::socket_t fd) {
			if (fd == client_fd) {
				client_connected = true;
			} else if (fd != listen_fd) {
				server_connected = true;
				io::INetEngine *eng = engine.get();
				engine->add_socket(fd, server_buf, sizeof(server_buf), [eng, this](int s, char *b, size_t n) {
					(void)eng;
					(void)b;
					if (n > 0) {
						server_reads++;
						eng->write(s, b, n);
					}
				});
			}
		};
		cbs.on_read = [&](io::socket_t, char *, size_t n) { client_reads += (int)n; };
		cbs.on_close = [&](io::socket_t) { closes++; };

		if (!engine->init(cbs))
			return false;
		if (!engine->accept(listen_fd, true, 8))
			return false;
		if (!engine->add_socket(client_fd, client_buf, sizeof(client_buf), cbs.on_read))
			return false;
		if (!engine->connect(client_fd, "127.0.0.1", out_port, true))
			return false;

		auto start = std::chrono::steady_clock::now();
		while (!(client_connected.load() && server_connected.load())) {
			if (std::chrono::steady_clock::now() - start > 2s)
				return false;
			if (!engine->loop_once(5)) std::this_thread::sleep_for(5ms);
		}
		return true;
	}
	void teardown() {
		if (engine) {
			if (client_fd != io::kInvalidSocket)
				engine->disconnect(client_fd);
			if (listen_fd != io::kInvalidSocket)
				::close(listen_fd);
			engine->destroy();
		}
	}
};
} // namespace

TEST(NetTimeoutPause, ResumeBeforeTimeout_NoClose) {
	TestRig rig;
	uint16_t port = 0;
	io::NetCallbacks cbs{};
	ASSERT_TRUE(rig.setup(cbs, port));

	// Set timeout and pause
	ASSERT_TRUE(rig.engine->set_read_timeout(rig.client_fd, 300 /*ms*/));
	ASSERT_TRUE(rig.engine->pause_read(rig.client_fd));

	// Send payload while paused, then resume before timeout expires
	std::string payload(256, 'B');
	ASSERT_TRUE(rig.engine->write(rig.client_fd, payload.data(), payload.size()));

	{
		auto until = std::chrono::steady_clock::now() + 100ms; // still within timeout window
		while (std::chrono::steady_clock::now() < until) {
			if (!rig.engine->loop_once(5)) std::this_thread::sleep_for(5ms);
		}
	}
	ASSERT_TRUE(rig.engine->resume_read(rig.client_fd));

	// Expect data delivered and no close due to timeout
	auto t0 = std::chrono::steady_clock::now();
	while (rig.client_reads.load() < (int)payload.size()) {
		if (std::chrono::steady_clock::now() - t0 > 2s)
			break;
		if (!rig.engine->loop_once(5)) std::this_thread::sleep_for(5ms);
	}
	EXPECT_EQ(rig.closes.load(), 0);
	EXPECT_EQ(rig.client_reads.load(), (int)payload.size());

	rig.teardown();
}

TEST(NetTimeoutPause, ExceedTimeoutWhilePaused_CloseFires) {
	TestRig rig;
	uint16_t port = 0;
	io::NetCallbacks cbs{};
	ASSERT_TRUE(rig.setup(cbs, port));

	// Set short timeout and pause, do NOT resume
	ASSERT_TRUE(rig.engine->set_read_timeout(rig.client_fd, 150 /*ms*/));
	ASSERT_TRUE(rig.engine->pause_read(rig.client_fd));

	// Wait beyond timeout; engine should close the socket due to idle timeout
	{
		auto until = std::chrono::steady_clock::now() + 450ms; // allow timeout and processing
		while (std::chrono::steady_clock::now() < until) {
			if (!rig.engine->loop_once(10)) std::this_thread::sleep_for(10ms);
		}
	}

	EXPECT_GE(rig.closes.load(), 1);

	rig.teardown();
}
