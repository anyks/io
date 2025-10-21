#include "io/net.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

namespace {
static io::socket_t make_listen(uint16_t &out_port) {
	io::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
	int opt = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	::bind(fd, (sockaddr *)&addr, sizeof(addr));
	::listen(fd, 256);
	socklen_t alen = sizeof(addr);
	::getsockname(fd, (sockaddr *)&addr, &alen);
	out_port = ntohs(addr.sin_port);
	return fd;
}
} // namespace

TEST(NetLimits, RandomChurnConnectDisconnect) {
	using namespace io;
	std::unique_ptr<INetEngine> engine(create_engine());
	ASSERT_NE(engine, nullptr);

	std::atomic<int> accepts{0};
	std::atomic<int> closes{0};
	auto srv_mtx = std::make_shared<std::mutex>();
	auto srv_bufs = std::make_shared<std::unordered_map<io::socket_t, std::unique_ptr<char[]>>>();
	std::unordered_set<io::socket_t> client_set;
	std::mutex client_mtx;

	NetCallbacks cbs{};
	cbs.on_accept = [&](io::socket_t fd) {
		std::lock_guard<std::mutex> lk(client_mtx);
		if (client_set.count(fd) == 0) {
			accepts++;
			auto buf = std::make_unique<char[]>(1024);
			char *raw = buf.get();
			{
				std::lock_guard<std::mutex> ls(*srv_mtx);
				srv_bufs->emplace(fd, std::move(buf));
			}
			engine->add_socket(fd, raw, 1024, [&, fd](io::socket_t, char *b, size_t n) { engine->write(fd, b, n); });
		}
	};
	cbs.on_close = [srv_mtx, srv_bufs, &closes](io::socket_t fd) {
		closes++;
		std::lock_guard<std::mutex> ls(*srv_mtx);
		srv_bufs->erase(fd);
	};

	ASSERT_TRUE(engine->init(cbs));

	uint16_t port = 0;
	io::socket_t listen_fd = make_listen(port);
	ASSERT_TRUE(engine->accept(listen_fd, true, 0));

	// churn loop: create/close clients quickly
	const int iterations = 200;
	for (int i = 0; i < iterations; ++i) {
		io::socket_t cfd = ::socket(AF_INET, SOCK_STREAM, 0);
		ASSERT_NE(cfd, io::kInvalidSocket);
		{
			std::lock_guard<std::mutex> lk(client_mtx);
			client_set.insert(cfd);
		}
		char buf[512];
		ASSERT_TRUE(engine->add_socket(cfd, buf, sizeof(buf), [&](io::socket_t, char *, size_t) { /* ignore */ }));
		// Retry connect briefly to tolerate transient EINPROGRESS/EALREADY behavior under heavy churn/TSan
		bool connected = engine->connect(cfd, "127.0.0.1", port, true);
		if (!connected) {
			for (int r = 0; r < 3 && !connected; ++r) {
				std::this_thread::sleep_for(1ms);
				connected = engine->connect(cfd, "127.0.0.1", port, true);
			}
		}
		ASSERT_TRUE(connected);
		// short activity
		engine->write(cfd, "a", 1);
		std::this_thread::sleep_for(2ms);
		engine->disconnect(cfd);
		{
			std::lock_guard<std::mutex> lk(client_mtx);
			client_set.erase(cfd);
		}
	}

	// wait a bit for loop to drain
	std::this_thread::sleep_for(200ms);
	::close(listen_fd);
	engine->destroy();

	// ensure server-side buffers cleaned
	{
		std::lock_guard<std::mutex> lk(*srv_mtx);
		EXPECT_TRUE(srv_bufs->empty());
	}

	// sanity: at least some accepts occurred, and closes >= accepts
	EXPECT_GE(accepts.load(), 1);
	EXPECT_GE(closes.load(), accepts.load());
}

TEST(NetLimits, EnforceMaxConnections) {
	using namespace io;
	std::unique_ptr<INetEngine> engine(create_engine());
	ASSERT_NE(engine, nullptr);

	const uint32_t maxC = 16;
	std::atomic<int> server_accepts{0};
	std::unordered_set<io::socket_t> client_fds;
	std::mutex cm;
	NetCallbacks cbs{};
	cbs.on_accept = [&](io::socket_t fd) {
		std::lock_guard<std::mutex> lk(cm);
		if (client_fds.count(fd) == 0)
			server_accepts++;
	};
	ASSERT_TRUE(engine->init(cbs));

	uint16_t port = 0;
	io::socket_t listen_fd = make_listen(port);
	ASSERT_TRUE(engine->accept(listen_fd, true, maxC));

	// First wave: open maxC clients and keep them connected for a moment
	std::vector<int> wave1;
	for (uint32_t i = 0; i < maxC; ++i) {
		io::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
		ASSERT_NE(fd, io::kInvalidSocket);
		{
			std::lock_guard<std::mutex> lk(cm);
			client_fds.insert(fd);
		}
		char buf[256];
		engine->add_socket(fd, buf, sizeof(buf), nullptr);
		ASSERT_TRUE(engine->connect(fd, "127.0.0.1", port, true));
		wave1.push_back(fd);
	}
	std::this_thread::sleep_for(100ms);
	EXPECT_EQ(server_accepts.load(), (int)maxC);

	// Second wave: try to exceed limit
	const int extra = 32;
	std::vector<int> wave2;
	for (int i = 0; i < extra; ++i) {
		int fd = ::socket(AF_INET, SOCK_STREAM, 0);
		ASSERT_GE(fd, 0);
		{
			std::lock_guard<std::mutex> lk(cm);
			client_fds.insert(fd);
		}
		char buf[256];
		engine->add_socket(fd, buf, sizeof(buf), nullptr);
		engine->connect(fd, "127.0.0.1", port, true);
		wave2.push_back(fd);
	}
	std::this_thread::sleep_for(100ms);
	// Limit should keep accepts at maxC while first wave is connected
	EXPECT_EQ(server_accepts.load(), (int)maxC);

	// Cleanup wave1 and allow wave2 to fill in up to maxC
	for (io::socket_t fd : wave1)
		engine->disconnect(fd);
	std::this_thread::sleep_for(150ms);
	// server_accepts should be between maxC and 2*maxC depending on timing
	EXPECT_LE(server_accepts.load(), (int)(2 * maxC));
	EXPECT_GE(server_accepts.load(), (int)maxC);

	for (io::socket_t fd : wave2)
		engine->disconnect(fd);
	::close(listen_fd);
	engine->destroy();
}
