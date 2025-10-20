#include <gtest/gtest.h>
#include "io/net.hpp"
#include <atomic>
#include <thread>
#include <future>
#include <mutex>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std::chrono_literals;

// Проверяем паузу на стороне сервера (accepted сокет):
// - Пока пауза активна, колбэк чтения сервера не вызывается
// - После resume_read данные доставляются
TEST(NetPauseResumeServer, ServerPauseSuppressesThenResumeDelivers) {
  using namespace io;
  std::unique_ptr<INetEngine> engine(create_engine());
  ASSERT_NE(engine, nullptr);

  std::atomic<bool> client_connected{false};
  std::atomic<bool> server_connected{false};
  std::atomic<int> server_bytes{0};

  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listen_fd, 0);
  int opt=1; ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK); addr.sin_port=0; // ephemeral
  ASSERT_EQ(::bind(listen_fd, (sockaddr*)&addr, sizeof(addr)), 0);
  ASSERT_EQ(::listen(listen_fd, 16), 0);
  socklen_t alen=sizeof(addr); ASSERT_EQ(::getsockname(listen_fd, (sockaddr*)&addr, &alen), 0);
  uint16_t port = ntohs(addr.sin_port);

  static thread_local char client_buf[4096];
  static thread_local char server_buf[4096];

  int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(client_fd, 0);

  std::atomic<int> server_fd{-1};

  io::NetCallbacks cbs{};
  cbs.on_accept = [&](int fd){
    if (fd == client_fd) {
      client_connected = true;
    } else if (fd != listen_fd) {
      server_connected = true;
      server_fd = fd;
      // Назначаем обработчик чтения на серверном сокете
      engine->add_socket(fd, server_buf, sizeof(server_buf), [&](int s, char* b, size_t n){
        (void)s; (void)b; server_bytes += (int)n;
      });
      // Немедленно ставим паузу на чтение серверного сокета
      engine->pause_read(fd);
    }
  };
  // Клиентский on_read не нужен для этой проверки
  cbs.on_read = [&](int, char*, size_t){ };

  ASSERT_TRUE(engine->init(cbs));
  ASSERT_TRUE(engine->accept(listen_fd, true, 8));
  ASSERT_TRUE(engine->add_socket(client_fd, client_buf, sizeof(client_buf), cbs.on_read));
  ASSERT_TRUE(engine->connect(client_fd, "127.0.0.1", port, true));

  // Ждём коннектов
  auto start = std::chrono::steady_clock::now();
  while (!(client_connected.load() && server_connected.load())) {
    if (std::chrono::steady_clock::now() - start > 2s) {
      FAIL() << "Timeout waiting for connections";
      break;
    }
    std::this_thread::sleep_for(10ms);
  }

  // Пока пауза активна на сервере, шлём данные клиентом
  const std::string payload = std::string(1024, 'S');
  ASSERT_TRUE(engine->write(client_fd, payload.data(), payload.size()));
  std::this_thread::sleep_for(100ms);
  // Проверяем, что байты не доставлены на сервере
  EXPECT_EQ(server_bytes.load(), 0);

  // Снимаем паузу на серверном сокете
  ASSERT_GE(server_fd.load(), 0);
  ASSERT_TRUE(engine->resume_read(server_fd.load()));

  // Ожидаем доставку
  auto t0 = std::chrono::steady_clock::now();
  while (server_bytes.load() < (int)payload.size()) {
    if (std::chrono::steady_clock::now() - t0 > 2s) break;
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_EQ(server_bytes.load(), (int)payload.size());

  // Cleanup
  engine->disconnect(client_fd);
  ::close(listen_fd);
  engine->destroy();
}
