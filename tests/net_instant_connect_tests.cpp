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

// Проверяем кейс мгновенного успешного connect(r==0):
// 1) add_socket(client_fd, buf, cb)
// 2) connect(async=false)
// 3) сервер, приняв соединение, отправляет данные
// 4) клиентский on_read получает данные — буфер/колбэк не потерялись
TEST(NetInstantConnect, AddSocketBeforeConnect_BufferAndCallbackSurvive) {
	using namespace io;
	std::unique_ptr<INetEngine> engine(create_engine());
	ASSERT_NE(engine, nullptr);

	// Поднимаем сервер на локальном порту
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

	// Буферы
	static thread_local char client_buf[2048];
	static thread_local char server_buf[2048];

	// Клиент и серверные флаги
	std::atomic<bool> client_connected{false};
	std::atomic<bool> server_accepted{false};
	std::atomic<io::socket_t> server_fd{io::kInvalidSocket};

	// Для валидации доставки на клиента
	std::promise<void> delivered_promise;
	auto delivered_future = delivered_promise.get_future();
	std::atomic<bool> delivered_set{false};
	std::string payload(256, 'I');

	io::socket_t client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_NE(client_fd, io::kInvalidSocket);

	io::NetCallbacks cbs{};
	cbs.on_accept = [&](io::socket_t fd) {
		if (fd == client_fd) {
			client_connected = true;
			return;
		}
		if (fd == listen_fd)
			return; // игнорируем сам listen
		// Принятый сервером сокет
		server_accepted = true;
		server_fd = fd;
		// Назначим обработчик (хоть и не обязателен — будем только писать)
		engine->add_socket(fd, server_buf, sizeof(server_buf), [&](io::socket_t, char *, size_t) { /*noop*/ });
		// Отправим полезную нагрузку клиенту
		engine->write(fd, payload.data(), payload.size());
	};
	cbs.on_read = [&](io::socket_t s, char *b, size_t n) {
		(void)s;
		if (n >= payload.size() && std::string(b, b + payload.size()) == payload) {
			if (!delivered_set.exchange(true))
				delivered_promise.set_value();
		}
	};

	ASSERT_TRUE(engine->init(cbs));
	ASSERT_TRUE(engine->accept(listen_fd, true, 8));

	// ВАЖНО: сначала добавляем сокет и колбэк
	ASSERT_TRUE(engine->add_socket(client_fd, client_buf, sizeof(client_buf), cbs.on_read));
	// Затем выполняем connect с async=false, чтобы попасть в путь мгновенного успеха
	ASSERT_TRUE(engine->connect(client_fd, "127.0.0.1", port, /*async=*/false));

	// Ждём и клиентский коннект, и серверный accept
	auto t0 = std::chrono::steady_clock::now();
	while (!(client_connected.load() && server_accepted.load())) {
		if (std::chrono::steady_clock::now() - t0 > 2s) {
			FAIL() << "Timeout waiting for client/server connect";
			break;
		}
		std::this_thread::sleep_for(10ms);
	}

	// Ожидаем доставку на клиента
	ASSERT_EQ(delivered_future.wait_for(2s), std::future_status::ready);

	// Cleanup
	engine->disconnect(client_fd);
	io::socket_t sfd = server_fd.load();
	if (sfd != io::kInvalidSocket)
		engine->disconnect(sfd);
	::close(listen_fd);
	engine->destroy();
}
