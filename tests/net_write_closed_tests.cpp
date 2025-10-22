#include "io/net.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

// These tests verify we don't crash (e.g., via SIGPIPE) when writing to a socket whose peer has already closed.

static void SetupListener(uint16_t &port, io::socket_t &listen_fd) {
  listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
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
  port = ntohs(addr.sin_port);
}

TEST(NetSigpipe, ClientWriteAfterServerClose) {
  using namespace io;
  std::unique_ptr<INetEngine> engine(create_engine());
  ASSERT_NE(engine, nullptr);

  uint16_t port = 0;
  io::socket_t listen_fd = io::kInvalidSocket;
  SetupListener(port, listen_fd);

  // Prepare client fd early so on_accept can filter it out
  io::socket_t client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(client_fd, io::kInvalidSocket);

  std::atomic<bool> server_accepted{false};
  std::atomic<io::socket_t> srv_client{io::kInvalidSocket};
  std::atomic<int> close_count{0};

  io::NetCallbacks cbs{};
  cbs.on_accept = [&](io::socket_t fd) {
    // Filter out listener and client-side fd emitted by connect completion
    if (fd != listen_fd && fd != client_fd) {
      server_accepted = true;
      srv_client.store(fd, std::memory_order_release);
    }
  };
  cbs.on_close = [&](io::socket_t) { close_count++; };

  ASSERT_TRUE(engine->init(cbs));
  ASSERT_TRUE(engine->accept(listen_fd, true, 16));

  // Client
  static thread_local char client_buf[256];
  ASSERT_TRUE(engine->add_socket(client_fd, client_buf, sizeof(client_buf), [&](int, char*, size_t){}));
  ASSERT_TRUE(engine->connect(client_fd, "127.0.0.1", port, true));

  // Wait for accept, then immediately close on server side
  auto t0 = std::chrono::steady_clock::now();
  while (!server_accepted.load()) {
    if (std::chrono::steady_clock::now() - t0 > 2s) {
      FAIL() << "timeout waiting for accept";
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  io::socket_t sfd = srv_client.load(std::memory_order_acquire);
  ASSERT_NE(sfd, io::kInvalidSocket);
  // Close server side to create a closed peer for client
  ::shutdown(sfd, SHUT_RDWR);
  ::close(sfd);

  // Now enqueue a write on the client; engine must not crash
  const char msg[] = "hello-after-close";
  ASSERT_TRUE(engine->write(client_fd, msg, sizeof(msg)));

  // Expect on_close to arrive within a reasonable time window
  auto t1 = std::chrono::steady_clock::now();
  while (close_count.load() == 0) {
    if (std::chrono::steady_clock::now() - t1 > 1s) break;
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_GE(close_count.load(), 1);

  ::close(listen_fd);
  engine->destroy();
}

TEST(NetSigpipe, ServerWriteAfterClientClose) {
  using namespace io;
  std::unique_ptr<INetEngine> engine(create_engine());
  ASSERT_NE(engine, nullptr);

  uint16_t port = 0;
  io::socket_t listen_fd = io::kInvalidSocket;
  SetupListener(port, listen_fd);

  // Prepare client fd early so on_accept can filter it out
  io::socket_t client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(client_fd, io::kInvalidSocket);

  std::atomic<bool> server_accepted{false};
  std::atomic<io::socket_t> srv_client{io::kInvalidSocket};

  std::atomic<int> close_count{0};
  io::NetCallbacks cbs{};
  cbs.on_accept = [&](io::socket_t fd) {
    // Filter out listener and client-side fd emitted by connect completion
    if (fd != listen_fd && fd != client_fd) {
      server_accepted = true;
      srv_client.store(fd, std::memory_order_release);
    }
  };
  cbs.on_close = [&](io::socket_t){ close_count++; };

  ASSERT_TRUE(engine->init(cbs));
  ASSERT_TRUE(engine->accept(listen_fd, true, 16));

  // Client connects and then closes
  static thread_local char client_buf[256];
  ASSERT_TRUE(engine->add_socket(client_fd, client_buf, sizeof(client_buf), [&](int, char*, size_t){}));
  ASSERT_TRUE(engine->connect(client_fd, "127.0.0.1", port, true));

  // Wait for accept, then close client side
  auto t0 = std::chrono::steady_clock::now();
  while (!server_accepted.load()) {
    if (std::chrono::steady_clock::now() - t0 > 2s) {
      FAIL() << "timeout waiting for accept";
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  ::shutdown(client_fd, SHUT_RDWR);
  ::close(client_fd);

  // Register server-side socket and attempt a write after client closed; engine should not crash
  io::socket_t sfd = srv_client.load(std::memory_order_acquire);
  static thread_local char srv_buf[256];
  ASSERT_TRUE(engine->add_socket(sfd, srv_buf, sizeof(srv_buf), [&](int, char*, size_t){}));
  const char payload[] = "server-sends-after-client-close";
  ASSERT_TRUE(engine->write(sfd, payload, sizeof(payload)));

  // Expect on_close to arrive within a reasonable time window
  auto t2 = std::chrono::steady_clock::now();
  while (close_count.load() == 0) {
    if (std::chrono::steady_clock::now() - t2 > 1s) break;
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_GE(close_count.load(), 1);

  ::close(listen_fd);
  engine->destroy();
}
