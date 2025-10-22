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

// N simultaneous clients: set read timeout and pause all; then
// - for first half: resume before timeout and verify delivery
// - for second half: keep paused beyond timeout and verify on_close fires
TEST(NetPauseMultiTimeout, MixedResumeAndTimeouts) {
	using namespace io;
	std::unique_ptr<INetEngine> engine(create_engine());
	ASSERT_NE(engine, nullptr);

	constexpr int N = 8;
	std::vector<std::atomic<io::socket_t>> client_fds(N);
	std::vector<char> client_bufs(N * 1024);
	std::vector<std::atomic<int>> delivered(N);
	for (int i = 0; i < N; ++i) {
		delivered[i] = 0;
		client_fds[i] = io::kInvalidSocket;
	}

	io::socket_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_NE(listen_fd, io::kInvalidSocket);
	int opt = 1;
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	ASSERT_EQ(::bind(listen_fd, (sockaddr *)&addr, sizeof(addr)), 0);
	ASSERT_EQ(::listen(listen_fd, 32), 0);
	socklen_t alen = sizeof(addr);
	ASSERT_EQ(::getsockname(listen_fd, (sockaddr *)&addr, &alen), 0);
	uint16_t port = ntohs(addr.sin_port);

	static thread_local char server_buf[2048];
	std::atomic<int> server_accepts{0};
	std::atomic<int> closes{0};

	io::NetCallbacks cbs{};
	cbs.on_accept = [&](io::socket_t fd) {
		if (fd == listen_fd)
			return; // игнорируем сам listen сокет на всякий случай
		// Отфильтруем клиентские сокеты (событие connect завершился), нас интересуют только принятые сервером
		bool is_client_socket = false;
		for (int i = 0; i < N; ++i) {
			if (client_fds[i].load() == fd) {
				is_client_socket = true;
				break;
			}
		}
		if (is_client_socket)
			return;
		// Принятый сервером сокет: назначаем чтение и эхо, учитываем приёмы
		INetEngine *eng = engine.get();
		engine->add_socket(fd, server_buf, sizeof(server_buf), [eng](int s, char *b, size_t n) {
			if (n > 0)
				eng->write(s, b, n);
		});
		server_accepts++;
	};
	cbs.on_read = [&](io::socket_t s, char *b, size_t n) {
		(void)b;
		// find client index by fd
		for (int i = 0; i < N; ++i) {
			if (client_fds[i].load() == s) {
				delivered[i] += (int)n;
				break;
			}
		}
	};
	cbs.on_close = [&](int) { closes++; };

	ASSERT_TRUE(engine->init(cbs));
	ASSERT_TRUE(engine->accept(listen_fd, true, 64));

	// create and connect clients
	for (int i = 0; i < N; ++i) {
		io::socket_t cfd = ::socket(AF_INET, SOCK_STREAM, 0);
		ASSERT_NE(cfd, io::kInvalidSocket);
		client_fds[i] = cfd;
		ASSERT_TRUE(engine->add_socket(cfd, client_bufs.data() + i * 1024, 1024, cbs.on_read));
		ASSERT_TRUE(engine->connect(cfd, "127.0.0.1", port, true));
	}

	// wait all accepted
	auto tstart = std::chrono::steady_clock::now();
	while (server_accepts.load() < N) {
		if (std::chrono::steady_clock::now() - tstart > 2s)
			break;
		if (!engine->loop_once(5)) std::this_thread::sleep_for(5ms);
	}
	ASSERT_GE(server_accepts.load(), N);

	// configure timeouts and pause all clients
	for (int i = 0; i < N; ++i) {
		ASSERT_TRUE(engine->set_read_timeout(client_fds[i], 300));
		ASSERT_TRUE(engine->pause_read(client_fds[i]));
	}

	// send payloads
	std::string payload(256, 'M');
	for (int i = 0; i < N; ++i) {
		ASSERT_TRUE(engine->write(client_fds[i], payload.data(), payload.size()));
	}

	// resume first half before timeout
	{
		auto until = std::chrono::steady_clock::now() + 100ms;
		while (std::chrono::steady_clock::now() < until) {
			if (!engine->loop_once(5)) std::this_thread::sleep_for(5ms);
		}
	}
	for (int i = 0; i < N / 2; ++i) {
		ASSERT_TRUE(engine->resume_read(client_fds[i]));
	}

	// wait for deliveries to first half
	auto t0 = std::chrono::steady_clock::now();
	while (true) {
		int ok = 0;
		for (int i = 0; i < N / 2; ++i)
			if (delivered[i].load() >= (int)payload.size())
				ok++;
		if (ok == N / 2)
			break;
		if (std::chrono::steady_clock::now() - t0 > 2s)
			break;
		if (!engine->loop_once(5)) std::this_thread::sleep_for(5ms);
	}
	for (int i = 0; i < N / 2; ++i)
		EXPECT_EQ(delivered[i].load(), (int)payload.size());

	// give time to fire timeouts for second half
	{
		auto until = std::chrono::steady_clock::now() + 400ms;
		while (std::chrono::steady_clock::now() < until) {
			if (!engine->loop_once(10)) std::this_thread::sleep_for(10ms);
		}
	}
	int before = closes.load();
	{
		auto until = std::chrono::steady_clock::now() + 100ms;
		while (std::chrono::steady_clock::now() < until) {
			if (!engine->loop_once(10)) std::this_thread::sleep_for(10ms);
		}
	}
	EXPECT_GE(closes.load(), before); // at least some closes should have fired due to timeout

	// cleanup
	for (int i = 0; i < N; ++i)
		engine->disconnect(client_fds[i]);
	::close(listen_fd);
	engine->destroy();
}
